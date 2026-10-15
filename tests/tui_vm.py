#!/usr/bin/env python3
"""Interactive diagnostic of the real desktop launcher and installer TUI.

Run with `python3 -B -i tests/tui_vm.py ISO --directory "$ANSER_VM_DIR/..."`.
The REPL exposes vm, password, capture(name), observe(name) and launch().
Type through vm.keyboard(); the actual desktop launcher receives all input.
All disks and evidence are disposable files beneath the configured ANSER_VM_DIR. No production
installer binary or image is changed.
"""

import argparse
import atexit
from pathlib import Path
import secrets

from vm import VM, begin_scope


def capture(name):
    print(vm.screenshot(name + ".png"))


def observe(name):
    output = vm.command(r'''date -u
ps -ww -eo user,pid,ppid,stat,etime,time,wchan:24,args --forest
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS
for process in $(pgrep -x cenere; pgrep -x rsync; pgrep -x mkfs.btrfs; pgrep -x mkinitcpio; pgrep -x grub-install); do
    printf '\nPROCESS %s\n' "$process"
    cat /proc/$process/io
    readlink /proc/$process/fd/0 /proc/$process/fd/1 /proc/$process/fd/2
    terminal=$(readlink /proc/$process/fd/0)
    case "$terminal" in /dev/pts/*) stty -F "$terminal" -a;; esac
done
true''', 30)
    (vm.directory / (name + ".txt")).write_text(output)
    print(output[-4500:])


def launch():
    output = vm.command(r'''set -eu
for socket in /run/user/1000/wayland-*; do test -S "$socket" && break; done
runuser -u liveuser -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY="$(basename "$socket")" DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus gio launch /usr/share/applications/cenere.desktop >/run/cenere-launcher.log 2>&1
sleep 4
pgrep -a -x cenere
cat /run/cenere-launcher.log
''')
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    begin_scope(args.directory)
    vm = VM(args.directory, iso=args.iso, graphics=True).start()
    atexit.register(vm.close)
    password = secrets.token_hex(16)
    vm.secrets.add(password.encode())
    vm.boot_iso(graphical=True)
    evidence = vm.command("systemctl --failed --no-pager; journalctl -b -p warning --no-pager; "
                          "gnome-shell --version; true", 90)
    (vm.directory / "boot-warnings.txt").write_text(evidence)
    print("Boot evidence saved; launch() opens the real desktop installer.")
