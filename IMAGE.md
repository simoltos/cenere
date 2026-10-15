# Prepare a system image

Use an x86-64 system with pacman, systemd, mkinitcpio, and GRUB.

## Build and package Cenere

Use a C11 compiler, make, and the json-c development files.
Run this command in the source directory:

```sh
make
```

Package `./cenere` as `cenere`, with the executable at `/usr/bin/cenere`.
Install that package and its runtime dependencies in the live system.

Include these installation utilities in `/usr/bin`:

```text
arch-chroot   blkid       btrfs       cryptsetup   e2fsck
grub-install  loadkeys    mkfs.btrfs  mkfs.ext4    mkfs.fat
ntfsresize    resize2fs   rsync       sfdisk       udevadm
wipefs
```

Also include json-c and the standard utilities from the base system.
Supply the GRUB firmware support files.
For coexistence with another system, include `os-prober`.
Supply console keymaps, systemd's `kbd-model-map`, and the XKB data from xkeyboard-config.
Include the XKB `evdev.lst` file for keyboard names in the interface.

## Select the final system

Include `base`, `pacman`, `systemd`, `mkinitcpio`, `grub`, and an installable kernel in the final package set.
Keep `pkgbase` and `vmlinuz` for each kernel under `/usr/lib/modules/<version>/`.
Supply mkinitcpio presets that produce `/boot/initramfs-<pkgbase>.img`.
Use the standard mkinitcpio configuration for those presets.

Supply the distribution's repository configuration and public vendor keyrings.
Cenere initializes pacman trust offline from those keyrings.
For package access after installation, configure repositories that the installed system can reach.

## Record the package sets

Resolve the final system's dependencies from an empty package database.
Resolve the live system's dependencies from a separate empty database.
Use the same repository snapshot for both sets.
Use identical versions for packages in both sets.

Create these files in `/usr/share/cenere/` in the prepared system:

| File | Contents |
| --- | --- |
| `packages.final` | Every final package and its exact version, as `name version`, one pair per line. |
| `packages.explicit` | Explicitly selected final package names, one per line. |
| `packages.live-only` | Package names present only in the live system, one per line. |

Sort each file by package name with `LC_ALL=C`.
Use unique entries.
End each file with a newline.
Do not include comments or blank lines.
Keep at least one entry in each file.

Include every explicit package in the final set.
Do not put a package in both the final and live-only sets.
Together, these sets must list all packages in the prepared system.
Put `cenere` and dependencies exclusive to the live environment in `packages.live-only`.
Keep shared dependencies in `packages.final`.

## Add the image descriptor

Create `/usr/share/cenere/image.json` in the prepared system:

```json
{"version": 1, "id": "arch", "live_user": ""}
```

| Field | Value |
| --- | --- |
| `version` | The number `1`. |
| `id` | The distribution identifier. Use 1 to 63 characters from `a-z`, `0-9`, `-`, and `_`. |
| `live_user` | The temporary live account. Use at most 31 characters from the same character set. Use an empty string for none. Never use `root`. |

Cenere uses `id` for the GRUB identifier and the default computer name.
It removes the specified live account during installation.

## Make the source available

If a distribution hook is necessary, add it as described in [INTEGRATION.md](INTEGRATION.md).
Put all installation defaults in the prepared system before you create the SquashFS.
Create the SquashFS from that system, with the live packages, descriptor, and manifests included.

Configure live boot to mount this SquashFS read-only.
Keep it mounted while Cenere runs.
Cenere finds this source through the mount table and the descriptor.
Changes in the live session's writable overlay do not enter the installed system.

Complete the [integration checks](INTEGRATION.md#do-the-integration-checks) with the finished image.
Compare the installed package names and versions with `packages.final` from that image.
