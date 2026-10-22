# Distribution integration

Use [IMAGE.md](IMAGE.md) for build instructions and image requirements.

## Add the hook

If distribution-specific changes are necessary, include `/usr/lib/cenere/integration` in the prepared image.
Use a regular file owned by root.
Set its mode to `0755`.
If no changes are necessary, the hook is optional.

Use this argument interface:

```text
integration keyboard  LIVE_USER XKB_LAYOUT XKB_MODEL XKB_VARIANT XKB_OPTIONS
integration configure USER      XKB_LAYOUT XKB_MODEL XKB_VARIANT XKB_OPTIONS
```

`LIVE_USER` is the account from the image descriptor and can be empty.
`USER` is the selected account for the installed system.
The XKB arguments give the selected keyboard mapping.
The variant and option arguments can be empty.

Cenere runs the hook as root and passes each argument directly, without a shell.
Return zero after success.
Return a nonzero status after a failure.
A nonzero status stops the operation.

| Action | Execution context | Execution order |
| --- | --- | --- |
| `keyboard` | Live environment; executable from the source image. | After system keyboard selection, before password entry. |
| `configure` | Inside the installed root. | After account creation, before initramfs generation, GRUB setup, and removal of live-only packages. |

The terminal wizard keeps the layout, variant, and optional typing test on one page.
Confirming the selection applies it before enabling the test field; Enter can skip typing.
Confirming a different mapping runs `keyboard` again and clears previously entered passwords.

## Configure the distribution

For a graphical image, apply the selected mapping in these locations:

- During `keyboard`: the live desktop account.
- During `configure`: the installed desktop account and graphical login.

Use the correct user identity, home directory, and D-Bus session for desktop settings.
In a preserved home, change only keyboard preferences.
Keep other settings and file ownership unchanged.
Supply keyboard defaults for accounts created later.

Use `configure` for the distribution's services and administrator policy.
Cenere creates the account with Bash and membership in `wheel`.
It locks the root account.
Configure administrator access for the new account.

Remove live autologin and other settings exclusive to the live session.
For an Archiso image, remove Archiso initramfs overrides from the installed system.
Set the installed system's default target.
Enable the services that the distribution selects.

## Do the integration checks

Use disposable virtual disks for these checks.
Run `cenere` as root in a terminal in the prepared live environment.

For both plain and encrypted installations, do these checks:

1. Before password entry, make sure the selected keyboard mapping is active, including when the optional typing test is skipped.
2. Complete the installation.
3. Boot the installed disk without the installation medium.
4. For an encrypted installation, do a keyboard test at disk unlock.
5. For a graphical system, do keyboard tests at graphical login and in the desktop.
6. Make sure the new account has the selected administrator access.
7. Make sure the selected services start.
8. Make sure live autologin is absent.

For a home-preservation test, select a keyboard mapping that differs from the existing desktop preference.
Make sure the selected mapping replaces that preference.
Make sure other settings and file ownership stay unchanged.

If the hook fails, read `/var/log/cenere/latest.log` in the live environment.
