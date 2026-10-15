# Cenere

Offline installer for Arch-based distributions, written in C11.

## Structure

- `cenere.c`: CLI, terminal wizard, and mounted-target copy.
- `cenere_install.c` / `.h`: installation backend and shared interface.
- `tests/`: Python regression suites and disposable-VM drivers.

## Development

- Build with `make`. See `make help` for prerequisites.
- Use `./cenere --demo` to check the wizard without installing.
- Run `make test` after code changes. Report checks that could not run.
- Add behavioral regressions to `test_cenere.py` for CLI, wizard, and copy;
  `test_runner.py` for command execution; `test_storage.py` for storage planning.
- Follow existing formatting: four spaces in C and Python, tabs in Make recipes.
- Use `snake_case`, keep internal helpers `static`, and prefix public backend
  functions with `cenere_`.
- Keep builds free of warnings under `-Wall -Wextra -Wpedantic`.
- Read and update `IMAGE.md` or `INTEGRATION.md` when their contracts change.

## Installer safety

- Test installation and boot changes only on disposable virtual disks,
  following `INTEGRATION.md`. See `tests/vm.py` for VM prerequisites.
- Preserve explicit installation confirmation and home-preservation checks.
- Keep passwords hidden and redacted from logs and diagnostics.
- Pass external command arguments directly, without a shell.
