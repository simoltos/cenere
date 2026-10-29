#!/usr/bin/env python3
"""Exercise fresh installs, coexistence and refusal on disposable VM disks."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import tempfile

from preservation_vm import result
from vm import VM, VM_ROOT


def live_kernel(iso, directory):
    paths = []
    for name in ('vmlinuz-linux', 'initramfs-linux.img'):
        path = directory/name
        with path.open('wb') as stream:
            subprocess.run(['isoinfo', '-R', '-i', str(iso), '-x', '/arch/boot/x86_64/'+name],
                           stdout=stream, check=True)
        if not path.stat().st_size:
            raise RuntimeError('The ISO has no '+name)
        paths.append(path)
    info = subprocess.check_output(['isoinfo', '-d', '-i', str(iso)], text=True)
    label = re.search(r'^Volume id: (.+)$', info, re.M)[1]
    return dict(kernel=paths[0], initrd=paths[1], kernel_args=
                f'archisobasedir=arch archisolabel={label} copytoram=n '
                'console=ttyS0,115200n8 systemd.debug_shell=ttyS0 '
                'systemd.mask=serial-getty@ttyS0.service systemd.unit=multi-user.target')


def existing_data(vm, kind):
    script = r'''set -eu
sfdisk /dev/vda <<'TABLE'
label: gpt
size=600MiB,type=U
size=2GiB,type=L
type=TYPE
TABLE
udevadm settle
mkfs.fat -F32 /dev/vda1
mkfs.ext4 -F -m0 /dev/vda2
FORMAT
mkdir /run/old
mount -t FSTYPE /dev/vda3 /run/old
python3 - <<'PY'
from pathlib import Path
import os,hashlib,json,subprocess
p=Path('/run/old')
if 'FSTYPE'=='ext4':
    free=os.statvfs(p).f_bavail*os.statvfs(p).f_frsize
    subprocess.run(['fallocate','-l',str(free-512*1024**2),str(p/'reserve')],check=True)
(p/'retained-data').write_bytes(os.urandom(8*1024**2))
os.sync()
if (p/'reserve').exists():
    (p/'reserve').unlink()
    subprocess.run(['filefrag','-v',str(p/'retained-data')],check=True)
(p/'etc').mkdir()
(p/'etc/os-release').write_text('ID=existing-system\n')
(p/'home').mkdir()
(p/'home/keep').write_text('existing home data\n')
data={'files':{str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest()
               for f in p.rglob('*') if f.is_file()},
      'layout':json.loads(subprocess.check_output(['sfdisk','--json','/dev/vda'])),
      'uuid':subprocess.check_output(['blkid','-s','UUID','-o','value','/dev/vda3'],text=True).strip()}
print('EXISTING='+json.dumps(data))
PY
sync
umount /run/old
'''
    script = script.replace('TYPE', 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7' if kind == 'ntfs' else 'L', 1)
    script = script.replace('FORMAT', 'mkntfs -F -Q /dev/vda3' if kind == 'ntfs' else 'mkfs.ext4 -F -m0 /dev/vda3')
    script = script.replace('FSTYPE', 'ntfs3' if kind == 'ntfs' else 'ext4')
    return result(vm.command(script, 240), 'EXISTING')


def run_case(iso, installer, case, encrypted=False, firmware='uefi'):
    VM_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='install-', dir=VM_ROOT) as temporary:
        directory = Path(temporary)
        media = directory/'installer-readonly.raw'
        binary = installer.read_bytes()
        media.write_bytes(binary+b'\0'*(-len(binary)%512))
        executable = directory/'cenere.arch'
        executable.write_bytes(binary)
        boot = live_kernel(iso, directory) if firmware == 'bios' else {}
        password = secrets.token_hex(12)
        kind = 'ntfs' if case == 'alongside-ntfs' else 'ext4'
        print(f'{case} ({firmware}, encrypted={encrypted}): boot live', flush=True)
        with VM(directory, iso=iso, upload_image=media, firmware=firmware, **boot) as vm:
            vm.boot_iso()
            vm.upload(executable)
            before = existing_data(vm, kind) if case != 'fresh' else None
            disk_prefix = "python3 - <<'PY'\nimport hashlib,json\nprint('PREFIX='+json.dumps({device:hashlib.sha256(open(device,'rb').read(4*1024**2)).hexdigest() for device in ['/dev/vda','/dev/vda3']}))\nPY"
            if case == 'unsupported-home':
                unchanged = result(vm.command(disk_prefix), 'PREFIX')
            answers = ['us', 'keyboard test']
            if before:
                answers += ['replace' if case == 'unsupported-home' else 'keep',
                            'yes' if case == 'unsupported-home' else 'no']
            answers += ['yes' if encrypted else 'no', 'en_US.UTF-8', 'UTC',
                        'anser-fixture', 'person', password, password, 'INSTALL']
            encoded = base64.b64encode(('\n'.join(answers)+'\n').encode()).decode()
            vm.secrets.update((password.encode(), encoded.encode()))
            print(f'{case}: run installer', flush=True)
            output = vm.command("python3 - <<'PY'\nimport subprocess,base64\n"
                f"p=subprocess.run(['/run/cenere-test','--install','/dev/vda'],input=base64.b64decode('{encoded}'),capture_output=True)\n"
                "print(p.stdout.decode(errors='replace'));print(p.stderr.decode(errors='replace'))\n"
                f"assert p.returncode=={1 if case=='unsupported-home' else 0},p.returncode\nPY", 900)
            if case == 'unsupported-home':
                assert 'compatible Btrfs layout' in output
                assert result(vm.command(disk_prefix), 'PREFIX') == unchanged, 'Rejected installation wrote disk metadata'
            if before:
                checked = vm.command(r'''set -eu
mount -t FSTYPE -o ro /dev/vda3 /run/old || { dmesg | tail -30; exit 1; }
python3 - <<'PY'
from pathlib import Path
import hashlib,json,subprocess
p=Path('/run/old')
print('CHECKED='+json.dumps({'files':{str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest()
    for f in p.rglob('*') if f.is_file()},
    'layout':json.loads(subprocess.check_output(['sfdisk','--json','/dev/vda'])),
    'uuid':subprocess.check_output(['blkid','-s','UUID','-o','value','/dev/vda3'],text=True).strip()}))
PY
umount /run/old
'''.replace('FSTYPE', 'ntfs-3g' if kind == 'ntfs' else 'ext4'))
                after = result(checked, 'CHECKED')
                assert before['files'] == after['files'], 'Existing contents changed'
                assert before['uuid'] == after['uuid'], 'Existing filesystem identity changed'
                if case == 'unsupported-home':
                    assert before['layout'] == after['layout'], 'Rejected installation changed partitions'
                    return {'case': case, 'rejected_before_changes': True}
                old = before['layout']['partitiontable']['partitions'][-1]
                new = next(p for p in after['layout']['partitiontable']['partitions'] if p['node']=='/dev/vda3')
                assert new['size'] < old['size'], 'Coexistence did not exercise a real shrink'
                assert {k:v for k,v in old.items() if k!='size'} == {k:v for k,v in new.items() if k!='size'}
                print(f'{case}: existing files and partition identity preserved after shrink', flush=True)
            key = base64.b64encode(password.encode()).decode()
            vm.secrets.add(key.encode())
            unlock = ("python3 - <<'PY'\nimport subprocess,base64\n"
                      "device=subprocess.check_output(['blkid','-t','TYPE=crypto_LUKS','-o','device'],text=True).strip()\n"
                      f"subprocess.run(['cryptsetup','open','--key-file','-',device,'test-root'],input=base64.b64decode('{key}'),check=True)\nPY") if encrypted else ''
            root = '/dev/mapper/test-root' if encrypted else '"$(blkid -L cenere-system)"'
            vm.command(f'''set -eu
{unlock}
mkdir /run/installed
mount -t btrfs -o subvol=@ {root} /run/installed
mount -t btrfs -o subvol=@home {root} /run/installed/home
mount "$(awk '$2=="/boot" {{print $1}}' /run/installed/etc/fstab)" /run/installed/boot
mount "$(awk '$2=="/boot/efi" {{print $1}}' /run/installed/etc/fstab)" /run/installed/boot/efi
cat >> /run/installed/etc/default/grub <<'SERIAL'
GRUB_TERMINAL_INPUT="console serial"
GRUB_TERMINAL_OUTPUT="console serial"
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"
GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0,115200 systemd.debug_shell=ttyS0 systemd.mask=serial-getty@ttyS0.service systemd.unit=multi-user.target"
SERIAL
arch-chroot /run/installed grub-mkconfig -o /boot/grub/grub.cfg
sync
umount -R /run/installed
{'cryptsetup close test-root' if encrypted else ''}
''', 180)
        print(f'{case}: boot installed disk', flush=True)
        with VM(directory, firmware=firmware) as vm:
            vm.boot_installed(passphrase=password if encrypted else None)
            vm.command('''set -eu
test "$(findmnt -n -o FSTYPE /)" = btrfs
test "$(findmnt -n -o FSTYPE /home)" = btrfs
test "$(findmnt -n -o UUID /)" = "$(findmnt -n -o UUID /home)"
id person
systemctl is-active multi-user.target
! pacman --config /dev/null -Q cenere >/dev/null 2>&1
''')
        return {'case': case, 'firmware': firmware, 'encrypted': encrypted,
                'installed_boot': True, 'shared_btrfs': True,
                'installer_sha256': hashlib.sha256(binary).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--installer', type=Path, default=Path(__file__).resolve().parents[1]/'cenere')
    parser.add_argument('--case', choices=('fresh','alongside-ext4','alongside-ntfs','unsupported-home'), default='fresh')
    parser.add_argument('--encrypt', action='store_true')
    parser.add_argument('--firmware', choices=('uefi','bios'), default='uefi')
    args = parser.parse_args()
    print(json.dumps(run_case(args.iso, args.installer, args.case, args.encrypt, args.firmware), indent=2))


if __name__ == '__main__':
    main()
