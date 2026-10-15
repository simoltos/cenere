"""Existing-system fixtures built exclusively through VM.command in a guest."""

import json
import re


HOME_SNAPSHOT = r'''python3 - <<'PY'
import base64, hashlib, json, os, stat
from pathlib import Path
root = Path("/run/cenere-fixture-home/fixture-person")
result = {}
groups = {}
for path in [root, *sorted(root.rglob("*"))]:
    info = path.lstat()
    groups.setdefault((info.st_dev, info.st_ino), []).append(str(path.relative_to(root)))
for path in [root, *sorted(root.rglob("*"))]:
    info = path.lstat()
    result[str(path.relative_to(root))] = {
        "mode": info.st_mode, "uid": info.st_uid, "gid": info.st_gid,
        # Directory link counts differ between ext4 and Btrfs; file hardlinks
        # must still have their exact count and topology after a home copy.
        "mtime_ns": info.st_mtime_ns, "nlink": None if stat.S_ISDIR(info.st_mode) else info.st_nlink,
        "links": groups[(info.st_dev, info.st_ino)],
        "content": os.readlink(path) if path.is_symlink() else
                   hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None,
        "xattrs": {name: base64.b64encode(os.getxattr(path, name, follow_symlinks=False)).decode()
                   for name in sorted(os.listxattr(path, follow_symlinks=False))},
    }
print("HOME_SNAPSHOT=" + json.dumps(result, sort_keys=True))
PY'''


def home_snapshot(vm, device="/dev/vda3"):
    # Fixed fixture devices are the only acceptable inputs to guest mount scripts.
    if device not in ("/dev/vda3", "/dev/mapper/cenere-home"):
        raise ValueError("Unsupported fixture home device")
    output = vm.command("set -eu\nmkdir -p /run/cenere-fixture-home\n"
                        f"mount -t ext4 -o ro,noload {device} /run/cenere-fixture-home\n"
                        "trap 'umount /run/cenere-fixture-home' EXIT\n" + HOME_SNAPSHOT)
    match = re.search(r"HOME_SNAPSHOT=(\{[^\r\n]*\})", output)
    if not match:
        raise AssertionError("Guest did not return the fixture home metadata")
    return json.loads(match[1])


def existing_system_snapshot(vm):
    """Hash foreign ESP/root bytes and record all their GPT identities."""
    output = vm.command(r'''set -eu
python3 - <<'PY'
import hashlib, json, subprocess
layout = json.loads(subprocess.check_output(["sfdisk", "--json", "/dev/vda"]))
result = {"partitions": layout["partitiontable"]["partitions"][:3]}
for label, name in (("efi_sha256", "/dev/vda1"), ("root_sha256", "/dev/vda2")):
    digest = hashlib.sha256()
    with open(name, "rb", buffering=0) as stream:
        while block := stream.read(8 * 1024 * 1024):
            digest.update(block)
    result[label] = digest.hexdigest()
print("SYSTEM_SNAPSHOT=" + json.dumps(result, sort_keys=True))
PY''', 180)
    match = re.search(r"SYSTEM_SNAPSHOT=(\{[^\r\n]*\})", output)
    if not match:
        raise AssertionError("Guest did not return the existing system snapshot")
    return json.loads(match[1])


def create_existing_system(vm):
    """Create foreign root, separate ext4 home, ESP and trailing free space.

    The fixture exercises GPT GUID preservation, EFI files, numeric ownership,
    hardlinks, symlinks, binary data, ACLs, xattrs and unusual filename characters.
    It represents an identifiable foreign installation, not a bootable distro.
    """
    vm.command(r'''set -eu
test "$(lsblk -dn -o TYPE /dev/vda)" = disk
test ! -e /dev/vda1
sfdisk /dev/vda <<'LAYOUT'
label: gpt
size=512MiB, type=U, name="Other system EFI"
size=8GiB, type=L, name="Other system root"
size=4GiB, type=L, name="Existing home"
LAYOUT
udevadm settle
mkfs.fat -F 32 -n OTHER_EFI /dev/vda1
mkfs.ext4 -F -m 0 -L other-root /dev/vda2
mkfs.ext4 -F -m 0 -L home /dev/vda3
mkdir -p /run/cenere-fixture-root /run/cenere-fixture-home /run/cenere-fixture-efi
mount /dev/vda2 /run/cenere-fixture-root
mount /dev/vda3 /run/cenere-fixture-home
mount /dev/vda1 /run/cenere-fixture-efi
trap 'umount /run/cenere-fixture-efi /run/cenere-fixture-home /run/cenere-fixture-root' EXIT
mkdir -p /run/cenere-fixture-root/etc /run/cenere-fixture-efi/EFI/OtherOS
printf 'ID=fixture_linux\nNAME="Fixture Linux"\n' > /run/cenere-fixture-root/etc/os-release
printf 'UUID=%s /home ext4 defaults 0 2\n' "$(blkid -s UUID -o value /dev/vda3)" > /run/cenere-fixture-root/etc/fstab
printf 'foreign EFI loader preservation sentinel\n' > /run/cenere-fixture-efi/EFI/OtherOS/keep.efi
printf 'foreign operating system preservation sentinel\n' > /run/cenere-fixture-root/keep-system
python3 - <<'PY'
import os
from pathlib import Path
root = Path("/run/cenere-fixture-home/fixture-person")
root.mkdir(mode=0o750)
document = root / "document with spaces\\and\ttab"
document.write_bytes(b"preserved personal data\n\x00binary\xff\n")
document.chmod(0o640)
os.setxattr(document, "user.cenere-fixture", b"preserved xattr\x00value")
os.link(document, root / "hardlink")
(root / "symlink").symlink_to(document.name)
for path in [*root.iterdir(), root]:
    os.chown(path, 2042, 2043, follow_symlinks=False)
    os.utime(path, ns=(1700000000000000000, 1700000000000000000), follow_symlinks=False)
PY
setfacl -m u:2050:r-- /run/cenere-fixture-home/fixture-person/hardlink
sync
''', 120)
    before = home_snapshot(vm)
    output = vm.command("set -eu\nsfdisk --json /dev/vda\nblkid /dev/vda1 /dev/vda2 /dev/vda3")
    (vm.directory / "existing-layout.txt").write_text(output)
    (vm.directory / "home-before.json").write_text(json.dumps(before, indent=2) + "\n")
    system = existing_system_snapshot(vm)
    (vm.directory / "system-before.json").write_text(json.dumps(system, indent=2) + "\n")
    return before


def assert_home_preserved(before, after):
    if after != before:
        raise AssertionError("Preserved home files or their metadata differ; compare home snapshots")
