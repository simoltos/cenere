#!/usr/bin/env python3
"""Small ISO/UEFI test driver; all VM writes stay in the disposable VM data directory.

Set ANSER_PROJECT to the sibling Anser root and ANSER_VM_DIR to a disposable
subdirectory when overriding the Anser .cache/vm default.
Run `python3 tests/vm.py ISO` to boot an untouched ISO and record a
root-shell smoke check. Import VM for installation and subsequent disk boots.
Guest commands execute only inside QEMU. No host disks are used. Networking is
disabled unless a scoped repository check explicitly enables it.
"""

import argparse
import atexit
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid


DEFAULT_PROJECT = Path(__file__).resolve().parents[1].parent / "anser"
PROJECT = Path(os.environ.get("ANSER_PROJECT", DEFAULT_PROJECT)).resolve()
VM_BASE = PROJECT / ".cache" / "vm"
VM_ROOT = Path(os.environ.get("ANSER_VM_DIR", VM_BASE)).absolute()
FIRMWARE = Path("/usr/share/edk2/ovmf")
PASSWORD_PROMPT = rb"(?i:Please enter passphrase|Enter passphrase)[^\r\n]{0,300}:[ \t]*"


def artifact(path):
    original = Path(path).absolute()
    path = original.resolve()
    if not (PROJECT / "local/mkanseriso").is_file():
        raise ValueError("Set ANSER_PROJECT to the Anser repository root")
    if "," in str(path):
        raise ValueError("VM paths cannot contain QEMU option separators")
    if (VM_ROOT.is_symlink() or VM_BASE.resolve() != VM_BASE
            or not VM_ROOT.resolve().is_relative_to(VM_BASE)
            or not path.is_relative_to(VM_ROOT.resolve()) or path == VM_ROOT.resolve()
            or any(parent.is_symlink() for parent in (original, *original.parents)
                   if parent.is_relative_to(VM_BASE))):
        raise ValueError(f"VM writes must be beneath {VM_ROOT}: {path}")
    return path


def cleanup_scope(directory):
    """Remove only this scope's large VM media after processes/mounts are gone.

    Serial output, screenshots and text/JSON diagnostics remain in place.
    Refuse cleanup if any process still has a file in the scope open or a mount
    remains beneath it. A refusal leaves the evidence and disk for inspection.
    """
    directory = artifact(directory)
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        mount = Path(line.split()[4].replace("\\040", " ").replace("\\134", "\\"))
        if mount == directory or mount.is_relative_to(directory):
            raise RuntimeError(f"VM scope is still mounted: {directory}")
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or process.name == str(os.getpid()):
            continue
        try:
            descriptors = list((process / "fd").iterdir())
        except (FileNotFoundError, PermissionError):
            continue
        for descriptor in descriptors:
            try:
                target = descriptor.readlink()
            except (FileNotFoundError, PermissionError):
                continue
            if target.is_absolute() and target.is_relative_to(directory):
                raise RuntimeError(f"VM scope still used by process {process.name}: {directory}")
    for name in ("disk.qcow2", "OVMF_VARS.fd", "installer-readonly.raw", "cenere.arch",
                 "serial.sock", "qmp.sock"):
        artifact(directory / name).unlink(missing_ok=True)


