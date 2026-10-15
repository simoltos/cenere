#!/usr/bin/env python3
"""Install from an actual ISO into a disposable QEMU disk, then boot that disk.

Use --installer only for development against an older ISO; its exact executable
is frozen and hashed in the run directory. Test passwords are generated solely
in memory. The ISO is detached for installed-system boot.
"""

import argparse
import atexit
import base64
import hashlib
import json
import re
from pathlib import Path
import secrets
import shutil
import subprocess
import time

from vm import VM_ROOT, VM, RepositoryServer, artifact, begin_scope, check_repository, clone_disk
from fixtures import assert_home_preserved, existing_system_snapshot, home_snapshot


def answers(password, encrypted=False, existing=False, alongside=True, preserve=True):
    result = ["us", "keyboard test"]
    if existing:
        result += ["keep" if alongside else "replace", "yes" if preserve else "no"]
    result += ["yes" if encrypted else "no", "en_US.UTF-8", "UTC", "anser-vm",
               "vmuser", password, password, "INSTALL"]
    return "\n".join(result) + "\n"


def check_password(vm, password, username="vmuser"):
    if not re.fullmatch(r"[a-z_][a-z0-9_-]{0,31}", username):
        raise ValueError("Unsupported test account name")
    encoded = base64.b64encode(password.encode()).decode()
    return vm.command(r'''python3 - <<'PASSWORD_CHECK'
import base64, ctypes, ctypes.util
accounts = {line.split(":")[0]: line.split(":")[1] for line in open("/etc/shadow")}
assert accounts["root"].startswith(("!", "*")), "Installed root account is not locked"
library = ctypes.CDLL(ctypes.util.find_library("crypt"))
library.crypt.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
library.crypt.restype = ctypes.c_char_p
hashed = accounts["ACCOUNT_USER"].encode()
assert library.crypt(base64.b64decode("PASSWORD_BASE64"), hashed) == hashed, "User password differs"
print("User password matches; root account is locked.")
PASSWORD_CHECK'''.replace("PASSWORD_BASE64", encoded).replace("ACCOUNT_USER", username))


def check_installation_log(vm, password, path):
    if path not in ("/var/log/cenere/latest.log", "/var/log/cenere/installation.log"):
        raise ValueError("Unsupported installation log path")
    encoded = base64.b64encode(password.encode()).decode()
    return vm.command(r'''python3 - <<'LOG_CHECK'
import base64, hashlib, json, os, re, stat
path = "LOG_PATH"
data = open(path, "rb").read()
secret = base64.b64decode("PASSWORD_BASE64")
assert secret not in data and base64.b64encode(secret) not in data, "Private password appeared in log"
assert stat.S_IMODE(os.stat(path).st_mode) == 0o600, "Installation log is not private"
assert b"Copying the Anser system" in data, "System-transfer stage is missing"
assert b"Running /usr/bin/rsync" in data and b"Finished /usr/bin/rsync: exit 0" in data
assert b"System setup completed" in data, "Completed setup is missing from log"
stages = {}
stage = "initialization"
for line in data.decode("utf-8", errors="replace").splitlines():
    status = re.match(r"\[[0-9-]+ [0-9:]+ UTC\] (.*)", line)
    if status and "still running" not in status[1]:
        stage = status[1]
    finished = re.match(r"Finished (/\S+): exit (-?[0-9]+) \(([0-9]+) seconds\)", line)
    if finished:
        entry = stages.setdefault(stage, {"command_seconds": 0, "commands": 0, "tools": {}})
        seconds = int(finished[3])
        entry["command_seconds"] += seconds
        entry["commands"] += 1
        tool = finished[1]
        entry["tools"][tool] = entry["tools"].get(tool, 0) + seconds
print(json.dumps({"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                  "password_absent": True, "mode": "0600", "stages": stages}))
LOG_CHECK'''.replace("LOG_PATH", path).replace("PASSWORD_BASE64", encoded))


