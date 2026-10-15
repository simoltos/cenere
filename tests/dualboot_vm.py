#!/usr/bin/env python3
"""Install alongside a real Anser baseline, then boot its old and new EFI loaders.

This legacy migration test requires an older ext4 Anser fixture: ESP vda2,
boot vda3, root vda4 and home vda5. A standalone disposable clone is always used.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets

from install_vm import answers, check_password, prepare_serial_boot
from vm import VM, begin_scope, clone_disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--expected-installer-sha", required=True)
    args = parser.parse_args()
    directory = begin_scope(args.directory)
    disk = clone_disk(args.baseline, directory / "disk.qcow2")
    password = secrets.token_urlsafe(24)
    report = {"iso": str(args.iso.resolve()), "baseline": str(args.baseline.resolve()),
              "iso_detached_on_both_reboots": True}
    with args.iso.open("rb") as image:
        report["iso_sha256"] = hashlib.file_digest(image, "sha256").hexdigest()
    with VM(directory, iso=args.iso, disk=disk) as vm:
        vm.secrets.add(password.encode())
        vm.boot_iso()
        program_hash = re.search(r"\b([a-f0-9]{64})\b", vm.command("sha256sum /usr/bin/cenere"))[1]
        assert program_hash == args.expected_installer_sha, "Wrong native installer version"
        report["installer_sha256"] = program_hash
        before = vm.command(r'''set -eu
mkdir -p /run/old-root /run/old-boot /run/old-home
mount -t ext4 /dev/vda4 /run/old-root
mount -t ext4 /dev/vda3 /run/old-boot
mount -t ext4 /dev/vda5 /run/old-home
trap 'umount /run/old-home /run/old-boot /run/old-root' EXIT
printf 'anser-old\n' > /run/old-root/etc/hostname
printf 'old operating system sentinel\n' > /run/old-root/keep-old-system
printf 'shared old user data sentinel\n' > /run/old-home/vmuser/keep-old-data
chown 1000:1000 /run/old-home/vmuser/keep-old-data
blkid /dev/vda2 /dev/vda3 /dev/vda4 /dev/vda5
sha256sum /run/old-root/keep-old-system /run/old-home/vmuser/keep-old-data /run/old-boot/grub/grub.cfg /run/old-boot/vmlinuz-linux-cachyos-lts
sfdisk --json /dev/vda
sync''')
        (directory / "old-system-before.txt").write_text(before)
        old_root_uuid = re.search(r'/dev/vda4:[^\r\n]*?\bUUID="([^"]+)"', before)[1]
        print("Real baseline prepared; installing alongside with automatic shrink.", flush=True)
        output = vm.command("TERM=dumb /usr/bin/cenere --install /dev/vda <<'ANSWERS'\n"
                            + answers(password, existing=True) + "ANSWERS", 1200)
        assert "Installation completed." in output, "Installer did not complete"
        assert "SHRINK /dev/vda4:" in output, "Expected automatic old-root shrink was absent"
        root = re.search(r"CREATE (/dev/vda[0-9]+):[^\r\n]*\(Btrfs system and home, shared space\)", output)[1]
        boot = re.search(r"CREATE (/dev/vda[0-9]+):[^\r\n]*\(/boot\)", output)[1]
        (directory / "installation.txt").write_text(output)
        checks = prepare_serial_boot(vm, root_device=root, boot_device=boot)
        (directory / "new-installed-files.txt").write_text(checks)
        configured = vm.command(r'''set -eu
efibootmgr -c -d /dev/vda -p 2 -L OldAnser -l '\EFI\Anser\grubx64.efi'
efibootmgr
blkid /dev/vda2 /dev/vda3 /dev/vda4 /dev/vda5
test -z "$(findmnt -rn -S /dev/vda4)"
''')
        (directory / "efi-entries.txt").write_text(configured)
        old_entry = re.search(r"Boot([0-9A-Fa-f]{4})\*? OldAnser", configured)[1]
        new_entry = re.search(r"Boot([0-9A-Fa-f]{4})\*? Anser-[^\r\n]+", configured)[1]
        vm.command(f"set -eu\nefibootmgr -n {new_entry}\nsync")
    with VM(directory, disk=disk) as vm:
        vm.secrets.add(password.encode())
        vm.boot_installed()
        new_boot = vm.command(f'''set -eu
test "$(cat /etc/hostname)" = anser-vm
test "$(findmnt -no SOURCE /)" = {root}
test "$(blkid -s UUID -o value /dev/vda4)" = {old_root_uuid}
grep -qx 'shared old user data sentinel' /home/vmuser/keep-old-data
test -s /boot/efi/EFI/Anser/grubx64.efi
findmnt -rn -o TARGET,SOURCE,FSTYPE
blkid /dev/vda4
efibootmgr
''')
        (directory / "new-system-boot.txt").write_text(new_boot)
        (directory / "account-check.txt").write_text(check_password(vm, password))
        vm.command(f"set -eu\nefibootmgr -n {old_entry}\nsync")
        print("New system booted; next boot selects the original EFI loader.", flush=True)
    with VM(directory, disk=disk) as vm:
        vm.boot_installed()
        old_boot = vm.command(f'''set -eu
test "$(cat /etc/hostname)" = anser-old
test "$(findmnt -no SOURCE /)" = /dev/vda4
test "$(blkid -s UUID -o value /dev/vda4)" = {old_root_uuid}
grep -qx 'old operating system sentinel' /keep-old-system
grep -qx 'shared old user data sentinel' /home/vmuser/keep-old-data
uname -r
findmnt -rn -o TARGET,SOURCE,FSTYPE
blkid /dev/vda4
efibootmgr
''')
        (directory / "old-system-boot.txt").write_text(old_boot)
    report.update(automatic_ext4_shrink="passed", new_efi_boot="passed", old_efi_boot="passed")
    (directory / "run.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Automatic shrink and both actual EFI boots passed. Artifacts: {directory}")


if __name__ == "__main__":
    main()
