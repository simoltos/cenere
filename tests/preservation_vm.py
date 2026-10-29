#!/usr/bin/env python3
"""Install and boot disposable home-preservation fixtures from an Anser ISO."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import tempfile

from vm import VM, VM_ROOT


def result(output, marker):
    match = re.search(re.escape(marker) + r"=(\{[^\r\n]*\})", output)
    if not match:
        raise AssertionError(f"Guest did not return {marker}: {output[-1500:]}")
    return json.loads(match[1])


SNAPSHOT = r'''
def snapshot(root):
    data = {}
    for p in [root, *sorted(root.rglob('*'))]:
        if str(p.relative_to(root))=='.config/dconf/user':
            continue  # Keyboard changes are checked semantically after boot.
        st = p.lstat()
        data[str(p.relative_to(root))] = {
            'mode': st.st_mode, 'uid': st.st_uid, 'gid': st.st_gid,
            'inode': st.st_ino, 'mtime': st.st_mtime_ns if not p.is_dir() else None,
            'content': os.readlink(p) if p.is_symlink() else
                       hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None,
            'xattrs': {n: base64.b64encode(os.getxattr(p, n, follow_symlinks=False)).decode()
                       for n in os.listxattr(p, follow_symlinks=False)}
        }
    return data
'''


def subvolumes(case):
    if 'anser' in case:
        return '@', '@home'
    if case.startswith('encrypted-'):
        return 'root', 'home'
    return 'system-volume', 'personal-data'


def create_fixture(vm, case, password):
    encrypted = case.startswith('encrypted-')
    copy_home = case == 'copy-btrfs-encrypted'
    root_subvol, home_subvol = subvolumes(case)
    encoded = base64.b64encode(password.encode()).decode()
    vm.secrets.update((password.encode(), encoded.encode()))
    script = r'''set -eu
test "$(lsblk -dn -o TYPE /dev/vda)" = disk
test ! -e /dev/vda1
sfdisk /dev/vda <<'PARTITIONS'
label: gpt
size=600MiB,type=U
size=2GiB,type=L
ROOT_PARTITION
PARTITIONS
udevadm settle
mkfs.fat -F32 /dev/vda1
mkfs.ext4 -F -m0 /dev/vda2
python3 - <<'PY'
import base64, subprocess
if ENCRYPTED:
    password=base64.b64decode("ENCODED")
    subprocess.run(['cryptsetup','luksFormat','--type','luks2','--batch-mode','--key-file','-',
                    '--iter-time','100','/dev/vda3'],input=password,check=True)
    subprocess.run(['cryptsetup','open','--key-file','-','/dev/vda3','fixture-root'],input=password,check=True)
PY
root_device=ROOT_DEVICE
mkfs.btrfs -f "$root_device"
mkdir -p /run/fixture /run/fixture-home /run/fixture-boot /run/fixture-efi
mount "$root_device" /run/fixture
BTRFS_PREPARE
mkdir -p /run/fixture/etc /run/fixture/usr/bin /run/fixture/home
HOME_MOUNT
mount /dev/vda2 /run/fixture-boot
mount /dev/vda1 /run/fixture-efi
python3 - <<'PY'
import os, subprocess
from pathlib import Path
root=Path('/run/fixture')
uuid=lambda dev:subprocess.check_output(['blkid','-s','UUID','-o','value',dev],text=True).strip()
root_uuid=uuid('ROOT_DEVICE')
home_uuid=root_uuid
root_type='btrfs'
root_options='subvol=ROOT_SUBVOL'
fstab=f'UUID={root_uuid} / {root_type} {root_options} 0 0\n'
fstab+=f'UUID={home_uuid} /home btrfs subvol=HOME_SUBVOL 0 0\n'
fstab+=f'UUID={uuid("/dev/vda2")} /boot ext4 defaults 0 2\n'
fstab+=f'UUID={uuid("/dev/vda1")} /boot/efi vfat defaults 0 2\n'
(root/'etc/fstab').write_text(fstab)
(root/'etc/os-release').write_text('ID=fedora\nPRETTY_NAME="Foreign Linux fixture"\n')
home=Path('/run/fixture-home/person')
home.mkdir(mode=0o750)
(home/'.bashrc').write_bytes(b'preserved shell settings\n')
file=home/'document with spaces\nand\ttabs'
file.write_bytes(b'personal data\x00\xff\n'*4096)
file.chmod(0o640)
os.setxattr(file,'user.cenere-test',b'preserve metadata')
subprocess.run(['setfacl','-m','u:2044:r',str(file)],check=True)
os.link(file,home/'hardlink')
(home/'symlink').symlink_to(file.name)
(home/'large-file').write_bytes(os.urandom(1024*1024))
with (home/'sparse-file').open('wb') as sparse:
    sparse.seek(8*1024*1024)
    sparse.write(b'preserved sparse tail')
for p in [home,*home.iterdir()]:
    os.chown(p,2042,2043,follow_symlinks=False)
    os.utime(p,ns=(1700000000000000000,1700000000000000000),follow_symlinks=False)
boot=Path('/run/fixture-boot')
(boot/'vmlinuz-6.18-fixture').write_bytes(b'old non-Arch kernel')
(boot/'initramfs-6.18-fixture.img').write_bytes(b'old non-Arch initramfs')
(boot/'keep-boot-data').write_bytes(b'unrelated boot data')
efi=Path('/run/fixture-efi/EFI/OtherOS');efi.mkdir(parents=True)
(efi/'keep.efi').write_bytes(b'preserve other EFI files')
PY
groupadd -g 2043 fixture-owner
useradd -u 2042 -g 2043 -M -d KEYBOARD_HOME fixture-owner
runuser -u fixture-owner -- env GIO_USE_VFS=local dbus-run-session -- sh -c 'gsettings set org.gnome.desktop.input-sources sources "[(\"xkb\", \"it\")]"; gsettings set org.gnome.desktop.interface clock-show-seconds true'
loginctl terminate-user fixture-owner
systemctl stop user@2042.service
userdel fixture-owner
if getent group fixture-owner >/dev/null; then groupdel fixture-owner; fi
python3 - <<'PY'
import os,json,hashlib,base64,subprocess
from pathlib import Path
SNAPSHOT_CODE
home=Path('/run/fixture-home/person')
data={'home':snapshot(home),'layout':json.loads(subprocess.check_output(['sfdisk','--json','/dev/vda'])),
      'uuid':subprocess.check_output(['blkid','-s','UUID','-o','value','ROOT_DEVICE'],text=True).strip()}
print('FIXTURE='+json.dumps(data))
PY
sync
HOME_UNMOUNT
umount /run/fixture /run/fixture-boot /run/fixture-efi
CLOSE_ROOT
'''
    values = {
        'ROOT_PARTITION': 'size=24GiB,type=L' if copy_home else 'type=L',
        'ENCRYPTED': str(encrypted), 'ENCODED': encoded,
        'ROOT_DEVICE': '/dev/mapper/fixture-root' if encrypted else '/dev/vda3',
        'BTRFS_PREPARE': (f'btrfs subvolume create /run/fixture/{root_subvol}\n'
                          f'btrfs subvolume create /run/fixture/{home_subvol}\n'
                          f'umount /run/fixture\nmount -t btrfs -o subvol={root_subvol} "$root_device" /run/fixture'),
        'HOME_MOUNT': f'mount -t btrfs -o subvol={home_subvol} "$root_device" /run/fixture-home',
        'HOME_UNMOUNT': 'umount /run/fixture-home',
        'CLOSE_ROOT': 'cryptsetup close fixture-root' if encrypted else '',
        'ROOT_SUBVOL': root_subvol, 'HOME_SUBVOL': home_subvol,
        'KEYBOARD_HOME': '/run/fixture-home/person', 'SNAPSHOT_CODE': SNAPSHOT,
    }
    for key, value in values.items():
        script = script.replace(key, value)
    return result(vm.command(script, 180), 'FIXTURE')


def run_case(iso, installer, case, interrupt=False, copy_to_ram=False, packaged=False):
    encrypted = case.startswith('encrypted-')
    copy_home = case == 'copy-btrfs-encrypted'
    target_encrypted = encrypted or copy_home
    if interrupt and copy_home:
        raise ValueError('Interruption check requires an in-place root fixture')
    root_subvol, home_subvol = subvolumes(case)
    disk_password = secrets.token_hex(12)
    user_password = secrets.token_hex(12)
    VM_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='preservation-', dir=VM_ROOT) as temporary:
        directory = Path(temporary)
        media = None
        if not packaged:
            executable = directory/'cenere.arch'
            executable.write_bytes(installer.read_bytes())
            data = executable.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            media = directory/'installer-readonly.raw'
            media.write_bytes(data+b'\0'*(-len(data)%512))
        subprocess.run(['qemu-img','create','-f','qcow2',str(directory/'disk.qcow2'),'512110190592'],
                       check=True,stdout=subprocess.DEVNULL)
        print(f'{case}: boot live',flush=True)
        with VM(directory,iso=iso,upload_image=media,memory=6144) as vm:
            vm.boot_iso(copy_to_ram=copy_to_ram)
            if copy_to_ram:
                vm.command('test -s /run/archiso/copytoram/airootfs.sfs')
                phase=re.search(rb'Copying rootfs image to RAM.*?done\.',
                                (directory/'serial.log').read_bytes(),re.S)
                assert phase and b'100%' in phase[0], 'Copy-to-RAM progress was not displayed'
            if packaged:
                output=vm.command('sha256sum /usr/bin/cenere')
                digest=re.search(r'([0-9a-f]{64})\s+/usr/bin/cenere',output)[1]
            else:
                vm.upload(executable)
            program='/usr/bin/cenere' if packaged else '/run/cenere-test'
            before = create_fixture(vm,case,disk_password)
            answers=['us','keyboard test']
            if encrypted:answers+=['unlock',disk_password]
            answers+=['replace','yes','yes' if target_encrypted else 'no','en_US.UTF-8','UTC',
                      'anser-fixture','person',user_password,user_password,'INSTALL']
            encoded=base64.b64encode(('\n'.join(answers)+'\n').encode()).decode()
            vm.secrets.update((disk_password.encode(),user_password.encode(),encoded.encode()))
            if interrupt:
                vm.command(r'''set -eu
cp /usr/bin/rsync /run/fixture-rsync
cat > /run/fixture-failed-transfer <<'SCRIPT'
#!/bin/sh
for argument do
    case "$argument" in /run/archiso/airootfs/) exit 37 ;; esac
done
exec /run/fixture-rsync "$@"
SCRIPT
chmod 755 /run/fixture-failed-transfer
mount --bind /run/fixture-failed-transfer /usr/bin/rsync
''')
            print(f'{case}: install with preserved home',flush=True)
            vm.command('python3 - <<\'PY\'\nimport base64,subprocess\n'
                       f'p=subprocess.run(["{program}","--install","/dev/vda"],input=base64.b64decode("{encoded}"))\n'
                       f'assert p.returncode=={1 if interrupt else 0},p.returncode\nPY',900)
            boot_password=user_password if copy_home else disk_password
            key=base64.b64encode(boot_password.encode()).decode()
            vm.secrets.add(key.encode())
            open_root = ('python3 - <<\'PY\'\nimport base64,subprocess\n'
                         f'device=subprocess.check_output(["blkid","-t","TYPE=crypto_LUKS","-o","device"],text=True).strip()\nsubprocess.run(["cryptsetup","open","--key-file","-",device,"fixture-check"],input=base64.b64decode("{key}"),check=True)\nPY') if target_encrypted else ''
            if interrupt:
                vm.command('umount /usr/bin/rsync')
                source='/dev/mapper/fixture-check' if encrypted else '/dev/vda3'
                home_path=f'/run/retained/{home_subvol}/person'
                old_root='/run/retained/@cenere-previous'
                output=vm.command(f'''set -eu
{open_root}
mkdir -p /run/retained
mount -t btrfs -o ro,rescue=nologreplay,subvolid=5 {source} /run/retained
python3 - <<'PY'
import os,json,hashlib,base64
from pathlib import Path
{SNAPSHOT}
assert 'ID=fedora' in Path('{old_root}/etc/os-release').read_text()
assert Path('/run/retained/.cenere-boot-backup/vmlinuz-6.18-fixture').read_bytes()==b'old non-Arch kernel'
print('RECOVERY='+json.dumps({{'home':snapshot(Path('{home_path}'))}}))
PY
umount /run/retained
{'cryptsetup close fixture-check' if encrypted else ''}
''')
                assert result(output,'RECOVERY')['home']==before['home']
                retry=base64.b64encode(('\n'.join(['us','keyboard test']+
                                      (['unlock',disk_password] if encrypted else []))+'\n').encode()).decode()
                vm.secrets.add(retry.encode())
                vm.command('python3 - <<\'PY\'\nimport base64,subprocess\n'
                           f'p=subprocess.run(["{program}","--install","/dev/vda"],input=base64.b64decode("{retry}"),capture_output=True)\n'
                           'assert p.returncode!=0\nassert b"recovery" in p.stderr.lower(),p.stderr\n'
                           'print("Interrupted installation requires recovery before retry")\nPY')
                return {'case':case,'interruption_recovery':True,'home_contents_and_metadata':True,
                        'installer_sha256':digest}
            root_device='/dev/mapper/fixture-check' if target_encrypted else '/dev/vda3'
            installed_root, installed_home = ('@','@home') if copy_home else (root_subvol,home_subvol)
            mount_root=f'mount -t btrfs -o subvol={installed_root} {root_device} /run/installed'
            mount_home=f'mount -t btrfs -o subvol={installed_home} {root_device} /run/installed/home'
            output=vm.command(f'''set -eu
mkdir -p /run/installed
{open_root}
{mount_root}
{mount_home}
mount "$(awk '$2=="/boot" {{print $1}}' /run/installed/etc/fstab)" /run/installed/boot
mount /dev/vda1 /run/installed/boot/efi
python3 - <<'PY'
import os,json,hashlib,base64,subprocess
from pathlib import Path
{SNAPSHOT}
r=Path('/run/installed')
assert 'ID=anser' in (r/'etc/os-release').read_text()
assert (r/'boot/efi/EFI/OtherOS/keep.efi').read_bytes()==b'preserve other EFI files'
assert any((r/'boot/efi/EFI').glob('anser*/grubx64.efi'))
assert not (r/'boot/vmlinuz-6.18-fixture').exists()
if {not copy_home!r}:assert (r/'boot/keep-boot-data').read_bytes()==b'unrelated boot data'
data={{'home':snapshot(r/'home/person'),'layout':json.loads(subprocess.check_output(['sfdisk','--json','/dev/vda']))}}
print('INSTALLED='+json.dumps(data))
PY
cat >> /run/installed/etc/default/grub <<'SERIAL'
GRUB_TERMINAL_INPUT="console serial"
GRUB_TERMINAL_OUTPUT="console serial"
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"
GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0,115200 systemd.debug_shell=ttyS0 systemd.mask=serial-getty@ttyS0.service systemd.unit=multi-user.target"
SERIAL
arch-chroot /run/installed grub-mkconfig -o /boot/grub/grub.cfg
sync
umount -R /run/installed
{'cryptsetup close fixture-check' if target_encrypted else ''}
''',180)
            after=result(output,'INSTALLED')
            for name, metadata in before['home'].items():
                actual=after['home'].get(name)
                if copy_home:
                    actual={k:v for k,v in actual.items() if k!='inode'}
                    metadata={k:v for k,v in metadata.items() if k!='inode'}
                assert actual==metadata, (case,name,metadata,actual)
            assert after['home']['hardlink']['inode']==after['home']['document with spaces\nand\ttabs']['inode']
            old_parts=before['layout']['partitiontable']['partitions']
            new_parts=after['layout']['partitiontable']['partitions']
            if not copy_home:
                assert old_parts==new_parts, 'Reused partition table changed'
            else:
                assert old_parts[-1] in new_parts, 'Preserved source partition changed'
        print(f'{case}: boot installed disk',flush=True)
        with VM(directory,memory=6144) as vm:
            vm.boot_installed(passphrase=boot_password if target_encrypted else None)
            output=vm.command("set -eu\nfindmnt /\nfindmnt -T /home/person\n"
                              "test \"$(id -u person)\" = 2042\ntest \"$(id -g person)\" = 2043\n"
                              "test -f /home/person/.bashrc\n! pacman --config /dev/null -Q cenere >/dev/null 2>&1\n"
                              "test ! -e /.cenere-previous\nsystemctl is-active multi-user.target\n")
            print(output,flush=True)
            if copy_to_ram:
                vm.command('! pacman --config /dev/null -Q pv >/dev/null 2>&1')
            vm.command(r'''python3 - <<'PY'
import subprocess
base=['runuser','-u','person','--','env','GIO_USE_VFS=local','dbus-run-session','--','gsettings','get']
assert subprocess.check_output(base+['org.gnome.desktop.input-sources','sources'],text=True).strip()=="[('xkb', 'us')]"
assert subprocess.check_output(base+['org.gnome.desktop.interface','clock-show-seconds'],text=True).strip()=='true'
print('Selected keyboard applied; unrelated desktop preference preserved')
PY''')
        return {'case':case,'home_contents_and_metadata':True,'installed_boot':True,
                'keyboard_and_other_settings':True,'installer_sha256':digest,
                'copy_to_ram_progress':copy_to_ram,'packaged_installer':packaged}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso',type=Path)
    parser.add_argument('--installer',type=Path,default=Path(__file__).resolve().parents[1]/'cenere')
    parser.add_argument('--case',choices=['btrfs','encrypted-btrfs','anser-btrfs','encrypted-anser-btrfs','copy-btrfs-encrypted'],
                        default='encrypted-btrfs')
    parser.add_argument('--interrupt',action='store_true',help='Fail system copying and verify recoverable data')
    parser.add_argument('--copy-to-ram',action='store_true',help='Verify boot copy progress with pv')
    parser.add_argument('--packaged',action='store_true',help='Use the installer shipped in the ISO')
    args=parser.parse_args()
    print(json.dumps(run_case(args.iso,args.installer,args.case,args.interrupt,args.copy_to_ram,args.packaged),indent=2))


if __name__=='__main__':
    main()