def check_live_desktop(vm):
    output = vm.command(r'''set -eu
for attempt in $(seq 1 60); do
    if pgrep -u liveuser -x gnome-shell > /dev/null; then break; fi
    sleep 1
done
systemctl is-active graphical.target gdm
pgrep -a -u liveuser -x gnome-shell
pacman -Q gnome-tiling opendoas
grep -qx 'Exec=ghostty --gtk-single-instance=false --wait-after-command=true -e doas /usr/bin/cenere' /usr/share/applications/cenere.desktop
loginctl list-sessions --no-pager
journalctl -b -u gdm --no-pager
''', 90)
    (vm.directory / "live-desktop.txt").write_text(output)
    vm.screenshot("live-desktop.png")


def prepare_serial_boot(vm, encrypted=False, password=None, root_device=None, boot_device=None):
    """Add only serial observability to the installed GRUB configuration."""
    opening = ""
    cleanup = "umount -R /run/cenere-installed"
    if encrypted:
        encoded = base64.b64encode(password.encode()).decode()
        opening = r'''mkdir -p /run/cenere-test-boot
mount -t ext4 "$(blkid -L anser-boot)" /run/cenere-test-boot
python3 - <<'UNLOCK'
import base64, re, subprocess
config = open("/run/cenere-test-boot/grub/grub.cfg").read()
root_uuid = re.search(r"rd\.luks\.name=([a-fA-F0-9-]+)=anser-root", config)[1]
device = subprocess.check_output(["blkid", "-U", root_uuid], text=True).strip()
subprocess.run(["cryptsetup", "open", device, "cenere-test-root", "--key-file", "-"],
               input=base64.b64decode("PASSWORD_BASE64"), check=True)
UNLOCK
umount /run/cenere-test-boot
'''.replace("PASSWORD_BASE64", encoded)
        cleanup += "; cryptsetup close cenere-test-root"
    script = r'''set -eu
mkdir -p /run/cenere-installed
OPEN_ROOT
mount -t btrfs -o subvol=@ "$(blkid -L anser-system)" /run/cenere-installed
trap 'CLEANUP' EXIT
mount -t btrfs -o subvol=@home "$(awk '$2=="/home" {print $1}' /run/cenere-installed/etc/fstab)" /run/cenere-installed/home
mount -t ext4 "$(blkid -L anser-boot)" /run/cenere-installed/boot
mount -t vfat "$(awk '$2=="/boot/efi" {print $1}' /run/cenere-installed/etc/fstab)" /run/cenere-installed/boot/efi
cat /run/cenere-installed/etc/fstab
cat /run/cenere-installed/etc/hostname
cat /run/cenere-installed/etc/locale.conf
test ! -e /run/cenere-installed/etc/mkinitcpio.conf.d/archiso.conf
test -s /run/cenere-installed/boot/grub/grub.cfg
python3 - <<'EFI_CHECK'
from pathlib import Path
assert any(p.is_file() and p.stat().st_size for p in
           Path("/run/cenere-installed/boot/efi/EFI").glob("Anser*/grubx64.efi"))
EFI_CHECK
cat >> /run/cenere-installed/etc/default/grub <<'SERIAL'
# Temporary serial observability for this disposable VM test only.
GRUB_TERMINAL_INPUT="console serial"
GRUB_TERMINAL_OUTPUT="console serial"
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"
GRUB_CMDLINE_LINUX_DEFAULT="${GRUB_CMDLINE_LINUX_DEFAULT} console=ttyS0,115200 systemd.debug_shell=ttyS0 systemd.mask=serial-getty@ttyS0.service"
SERIAL
arch-chroot /run/cenere-installed grub-mkconfig -o /boot/grub/grub.cfg
sync
'''.replace("OPEN_ROOT", opening).replace("CLEANUP", cleanup)
    for device in (root_device, boot_device):
        if device and not re.fullmatch(r"/dev/vda[0-9]+", device):
            raise ValueError("Serial setup requires selected virtual-disk partitions")
    if root_device:
        script = script.replace('"$(blkid -L anser-system)"', root_device)
    if boot_device:
        script = script.replace('"$(blkid -L anser-boot)"', boot_device)
    return vm.command(script, 180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--expected-installer-sha", metavar="SHA256")
    parser.add_argument("--encrypt", action="store_true")
    parser.add_argument("--graphics", action="store_true",
                        help="Use runvm's display/input devices on both boots and check the live desktop")
    parser.add_argument("--existing-base", type=Path,
                        help="Clone an existing-system fixture and preserve its OS and home")
    parser.add_argument("--replace-existing", action="store_true")
    parser.add_argument("--discard-home", action="store_true")
    parser.add_argument("--expect-rejection", metavar="TEXT",
                        help="Require this pre-write rejection and an identical fixture disk")
    parser.add_argument("--directory", type=Path,
                        default=VM_ROOT / time.strftime("install-%Y%m%dT%H%M%SZ", time.gmtime()))
    parser.add_argument("--workaround-firstboot", action="store_true",
                        help="Exploration only: bypass old ISO's initial-setup blockage")
    parser.add_argument("--repository-check", action="store_true",
                        help="Enable guest networking; verify signed repository install/update in both boots")
    parser.add_argument("--keep-disk", action="store_true",
                        help="Retain this disposable disk as a fixture for a specific follow-up test")
    args = parser.parse_args()
    directory = begin_scope(args.directory, keep_disk=args.keep_disk)
    if args.repository_check:
        repository = RepositoryServer().__enter__()
        atexit.register(repository.__exit__)
    if args.existing_base:
        clone_disk(args.existing_base, directory / "disk.qcow2")
    password = secrets.token_urlsafe(24)
    executable = "/usr/bin/cenere"
    frozen = None
    upload_image = None
    if args.installer:
        frozen = directory / "cenere.arch"
        shutil.copyfile(args.installer, frozen)
        frozen.chmod(0o755)
        data = frozen.read_bytes()
        upload_image = directory / "installer-readonly.raw"
        upload_image.write_bytes(data + b"\x00" * (-len(data) % 512))
        executable = "/run/cenere-test"
    report = {"iso": str(args.iso.resolve()), "iso_detached_on_reboot": True,
              "old_iso_workarounds": args.workaround_firstboot, "encrypted": args.encrypt,
              "existing_base": str(args.existing_base.resolve()) if args.existing_base else None,
              "alongside": not args.replace_existing, "preserve_home": not args.discard_home,
              "installer_sha256": hashlib.sha256(frozen.read_bytes()).hexdigest() if frozen else None}
    with args.iso.open("rb") as image:
        report["iso_sha256"] = hashlib.file_digest(image, "sha256").hexdigest()
    (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
    with VM(directory, iso=args.iso, upload_image=upload_image, graphics=args.graphics,
            network=args.repository_check) as vm:
        vm.secrets.update((password.encode(), base64.b64encode(password.encode())))
        vm.boot_iso(workarounds=args.workaround_firstboot, graphical=args.graphics)
        if args.repository_check:
            (directory / "live-repository.txt").write_text(check_repository(vm))
        if args.graphics:
            try:
                check_live_desktop(vm)
            except RuntimeError:
                vm.screenshot("desktop-failure.png")
                diagnostic = vm.command("systemctl status greetd --no-pager; loginctl list-sessions --no-pager; "
                                        "ps -eo user,pid,args | grep -E 'Hyprland|uwsm|noctalia|greetd'; "
                                        "journalctl -b -u greetd --no-pager; true")
                (directory / "desktop-failure.txt").write_text(diagnostic)
                raise
        print("ISO booted; starting real disk installation.", flush=True)
        if frozen:
            vm.upload(frozen)
        program_hash = vm.command(f"sha256sum {executable}")
        report["installer_sha256"] = re.search(r"\b([a-f0-9]{64})\b", program_hash)[1]
        if args.expected_installer_sha and report["installer_sha256"] != args.expected_installer_sha:
            raise AssertionError("Booted installer does not match the frozen binary")
        (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
        home_before = home_snapshot(vm) if args.existing_base else None
        system_before = existing_system_snapshot(vm) if args.existing_base else None
        try:
            install_started = time.monotonic()
            # Real GUI tests exercise terminal progress directly. Automated
            # validation keeps dense progress off the emulated serial UART;
            # the full per-run installation log remains inside the guest.
            output = vm.command(f"umask 077\nTERM=dumb {executable} --install /dev/vda "
                                ">/run/cenere-test-console.log 2>&1 <<'ANSWERS'\n"
                                + answers(password, encrypted=args.encrypt, existing=bool(args.existing_base),
                                          alongside=not args.replace_existing, preserve=not args.discard_home)
                                + "ANSWERS\nresult=$?\ntail -c 8192 /run/cenere-test-console.log\nexit \"$result\"", 1200)
            install_elapsed = time.monotonic() - install_started
        except RuntimeError as error:
            (directory / "installation-failure.txt").write_text(str(error) + "\n")
            report["installation"] = "failed"
            (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
            if args.expect_rejection:
                if args.expect_rejection not in str(error) or not args.existing_base:
                    raise AssertionError("Expected rejection text and an existing fixture are required") from error
                vm.close()
                compared = subprocess.run(["/usr/bin/qemu-img", "compare", "-f", "qcow2", "-F", "qcow2",
                                           str(args.existing_base.resolve()), str(vm.disk)],
                                          check=True, text=True, capture_output=True)
                (directory / "safety-rejection-disk-compare.txt").write_text(compared.stdout + compared.stderr)
                report.update(installation="expected pre-write rejection", safety_rejection="passed",
                              unchanged_entire_disk="qemu-img compare verified identical guest-visible bytes")
                (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
                print(f"Expected rejection and unchanged disk passed. Artifacts: {directory}")
                return
            diagnostic = vm.command(r'''uname -r
lsblk -o NAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINTS
blkid -p /dev/vda2
udevadm info -q property /dev/vda2
lsblk -f
cat /etc/filesystems /proc/filesystems
modinfo vfat
mkdir -p /run/cenere-efi-probe
if mount -t vfat -o ro /dev/vda2 /run/cenere-efi-probe; then
    printf 'EXPLICIT_VFAT_MOUNT_PASSED\n'
    umount /run/cenere-efi-probe
fi
dmesg | tail -40
true''')
            (directory / "failure-diagnostics.txt").write_text(diagnostic)
            raise
        if "Installation completed." not in output:
            raise AssertionError("Installer exited without reporting a completed installation")
        if args.expect_rejection:
            raise AssertionError("Installer completed despite an expected rejection")
        (directory / "installation.txt").write_text(output)
        log_check = check_installation_log(vm, password, "/var/log/cenere/latest.log")
        (directory / "live-log-check.txt").write_text(log_check)
        performance = {"installer_elapsed_seconds": round(install_elapsed, 3),
                       "vcpu": 4, "memory_mib": vm.memory,
                       "live_log": json.loads(re.search(r"\{[^\r\n]*\}", log_check)[0])}
        (directory / "performance.json").write_text(json.dumps(performance, indent=2) + "\n")
        vm.command(r'''set -eu
test ! -e /dev/mapper/cenere-root
test ! -e /dev/mapper/cenere-home
python3 - <<'CLEANUP_CHECK'
for line in open("/proc/self/mountinfo"):
    assert not line.split()[4].startswith("/run/cenere-"), "Installer left a mounted workspace"
CLEANUP_CHECK''')
        print("Installation completed; preparing serial access for disk-only reboot.", flush=True)
        checks = prepare_serial_boot(vm, encrypted=args.encrypt, password=password)
        (directory / "installed-files.txt").write_text(checks)
        vm.command("sync")
    with VM(directory, graphics=args.graphics, network=args.repository_check) as vm:
        vm.secrets.update((password.encode(), base64.b64encode(password.encode())))
        vm.boot_installed(passphrase=password if args.encrypt else None)
        if args.repository_check:
            (directory / "installed-repository.txt").write_text(check_repository(vm))
        output = vm.command(r'''set -eu
test "$(cat /etc/hostname)" = anser-vm
grep -qx 'LANG=en_US.UTF-8' /etc/locale.conf
test "$(findmnt -n -o FSTYPE /)" = btrfs
test "$(findmnt -n -o FSTYPE /home)" = btrfs
test "$(findmnt -n -o UUID /)" = "$(findmnt -n -o UUID /home)"
test "$(findmnt -n -o FSTYPE /boot)" = ext4
test "$(findmnt -n -o FSTYPE /boot/efi)" = vfat
test ! -e /etc/mkinitcpio.conf.d/archiso.conf
! getent passwd liveuser
id vmuser
! pacman -Q cenere
! pacman -Q sudo
test ! -e /usr/bin/cenere
test ! -e /usr/share/cenere
test ! -e /usr/share/applications/cenere.desktop
pacman -Q > /run/installed-packages
cmp /run/installed-packages /usr/share/cenere/packages.final
pacman -Dk
runuser -u dbus -- test -r /etc/machine-id
findmnt -rn -o TARGET,SOURCE,FSTYPE | awk '$1=="/" || $1=="/home" || $1=="/boot" || $1=="/boot/efi"'
for service in multi-user.target dbus.service NetworkManager systemd-resolved; do
    systemctl is-active --quiet "$service" || {
        systemctl status "$service" --no-pager
        exit 1
    }
done
cat /etc/os-release
''')
        (directory / "disk-boot.txt").write_text(output)
        (directory / "account-check.txt").write_text(check_password(vm, password))
        installed_log_check = check_installation_log(vm, password, "/var/log/cenere/installation.log")
        (directory / "installed-log-check.txt").write_text(installed_log_check)
        performance["installed_log"] = json.loads(re.search(r"\{[^\r\n]*\}", installed_log_check)[0])
        (directory / "performance.json").write_text(json.dumps(performance, indent=2) + "\n")
        if args.encrypt:
            crypto = vm.command("set -eu\ncryptsetup status anser-root\n"
                                "test ! -e /dev/mapper/anser-home\n"
                                "test $(lsblk -rn -o TYPE | grep -c '^crypt$') = 1\n"
                                "findmnt -n -o SOURCE,FSTYPE,FSROOT / /home")
            (directory / "encryption.txt").write_text(crypto)
        if home_before is not None and not args.discard_home:
            # The installed /home is already mounted; inspect its sentinel in place.
            from fixtures import HOME_SNAPSHOT
            snapshot_output = vm.command(HOME_SNAPSHOT.replace("/run/cenere-fixture-home", "/home"))
            home_after = json.loads(re.search(r"HOME_SNAPSHOT=(\{[^\r\n]*\})", snapshot_output)[1])
            (directory / "home-after.json").write_text(json.dumps(home_after, indent=2) + "\n")
            assert_home_preserved(home_before, home_after)
        if system_before is not None and not args.replace_existing:
            system_after = existing_system_snapshot(vm)
            for field in ("partitions", "root_sha256"):
                if system_after[field] != system_before[field]:
                    raise AssertionError(f"Existing system changed: {field}")
            vm.command("grep -qx 'foreign EFI loader preservation sentinel' /boot/efi/EFI/OtherOS/keep.efi")
            (directory / "system-after.json").write_text(json.dumps(system_after, indent=2) + "\n")
    report["installation"] = "passed"
    report["disk_boot"] = "passed"
    (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Installation and disk-only boot passed. Artifacts: {directory}")


if __name__ == "__main__":
    main()