def begin_scope(directory, keep_disk=False):
    """Claim a fresh test scope; never adopt or delete earlier test media."""
    directory = artifact(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        raise ValueError(f"Use a fresh disposable test directory: {directory}")
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if not keep_disk:
        atexit.register(cleanup_scope, directory)
    return directory


class RepositoryServer:
    """Use Anser runvm's validated, loopback-only repository lifecycle."""

    def __enter__(self):
        self.process = subprocess.Popen([str(PROJECT / "runvm"), "--repo-only"],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # This message is emitted only after runvm validates the listener and
        # current repository bytes. Merely seeing an open port is insufficient.
        pending = b""
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError("runvm repository server failed; inspect Anser out/runvm.log")
                if select.select([self.process.stdout], [], [], 0.1)[0]:
                    chunk = os.read(self.process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    pending += chunk
                    if b"Testing repository: http://10.0.2.2:8080 (host 127.0.0.1 only)\n" in pending:
                        return self
            raise TimeoutError("Testing repository server did not become ready")
        except BaseException:
            self.__exit__()
            raise

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stdout.close()


def check_repository(vm):
    """Check the real configured endpoint, signatures, install and update."""
    return vm.command(r'''set -eu
test "$(pacman-conf --repo anser-testing Server)" = http://10.0.2.2:8080
! pacman -Q oca
cat > /run/cenere-repository-check.conf <<'PACMAN'
[options]
Architecture = auto
SigLevel = Required DatabaseRequired
LocalFileSigLevel = Required
[anser-testing]
Server = http://10.0.2.2:8080
PACMAN
for attempt in $(seq 1 60); do
    if curl --fail --silent --output /dev/null http://10.0.2.2:8080/anser-testing.db; then break; fi
    sleep 1
done
pacman --config /run/cenere-repository-check.conf -Sy --noconfirm
pacman --config /run/cenere-repository-check.conf -Si cenere oca
pacman -Syu --noconfirm oca
pacman --config /run/cenere-repository-check.conf -Syu --noconfirm
pacman -Q oca
pacman -Rns --noconfirm oca
! pacman -Q oca
''', 300)


class VM:
    def __init__(self, directory, iso=None, disk=None, memory=6144, upload_image=None,
                 graphics=False, network=False, firmware="uefi", kernel=None, initrd=None,
                 kernel_args=""):
        self.directory = artifact(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = artifact(self.directory / ".vm.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lock.close()
            raise RuntimeError(f"VM scope is already in use: {self.directory}") from None
        self.iso = Path(iso).resolve(strict=True) if iso else None
        if self.iso and (not self.iso.is_file() or "," in str(self.iso)):
            raise ValueError("ISO must be a regular image file")
        self.disk = artifact(disk or self.directory / "disk.qcow2")
        if self.disk != self.directory / "disk.qcow2":
            raise ValueError("Each test scope must own its own disk.qcow2")
        if self.disk.exists() and not self.disk.is_file():
            raise ValueError("VM disk must be a regular image file")
        if not self.disk.exists():
            subprocess.run(["/usr/bin/qemu-img", "create", "-f", "qcow2", str(self.disk), "64G"],
                           check=True, stdout=subprocess.DEVNULL)
        disk_info(self.disk)
        self.memory = memory
        self.upload_image = artifact(upload_image) if upload_image else None
        if self.upload_image and not self.upload_image.is_file():
            raise ValueError("Upload media must be a regular disposable file")
        self.graphics = graphics
        self.network = network
        if firmware not in ("uefi", "bios"):
            raise ValueError("Unknown test firmware")
        if bool(kernel) != bool(initrd):
            raise ValueError("Direct boot requires both kernel and initrd")
        self.firmware = firmware
        self.kernel = artifact(kernel) if kernel else None
        self.initrd = artifact(initrd) if initrd else None
        self.kernel_args = kernel_args
        self.process = None
        self.serial = self.qmp = None
        self.pending = b""
        self.secrets = set()
        self.log_pending = b""
        self.shell_ready = False
        self.command_active = False
        self.shutdown_result = None

    def start(self):
        if self.process is not None:
            raise RuntimeError("VM instance cannot be started twice")
        variables = artifact(self.directory / "OVMF_VARS.fd")
        if self.firmware == "uefi" and not variables.exists():
            shutil.copyfile(FIRMWARE / "OVMF_VARS.fd", variables)
        serial_path = artifact(self.directory / "serial.sock")
        qmp_path = artifact(self.directory / "qmp.sock")
        for path in (serial_path, qmp_path):
            path.unlink(missing_ok=True)
        # Keep the disk in the firmware boot order while selecting the ISO;
        # OVMF can otherwise prune its saved operating-system entries.
        args = ["/usr/bin/qemu-system-x86_64", "-machine", "q35,accel=kvm", "-cpu", "host",
                "-smp", "4", "-m", str(self.memory), "-nic",
                "user,model=virtio-net-pci" if self.network else "none",
                "-drive", f"if=none,id=systemdisk,format=qcow2,file={self.disk}",
                "-device", "virtio-blk-pci,drive=systemdisk" + (",bootindex=2" if self.iso else ""),
                "-serial", f"unix:{serial_path},server=on,wait=off",
                "-qmp", f"unix:{qmp_path},server=on,wait=off", "-no-reboot"]
        if self.firmware == "uefi":
            args.extend(["-drive", f"if=pflash,format=raw,readonly=on,file={FIRMWARE / 'OVMF_CODE.fd'}",
                         "-drive", f"if=pflash,format=raw,file={variables}"])
        if self.kernel:
            args.extend(["-kernel", str(self.kernel), "-initrd", str(self.initrd),
                         "-append", self.kernel_args])
        if self.graphics:
            # Match runvm's graphics/input devices; QMP supplies the test display.
            args.extend(["-display", "none", "-vga", "std",
                         "-device", "qemu-xhci", "-device", "usb-tablet"])
        else:
            args.extend(["-display", "none"])
        if self.iso:
            args.extend(["-drive", f"if=none,media=cdrom,readonly=on,id=liveiso,file={self.iso}",
                         "-device", "ide-cd,drive=liveiso,bootindex=1"])
        if self.upload_image:
            args.extend(["-drive", f"if=virtio,format=raw,readonly=on,file={self.upload_image}"])
        self.stderr = artifact(self.directory / "qemu.log").open("ab")
        self.transcript = artifact(self.directory / "serial.log").open("ab", buffering=0)
        self.process = subprocess.Popen(args, stdout=self.stderr, stderr=self.stderr)
        try:
            self.serial = self.connect(serial_path)
            self.qmp = self.connect(qmp_path)
            self.qmp.settimeout(5)
            self.qmp_file = self.qmp.makefile("rwb", buffering=0)
            json.loads(self.qmp_file.readline())
            self.monitor("qmp_capabilities")
        except BaseException:
            self.close()
            raise
        return self

    def connect(self, path):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            connection = socket.socket(socket.AF_UNIX)
            try:
                connection.connect(str(path))
                return connection
            except (FileNotFoundError, ConnectionRefusedError):
                connection.close()
                if self.process.poll() is not None:
                    raise RuntimeError("QEMU exited; inspect qemu.log")
                time.sleep(0.05)
        raise TimeoutError(f"QEMU socket did not appear: {path}")

    def monitor(self, name, arguments=None):
        request = {"execute": name}
        if arguments:
            request["arguments"] = arguments
        self.qmp_file.write(json.dumps(request).encode() + b"\n")
        while True:
            reply = json.loads(self.qmp_file.readline())
            if "error" in reply:
                raise RuntimeError(reply["error"])
            if "return" in reply:
                return reply["return"]

    def keyboard(self, value):
        """Type ASCII through the guest keyboard, avoiding EFI UART polling."""
        punctuation = {" ": "spc", "\n": "ret", "\r": "ret", "_": "minus",
                       ";": "semicolon", "-": "minus", "/": "slash", ".": "dot",
                       "$": "4", '"': "apostrophe", "=": "equal", "(": "9", ")": "0",
                       ",": "comma", "@": "2"}
        unknown = [c for c in value if c not in punctuation and not (c.isascii() and c.isalnum())]
        if unknown:
            raise ValueError(f"Unsupported keyboard characters: {unknown!r}")
        for character in value:
            code = punctuation.get(character, character.lower())
            keys = [{"type": "qcode", "data": code}]
            if character in '_$"()@' or character.isupper():
                keys.insert(0, {"type": "qcode", "data": "shift"})
            self.monitor("send-key", {"keys": keys, "hold-time": 20})
            time.sleep(0.05)
            while self.serial and select.select([self.serial], [], [], 0)[0]:
                data = self.serial.recv(65536)
                if not data:
                    raise RuntimeError("Guest serial socket closed during keyboard input")
                self.log_output(data)
                self.pending += data

    def send(self, value):
        self.serial.sendall(value.encode() if isinstance(value, str) else value)

    def redact(self, data):
        for secret in sorted(self.secrets, key=len, reverse=True):
            data = data.replace(secret, b"[REDACTED]")
        return data

    def log_output(self, data=b"", flush=False):
        self.log_pending = self.redact(self.log_pending + data)
        keep = 0 if flush else max((len(value) - 1 for value in self.secrets), default=0)
        if len(self.log_pending) > keep:
            boundary = len(self.log_pending) - keep
            self.transcript.write(self.log_pending[:boundary])
            self.log_pending = self.log_pending[boundary:]

    def send_secret(self, value):
        self.secrets.add(value.encode())
        self.send(value + "\r")

    def expect(self, expression, timeout=120):
        pattern = re.compile(expression.encode() if isinstance(expression, str) else expression,
                             re.DOTALL)
        deadline = time.monotonic() + timeout
        while True:
            match = pattern.search(self.pending)
            if match:
                before = self.pending[:match.start()]
                self.pending = self.pending[match.end():]
                return before, match
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Guest did not emit {expression!r}; inspect serial.log")
            ready, _, _ = select.select([self.serial], [], [], min(remaining, 1))
            if ready:
                data = self.serial.recv(65536)
                if not data:
                    raise RuntimeError("Guest serial socket closed; inspect qemu.log")
                self.log_output(data)
                self.pending += data
            elif self.process.poll() is not None:
                raise RuntimeError("QEMU exited; inspect qemu.log")

    def boot_iso(self, workarounds=False, wait_ready=True, kernel="linux", graphical=False,
                 copy_to_ram=False):
        """Boot the ISO through its systemd-boot menu with serial test access."""
        if kernel != "linux":
            raise ValueError("The Anser image uses the standard Arch kernel")
        if not self.kernel:
            self.expect(b"Anser install medium",90)
            self.keyboard("e")
            self.expect(b"archisobasedir=arch",15)
            self.monitor("send-key",{"keys":[{"type":"qcode","data":"home"}],"hold-time":20})
            options=["cow_spacesize=10G","console=ttyS0,115200n8","systemd.debug_shell=ttyS0",
                     "systemd.mask=serial-getty@ttyS0.service"]
            if workarounds:
                options += ["systemd.mask=systemd-firstboot.service","systemd.mask=systemd-time-wait-sync.service"]
            if not graphical:
                options.append("systemd.unit=multi-user.target")
            if copy_to_ram:
                options.append("copytoram=y")
            self.keyboard(" "+" ".join(options)+" ")
            self.keyboard("\n")
        self.expect(rb"(?:sh|bash)-[^\r\n]*#",180)
        self.send("stty -echo; export PS1='CENERE_VM# ' PS2='' SYSTEMD_PAGER=cat "
                  "PAGER=cat SYSTEMD_COLORS=0 TERM=dumb; printf '\\nREADY\\n'\r")
        self.expect(rb"\r*\nREADY\r*\n",30)
        self.shell_ready=True
        if wait_ready:
            target="graphical.target" if graphical else "multi-user.target"
            self.command(f"while ! systemctl is-active --quiet {target}; do sleep 1; done",180)

    def screenshot(self, name="screen.png"):
        destination = artifact(self.directory / name)
        self.monitor("screendump", {"filename": str(destination), "format": "png"})
        return destination

    def command(self, script, timeout=120, passphrase=None):
        """Run a guest shell script; raise on failure and return its output."""
        if not self.shell_ready or self.command_active:
            raise RuntimeError("Guest command requires an idle, initialized test shell")
        token = "CENERE_" + uuid.uuid4().hex
        if passphrase:
            self.secrets.add(passphrase.encode())
        # Use a single quoted here-document so host and guest shells cannot
        # expand script content before the guest sh executes it.
        if token in script:
            raise ValueError("Unexpected here-document delimiter in script")
        self.command_active = True
        self.send(f"sh <<'{token}'; printf '\\n{token}:%s\\n' \"$?\"\n"
                  f"{script}\n{token}\n")
        marker = rb"\r?\n" + token.encode() + rb":([0-9]+)\r?\n"
        expression = marker if passphrase is None else marker + b"|" + PASSWORD_PROMPT
        deadline = time.monotonic() + timeout
        chunks = []
        attempts = 0
        while True:
            output, match = self.expect(expression, max(0, deadline - time.monotonic()))
            chunks.append(output)
            if match[1] is not None:
                self.command_active = False
                break
            attempts += 1
            if attempts > 4:
                raise RuntimeError("Encrypted boot repeatedly rejected the passphrase")
            self.send_secret(passphrase)
        output = self.redact(b"".join(chunks))
        if int(match[1]) != 0:
            raise RuntimeError(f"Guest command failed ({match[1].decode()}): {output.decode(errors='replace')}")
        return output.decode(errors="replace")

    def boot_installed(self, passphrase=None):
        """Reach the installed kernel's test serial shell with ISO detached.

        Before shutting down the installation guest, add console/debug-shell
        arguments to its GRUB config. No initramfs or storage step is bypassed.
        Passphrases stay in memory and are never copied to host artifacts.
        """
        if self.iso:
            raise ValueError("Installed-system boot requires the ISO to be detached")
        if passphrase:
            self.secrets.add(passphrase.encode())
        shell = rb"(?:sh|bash)-[^\r\n]*#"
        _, match = self.expect(shell + b"|" + PASSWORD_PROMPT, 180)
        if not match[0].endswith(b"#"):
            if passphrase is None:
                raise RuntimeError("Installed system requested an encryption passphrase")
            self.send_secret(passphrase)
            # Plymouth redraws its prompt for every typed character. These are
            # not new password requests; send the known test secret only once.
            self.expect(shell, 180)
        self.send("stty -echo; export PS1='CENERE_VM# ' PS2='' SYSTEMD_PAGER=cat "
                  "PAGER=cat SYSTEMD_COLORS=0 TERM=dumb; printf '\\nREADY\\n'\r")
        self.expect(rb"\r?\nREADY\r?\n", 30)
        self.shell_ready = True
        self.command("while ! systemctl is-active --quiet multi-user.target; do sleep 1; done", 180,
                     passphrase=passphrase)

    def upload(self, source, destination="/run/cenere-test"):
        """Transfer an executable from read-only test media; verify exact bytes."""
        if not re.fullmatch(r"/run/[a-zA-Z0-9_.-]+", destination):
            raise ValueError("Uploads require a simple disposable /run filename")
        data = Path(source).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if not self.upload_image:
            raise ValueError("Executable upload requires attached read-only test media")
        self.command(f"set -eu\nhead -c {len(data)} /dev/vdb > {destination}\n"
                     f"test \"$(sha256sum {destination} | cut -d ' ' -f 1)\" = {digest}\n"
                     f"chmod 755 {destination}\nprintf 'UPLOADED {digest}\\n'", 30)
        return digest

    def close(self, require_durable=False):
        """Flush an idle guest and power it off before releasing its disk.

        A failed boot or an unfinished serial command is not an idle shell:
        never inject commands into that input stream. Forced shutdown remains
        available for cleanup, but its evidence never claims durable success.
        """
        if self.shutdown_result is not None:
            if require_durable and not self.shutdown_result["durable"]:
                raise RuntimeError("VM shutdown was forced; disk durability was not verified")
            return self.shutdown_result
        result = {"iso_attached": self.iso is not None, "shell_ready": self.shell_ready,
                  "command_active": self.command_active, "synced": False,
                  "orderly_poweroff": False, "forced": False, "durable": False}
        interrupted = None
        if self.process and self.process.poll() is None:
            if self.shell_ready and not self.command_active:
                try:
                    # The foreground installer runs through command(), which
                    # keeps command_active set until its completion marker.
                    # Also avoid powering off an independently launched TUI.
                    self.command("! pgrep -x cenere >/dev/null", 10)
                    self.command("sync", 60)
                    result["synced"] = True
                    self.send("systemctl poweroff --no-block\n")
                    self.shell_ready = False
                    deadline = time.monotonic() + 60
                    while self.process.poll() is None and time.monotonic() < deadline:
                        if self.serial and select.select([self.serial], [], [], 0.1)[0]:
                            data = self.serial.recv(65536)
                            if data:
                                self.log_output(data)
                            else:
                                self.process.wait(timeout=max(0.1, deadline - time.monotonic()))
                        elif not self.serial:
                            self.process.wait(timeout=max(0.1, deadline - time.monotonic()))
                    result["orderly_poweroff"] = self.process.poll() == 0
                except BaseException as error:
                    result["error"] = self.redact(str(error).encode()).decode(errors="replace")
                    if not isinstance(error, Exception):
                        interrupted = error
            if self.process.poll() is None:
                result["forced"] = True
                try:
                    self.monitor("quit")
                except (OSError, ValueError, RuntimeError, AttributeError):
                    self.process.terminate()
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
        result["durable"] = result["synced"] and result["orderly_poweroff"] and not result["forced"]
        self.shutdown_result = result
        if getattr(self, "transcript", None) and not self.transcript.closed:
            self.log_output(flush=True)
        for name in ("serial", "qmp_file", "qmp", "transcript", "stderr", "lock"):
            value = getattr(self, name, None)
            if value:
                value.close()
        with artifact(self.directory / "shutdowns.jsonl").open("a") as evidence:
            evidence.write(json.dumps(result) + "\n")
        if interrupted is not None:
            raise interrupted
        if not result["durable"]:
            message = "VM required cleanup without a verified durable guest shutdown; inspect shutdowns.jsonl"
            if require_durable:
                raise RuntimeError(message)
            print(message, file=sys.stderr)
        return result

    def __enter__(self):
        return self.start()

    def __exit__(self, exception_type, *_):
        try:
            self.close(require_durable=exception_type is None)
        except Exception as error:
            if exception_type is None:
                raise
            # Cleanup diagnostics must not replace the original test failure.
            print(f"VM cleanup also failed: {error}", file=sys.stderr)


def disk_info(disk):
    info = json.loads(subprocess.check_output(["/usr/bin/qemu-img", "info", "--output=json", str(disk)]))
    if info.get("format") != "qcow2" or info.get("backing-filename"):
        raise ValueError("Test disks must be standalone qcow2 images")
    return info


def clone_disk(source, destination):
    """Make a standalone disposable clone without a temporary backing dependency."""
    source = artifact(source)
    destination = artifact(destination)
    if not source.is_file() or destination.exists():
        raise ValueError("Clone requires a regular source and a new destination")
    disk_info(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["/usr/bin/qemu-img", "convert", "-f", "qcow2", "-O", "qcow2",
                    str(source), str(destination)], check=True, stdout=subprocess.DEVNULL)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--directory", type=Path,
                        default=VM_ROOT / time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()))
    parser.add_argument("--workaround-firstboot", action="store_true",
                        help="Exploration only: mask firstboot and network time wait on old images")
    parser.add_argument("--graphics", action="store_true",
                        help="Use runvm's display/input devices and boot the natural graphical target")
    args = parser.parse_args()
    begin_scope(args.directory)
    with VM(args.directory, iso=args.iso, graphics=args.graphics) as vm:
        vm.boot_iso(workarounds=args.workaround_firstboot, graphical=args.graphics)
        result = vm.command("set -eu\ncat /etc/os-release\nlsblk -o NAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS\n"
                            "test -d /sys/firmware/efi\nsystemctl is-active multi-user.target")
        (vm.directory / "smoke.txt").write_text(result)
        if args.graphics:
            vm.screenshot()
        print(result)
        print(f"VM artifacts: {vm.directory}")


if __name__ == "__main__":
    main()
