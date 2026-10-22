#!/usr/bin/env python3
"""Exercise the installer with real rsync and disposable Linux mounts.

Run through `make test`. Bubblewrap exposes the host filesystem read-only,
provides private /tmp and /run filesystems, and disables network access.
Only the sandbox has root privileges; no host mount or disk is modified.
"""

import argparse
import fcntl
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest


PROGRAM = None


def demo_answers(**changes):
    """Answer the complete demo wizard, with explicit review and confirmation."""
    choices = dict(preserve="yes", encrypt="no", keymap="us", keyboard_test="layout test", locale="en_US.UTF-8",
                   timezone="UTC", hostname="new-system", username="user",
                   password="sample secret", password_confirmation="sample secret",
                   review="continue", confirmation="INSTALL")
    choices.update(changes)
    if choices["review"] in ("edit", "cancel"):
        choices["confirmation"]=choices["review"]
    names = ["keymap", "keyboard_test", "preserve", "encrypt", "locale", "timezone", "hostname", "username",
             "password", "password_confirmation", "confirmation"]
    return "\n".join(choices[name] for name in names) + "\n"


def run_installer(*arguments, answer="", extra_env=None):
    return subprocess.run(
        [str(PROGRAM), *map(str, arguments)],
        input=answer,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=15,
        env={**os.environ, "LC_ALL": "C", **(extra_env or {})},
    )


def command(*arguments):
    subprocess.run(list(map(str, arguments)), check=True, timeout=15)


def snapshot(root):
    """Record contents and metadata without following symbolic links."""
    result = {}
    for path in [root, *sorted(root.rglob("*"))]:
        metadata = path.lstat()
        contents = None
        if path.is_symlink():
            contents = os.readlink(path)
        elif path.is_file():
            contents = path.read_bytes()
        result[str(path.relative_to(root))] = (
            metadata.st_mode,
            metadata.st_uid,
            metadata.st_gid,
            metadata.st_mtime_ns,
            contents,
        )
    return result


class CommandLineTests(unittest.TestCase):
    def test_usage(self):
        result = run_installer("--unknown")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("Usage:", result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "requires an unprivileged test launcher")
    def test_requires_root(self):
        result = run_installer(".", ".", answer="INSTALL\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Run this installer as root.", result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "requires an unprivileged test launcher")
    def test_default_mode_requires_root_and_does_not_simulate(self):
        result = run_installer(answer=demo_answers())
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("[SIMULATED]", result.stdout)

    def test_help(self):
        result = run_installer("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--demo", result.stderr)
        self.assertIn("--install DISK", result.stderr)

    def test_install_requires_exactly_one_disk_argument(self):
        for arguments in (("--install",), ("--install", "/dev/example", "extra")):
            with self.subTest(arguments=arguments):
                result = run_installer(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("Usage:", result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "requires an unprivileged test launcher")
    def test_whole_disk_install_requires_root(self):
        result = run_installer("--install", "/dev/example", answer=demo_answers())
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Run this installer as root.", result.stderr)
        self.assertNotIn("[SIMULATED]", result.stdout)

    def test_demo_rejects_real_paths_and_other_options(self):
        for arguments in (("--demo", "/"), ("--demo", "/", "/"),
                          ("--demo", "--help"), ("--demo", "--demo")):
            with self.subTest(arguments=arguments):
                result = run_installer(*arguments, answer=demo_answers())
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("[SIMULATED]", result.stdout)


class DemoTests(unittest.TestCase):
    def assert_demo_stopped(self, result):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("[SIMULATED]", result.stdout)
        self.assertNotIn("Simulation completed", result.stdout)
        self.assertNotIn("Filesystem transfer completed", result.stdout)

    def test_complete_demo_uses_choices_and_ordered_stages(self):
        result = run_installer("--demo", answer=demo_answers(
            keymap="it", locale="it_IT.UTF-8", timezone="Europe/Rome",
            hostname="demo-anser", username="person"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DEMO MODE", result.stderr)
        self.assertIn("it_IT.UTF-8", result.stdout)
        self.assertIn("Europe/Rome", result.stdout)
        self.assertIn("user person", result.stdout)
        stages = ["Keep the original home", "Prepare a Btrfs root", "Create the home subvolume",
                  "Mount root", "Copy and verify preserved", "Copy the prepared", "Configure /etc/fstab",
                  "Set keymap", "Set hostname", "Configure user", "Generate the target",
                  "Configure grub"]
        simulated = [line for line in result.stdout.splitlines() if line.startswith("[SIMULATED]")]
        self.assertEqual(len(simulated), len(stages))
        for stage, line in zip(stages, simulated):
            self.assertIn(stage, line)
        self.assertIn("Simulation completed. No system was installed.", result.stdout)
        self.assertNotIn("Filesystem transfer completed", result.stdout)

    def test_defaults_work_without_a_payload_or_target(self):
        result = run_installer("--demo", answer=demo_answers(keymap="", keyboard_test="", preserve="", encrypt="", locale="", timezone="", hostname="", username=""))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Simulation completed", result.stdout)

    def test_policy_details_appear_only_in_execution_output(self):
        result = run_installer("--demo", answer=demo_answers())
        self.assertEqual(result.returncode, 0, result.stderr)
        for removed in ("Installation task", "complete/transfer", "Home filesystem layout",
                        "Administrator access", "Demo firmware mode", "Boot loader"):
            self.assertNotIn(removed, result.stderr)
        for detail in ("share free space", "root account locked", "grub", "uefi"):
            self.assertNotIn(detail, result.stderr)
            self.assertIn(detail, result.stdout)
        self.assertIn("The reviewed root will be replaced", result.stderr)
        self.assertIn("/home will be preserved", result.stderr)
        self.assertNotIn("sample secret", result.stdout + result.stderr)

    def test_edit_rebuilds_plan_before_confirmation(self):
        answers = demo_answers(review="edit").rsplit("INSTALL\n", 1)[0]
        result = run_installer("--demo", answer=answers + demo_answers(hostname="edited-host"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count("DEMO installation review"), 2)
        self.assertIn("Set hostname edited-host", result.stdout)

    def test_edit_can_keep_password_without_showing_it(self):
        answers = demo_answers(review="edit").rsplit("INSTALL\n", 1)[0]
        result = run_installer("--demo", answer=answers + "\n" * 9 + "INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Enter to keep current", result.stderr)
        self.assertNotIn("sample secret", result.stdout + result.stderr)

    def test_cancellation_eof_and_exact_confirmation(self):
        for answer in ("", "us\ncancel\n", demo_answers(review="cancel"),
                       "\n" * 7 + "cancel\n", "\n" * 7 + "sample secret\ncancel\n",
                       demo_answers(confirmation=""), demo_answers(confirmation="yes"),
                       demo_answers(confirmation="install"), demo_answers(confirmation="INSTALL "),
                       demo_answers(confirmation="INSTALL\x00extra")):
            with self.subTest(answer=answer):
                self.assert_demo_stopped(run_installer("--demo", answer=answer))

    def test_invalid_question_values_are_reprompted(self):
        answers = demo_answers(hostname="Bad Host!\ndemo-host", username="root\nperson")
        result = run_installer("--demo", answer=answers)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Invalid value", result.stderr)
        self.assertIn("Set hostname demo-host", result.stdout)
        self.assertIn("user person", result.stdout)

    def test_configuration_names_reject_traversal_and_shell_syntax(self):
        for field, invalid, valid in (("keymap", "us;touch /tmp/unsafe", "us"),
                                      ("locale", "../../etc/passwd", "en_US.UTF-8"),
                                      ("timezone", "/etc/shadow", "UTC")):
            with self.subTest(field=field):
                result = run_installer("--demo", answer=demo_answers(**{field: invalid + "\n" + valid}))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Invalid value", result.stderr)
                self.assertNotIn(invalid, result.stdout)

    def test_password_mismatch_requires_a_new_matching_pair(self):
        result = run_installer("--demo", answer=demo_answers(
            password="first secret\nwrong secret\nsecond secret", password_confirmation="second secret"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Passwords do not match", result.stderr)
        for secret in ("first secret", "wrong secret", "second secret"):
            self.assertNotIn(secret, result.stdout + result.stderr)

    def test_empty_password_requires_input(self):
        result = run_installer("--demo", answer=demo_answers(password="\nsample secret"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Enter a value for User password", result.stderr)

    def test_invalid_password_input_cannot_be_truncated_into_confirmation(self):
        for password in ("invalid\x00secret", "x" * 256):
            with self.subTest(length=len(password)):
                result = run_installer("--demo", answer=demo_answers(password=password))
                self.assert_demo_stopped(result)
                self.assertNotIn(password, result.stdout + result.stderr)

    def test_replace_existing_home_with_one_encrypted_shared_filesystem(self):
        result = run_installer("--demo", answer=demo_answers(
            preserve="no", encrypt="yes"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("/home will be replaced", result.stderr)
        self.assertIn("Use one encrypted container for both the system and /home", result.stdout)
        self.assertIn("existing home files will be replaced", result.stdout)
        self.assertLess(result.stdout.index("Use one encrypted container"),
                        result.stdout.index("Prepare a Btrfs root"))
        self.assertNotIn("sample secret", result.stdout + result.stderr)

    def test_preserved_home_keeps_original_source_and_verifies_the_copy(self):
        result = run_installer("--demo", answer=demo_answers(encrypt="yes"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("source home partition is retained", result.stderr)
        self.assertLess(result.stdout.index("Keep the original home"),
                        result.stdout.index("Use one encrypted container"))
        self.assertLess(result.stdout.index("Create the home subvolume"),
                        result.stdout.index("Copy and verify preserved home files"))

    def test_user_default_can_be_supplied_externally(self):
        result = run_installer("--demo", answer=demo_answers(username=""),
                               extra_env={"CENERE_DEFAULT_USERNAME": "guest"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Local user: guest", result.stderr)
        self.assertIn("Configure user guest", result.stdout)


class TerminalWizardTests(unittest.TestCase):
    def setUp(self):
        self.master,self.slave=os.openpty()
        self.original_settings=termios.tcgetattr(self.slave)
        self.temporary=tempfile.TemporaryDirectory(prefix="cenere-catalog-")
        self.addCleanup(self.temporary.cleanup)
        base=Path(self.temporary.name)
        self.model_map=base/"kbd-model-map"
        self.model_map.write_text("us us pc105 - -\nit it pc105 - -\nit-us it pc105 us -\nde de pc105 - -\n")
        self.names=base/"evdev.lst"
        self.names.write_text("! layout\n  us English (US)\n  it Italian\n  de German\n! variant\n  us it: Italian (US keyboard)\n")
        self.addCleanup(self.close_terminal)
        self.start_program()

    def start_program(self,extra_env=None):
        env={**os.environ,"TERM":"xterm","LC_ALL":"C",
             "CENERE_KEYBOARD_MAP":str(self.model_map),"CENERE_XKB_NAMES":str(self.names),**(extra_env or {})}
        self.process=subprocess.Popen([str(PROGRAM),"--demo"],stdin=self.slave,stdout=self.slave,stderr=self.slave,env=env)
        self.transcript=b"";self.position=0
        self.wait_for(b"Choose the keyboard layout")

    def close_terminal(self):
        if self.process.poll() is None:self.process.kill()
        self.process.wait(timeout=5)
        os.close(self.master);os.close(self.slave)

    def wait_for(self,expected):
        deadline=time.monotonic()+5
        while expected not in self.transcript[self.position:]:
            remaining=deadline-time.monotonic()
            if remaining<=0:self.fail(f"Timed out waiting for {expected!r}: {self.transcript[-800:]!r}")
            if select.select([self.master],[],[],remaining)[0]:self.transcript+=os.read(self.master,65536)
        self.position=self.transcript.index(expected,self.position)+len(expected)

    def enter(self,expected):
        os.write(self.master,b"\r");self.wait_for(expected)

    def prepare_keyboard(self):
        self.enter(b"Enter: continue   Tab: layout")
        self.enter(b"Choose the destination disk")

    def reach_password(self):
        self.prepare_keyboard()
        for expected in (b"Keep the existing home files?",b"Encrypt the system and home?",
                         b"Choose the language and locale",b"Choose the timezone",b"Computer name",b"Local username",b"User password"):
            self.enter(expected)

    def finish(self,secret=b"terminal test secret"):
        os.write(self.master,secret+b"\r");self.wait_for(b"Confirm user password")
        os.write(self.master,secret+b"\r");self.wait_for(b"Review and confirm")
        self.wait_for(b"Type INSTALL")
        self.assertNotIn(b"[SIMULATED]",self.transcript)
        os.write(self.master,b"INSTALL\r");self.wait_for(b"Simulation completed")
        self.assertEqual(self.process.wait(timeout=5),0)
        self.assertEqual(termios.tcgetattr(self.slave),self.original_settings)
        self.assertNotIn(secret,self.transcript)

    def test_keyboard_is_first_and_full_phase_path_is_visible(self):
        self.assertNotIn(b"Choose the destination disk",self.transcript)
        self.assertIn(b"[Keyboard] > Disk > Preferences > Account > Summary > Install",self.transcript)
        self.reach_password()
        self.assertIn(b"1 of 4: Preferences",self.transcript)
        self.assertIn(b"2 of 4: Account",self.transcript)
        self.finish()
        self.assertNotIn(b"Review action",self.transcript)

    def test_password_is_hidden_and_terminal_settings_are_restored(self):
        self.reach_password()
        self.assertFalse(termios.tcgetattr(self.slave)[3]&(termios.ECHO|termios.ECHONL))
        self.finish()
        self.assertIn(b"\x1b[?1049h",self.transcript)
        self.assertIn(b"\x1b[?1049l",self.transcript)

    def test_interrupt_restores_terminal_echo(self):
        self.reach_password()
        self.process.send_signal(signal.SIGINT)
        self.assertEqual(self.process.wait(timeout=5),128+signal.SIGINT)
        self.assertEqual(termios.tcgetattr(self.slave),self.original_settings)

    def test_named_variants_back_navigation_and_disk_without_home(self):
        os.write(self.master,b"italian\r");self.wait_for(b"> Variant")
        self.wait_for(b"Italian (US keyboard)")
        os.write(self.master,b"\x1b");self.wait_for(b"Choose the keyboard layout")
        self.wait_for(b"\x1b[7m> Italian")
        self.enter(b"> Variant")
        self.enter(b"Enter: continue   Tab: layout")
        os.write(self.master,b"keyboard test\r");self.wait_for(b"Choose the destination disk")
        os.write(self.master,b"\x1b[B\r");self.wait_for(b"Encrypt the system and home?")
        self.assertNotIn(b"Keep the existing home files?",self.transcript)
        for expected in (b"Choose the language and locale",b"Choose the timezone",b"Computer name",b"Local username",b"User password"):
            self.enter(expected)
        self.finish(b"selected layout secret")
        self.assertIn(b"Demo disk B",self.transcript)
        self.assertNotIn(b"Variant (",self.transcript)

    def test_keyboard_trial_is_visible_from_the_start_and_optional_on_the_same_page(self):
        self.wait_for(b"Enter: select   Tab: next field")
        self.assertIn(b"Test the keyboard (optional)", self.transcript)
        self.assertIn(b"Confirm a layout to try it.", self.transcript)
        self.enter(b"Enter: continue   Tab: layout")
        screen = self.transcript.rsplit(b"\x1b[2J", 1)[-1]
        for label in (b"Choose the keyboard layout", b"Layout", b"Variant",
                      b"> Test the keyboard (optional)", b"Active: English (US) - Default (us)"):
            self.assertIn(label, screen)
        self.assertNotIn(b"Choose the destination disk", self.transcript)
        self.enter(b"Choose the destination disk")
        self.assertNotIn(b"Enter a valid value", self.transcript)

    def test_variant_filter_and_trial_text_do_not_change_the_layout_search(self):
        os.write(self.master, b"italian\r")
        self.wait_for(b"> Variant")
        os.write(self.master, b"us\r")
        self.wait_for(b"Active: Italian - Italian (US keyboard) (it-us)")
        self.wait_for(b"Enter: continue   Tab: layout")
        trial = "Symbols @# and accents èé".encode()
        os.write(self.master, trial)
        self.wait_for(trial)
        self.wait_for(b"Enter: continue   Tab: layout")
        screen = self.transcript.rsplit(b"\x1b[2J", 1)[-1]
        self.assertIn(b"italian", screen)
        self.assertIn(b"Active: Italian - Italian (US keyboard) (it-us)", screen)
        os.write(self.master, b"\x7f")
        self.wait_for(b"Enter: continue   Tab: layout")
        screen = self.transcript.rsplit(b"\x1b[2J", 1)[-1]
        self.assertIn("Symbols @# and accents è ".encode(), screen)
        os.write(self.master, b"\x15\r")
        self.wait_for(b"Choose the destination disk")
        for expected in (b"Keep the existing home files?", b"Encrypt the system and home?",
                         b"Choose the language and locale", b"Choose the timezone", b"Computer name",
                         b"Local username", b"User password"):
            self.enter(expected)
        self.finish()
        self.assertIn(b"console it-us", self.transcript)

    def test_changing_keyboard_clears_trial_and_preserves_selection_when_returning(self):
        self.enter(b"Enter: continue   Tab: layout")
        os.write(self.master, b"previous trial\t")
        self.wait_for(b"> Layout")
        os.write(self.master, b"italian\t")
        self.wait_for(b"> Variant")
        os.write(self.master, b"\x1b[B\t")
        self.wait_for(b"Active: Italian - Italian (US keyboard) (it-us)")
        self.wait_for(b"Enter: continue   Tab: layout")
        screen = self.transcript.rsplit(b"\x1b[2J", 1)[-1]
        self.assertNotIn(b"previous trial", screen)
        self.assertIn(b"Type here, or press Enter to continue.", screen)
        self.enter(b"Choose the destination disk")
        os.write(self.master, b"\x1b")
        self.wait_for(b"Enter: select   Tab: next field")
        screen = self.transcript.rsplit(b"\x1b[2J", 1)[-1]
        self.assertIn(b"\x1b[7m> Italian", screen)
        self.assertIn(b"> Italian (US keyboard)", screen)
        self.assertIn(b"Active: Italian - Italian (US keyboard) (it-us)", screen)
        self.enter(b"> Variant")
        self.enter(b"Enter: continue   Tab: layout")
        self.enter(b"Choose the destination disk")

    def test_keyboard_search_without_matches_can_be_edited_without_advancing(self):
        os.write(self.master, b"not-a-layout\r\t")
        self.wait_for(b"No matching choices.")
        os.write(self.master, b"\x15italian\r")
        self.wait_for(b"> Variant")
        os.write(self.master, b"not-a-variant\r")
        self.wait_for(b"No matching choices.")
        os.write(self.master, b"\x1b")
        self.wait_for(b"> Layout")
        self.enter(b"> Variant")
        os.write(self.master, b"\x15\x1b[H\r")
        self.wait_for(b"Active: Italian - Default (it)")
        self.wait_for(b"Enter: continue   Tab: layout")
        self.assertNotIn(b"Choose the destination disk", self.transcript)
        self.enter(b"Choose the destination disk")

    def test_keyboard_lists_scroll_on_a_small_terminal(self):
        os.write(self.master, b"\x03")
        self.wait_for(b"Installation cancelled")
        self.assertEqual(self.process.wait(timeout=5), 1)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 60, 0, 0))
        self.start_program()
        os.write(self.master, b"\x1b[F")
        self.wait_for(b"3 / 3")
        self.enter(b"> Variant")
        self.enter(b"Enter: continue   Tab: layout")
        self.assertIn(b"\x1b[13;", self.transcript)
        self.enter(b"Choose the destination disk")

    def test_keyboard_trial_can_be_skipped_after_reaching_its_length_limit(self):
        self.enter(b"Enter: continue   Tab: layout")
        os.write(self.master, b"x" * 160 + b"\r")
        self.wait_for(b"Choose the destination disk")

    def test_input_cursor_is_inside_the_field(self):
        self.reach_password();self.wait_for(b"\x1b[9;6H")
        self.assertIn(b"\x1b[9;5H\x1b[7m Type password...",self.transcript)

    def test_cancel_restores_terminal(self):
        os.write(self.master,b"\x03");self.wait_for(b"Installation cancelled")
        self.assertEqual(self.process.wait(timeout=5),1)
        self.assertEqual(termios.tcgetattr(self.slave),self.original_settings)

    def test_encryption_and_home_replacement_are_reviewed(self):
        self.prepare_keyboard();self.enter(b"Keep the existing home files?")
        os.write(self.master,b"\x1b[B\r");self.wait_for(b"Encrypt the system and home?")
        os.write(self.master,b"\x1b[B\r");self.wait_for(b"Choose the language and locale")
        for expected in (b"Choose the timezone",b"Computer name",b"Local username",b"User password"):
            self.enter(expected)
        self.finish()
        self.assertIn(b"/home will be replaced",self.transcript)
        self.assertIn(b"Use one encrypted container",self.transcript)

    def test_supplied_locale_and_timezone_catalogues_remain_available(self):
        base=Path(self.temporary.name)
        locales=base/"locale.gen";locales.write_text("#zz_ZZ.UTF-8 UTF-8\n")
        zones=base/"zone.tab";zones.write_text("ZZ\t+0000\tEtc/Demo\n")
        os.write(self.master,b"\x03");self.wait_for(b"Installation cancelled")
        self.assertEqual(self.process.wait(timeout=5),1)
        self.start_program({"CENERE_LOCALE_FILE":str(locales),"CENERE_TIMEZONE_FILE":str(zones)})
        self.prepare_keyboard()
        for expected in (b"Keep the existing home files?",b"Encrypt the system and home?",b"Choose the language and locale"):
            self.enter(expected)
        self.assertIn(b"zz_ZZ.UTF-8",self.transcript)
        self.enter(b"Choose the timezone")
        self.assertIn(b"Etc/Demo",self.transcript)
        for expected in (b"Computer name",b"Local username",b"User password"):
            self.enter(expected)
        self.finish()
        self.assertIn(b"Etc/Demo",self.transcript)

    def test_summary_can_be_edited_and_requires_exact_install(self):
        self.reach_password()
        os.write(self.master,b"private password\r");self.wait_for(b"Confirm user password")
        os.write(self.master,b"private password\r");self.wait_for(b"Review and confirm")
        os.write(self.master,b"install\r");self.wait_for(b"Type INSTALL exactly")
        self.assertNotIn(b"[SIMULATED]",self.transcript)
        os.write(self.master,b"\x1b");self.wait_for(b"Confirm user password")
        os.write(self.master,b"private password\r");self.wait_for(b"Review and confirm")
        os.write(self.master,b"INSTALL\r");self.wait_for(b"Simulation completed")
        self.assertEqual(self.process.wait(timeout=5),0)


class GuidedOrderTests(unittest.TestCase):
    """Exercise the real CLI questionnaire with a fake, non-writing disk backend."""
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix="cenere-flow-")
        cls.base=Path(cls.temporary.name)
        source=cls.base/"flow.c"
        source.write_text(r"""
#define _GNU_SOURCE
#define main cenere_main
#define geteuid test_geteuid
#include "cenere.c"
#undef main
#undef geteuid
uid_t test_geteuid(void) { return 0; }
const char *cenere_error(void) { return "Cannot unlock fixture storage"; }
size_t cenere_keyboards(struct cenere_keyboard *maps,size_t limit,bool demo) {
    (void)demo;
    const struct cenere_keyboard fixtures[] = {
        {.keymap="us",.layout="us",.name="English (US)",.variant_name="Default"},
        {.keymap="it",.layout="it",.name="Italian",.variant_name="Default"},
        {.keymap="it-us",.layout="it",.variant="us",.name="Italian",.variant_name="Italian (US keyboard)"}
    };
    size_t count=limit<3?limit:3;
    memcpy(maps,fixtures,count*sizeof(*maps));return count;
}
bool cenere_select_keyboard(struct installation_plan *p,const char *k,bool demo) {
    struct cenere_keyboard maps[3];
    size_t count=cenere_keyboards(maps,3,demo);
    for (size_t i=0;i<count;i++) if (!strcmp(k,maps[i].keymap)) {
        strcpy(p->keymap,maps[i].keymap);strcpy(p->keymap_group,maps[i].layout);
        strcpy(p->xkb_layout,maps[i].layout);strcpy(p->xkb_variant,maps[i].variant);return true;
    }
    return false;
}
bool cenere_apply_keyboard(struct installation_plan *p) {
    if (getenv("FIXTURE_KEYBOARD_FAILURE")) return false;
    const char *id=getenv("FIXTURE_IMAGE_ID");
    snprintf(p->image_id,sizeof(p->image_id),"%s",id?id:"anser");
    fprintf(stderr,"APPLIED_KEYBOARD %s\n",p->keymap);return true;
}
size_t cenere_disks(struct cenere_disk *d,size_t n) {(void)d;(void)n;return 0;}
bool cenere_inspect(struct installation_plan *p) {
    strcpy(p->snapshot,"fixture");p->locked_storage=!*p->existing_password;
    p->existing_home=true;p->existing_os=true;
    if (p->locked_storage) {fputs("DISCOVER_LOCKED\n",stderr);return true;}
    if (strcmp(p->existing_password,"old secret")) return false;
    p->existing_encryption=true;fputs("DISCOVER_UNLOCKED_HOME\n",stderr);return true;
}
bool cenere_preflight(struct installation_plan *p) {
    fputs("PREFLIGHT\n",stderr);
    return p->discard_encrypted ? !p->preserve_home && !p->alongside :
        !strcmp(p->existing_password,"old secret") && p->preserve_home;
}
void cenere_set_progress_callback(cenere_progress_callback callback) { (void)callback; }
int cenere_install(const struct installation_plan *p) {
    printf("STUB_INSTALL hostname=%s\n",p->hostname);
    return atoi(getenv("FIXTURE_INSTALL_STATUS")?getenv("FIXTURE_INSTALL_STATUS"):"0");
}
int cenere_power(bool reboot) {
    puts(reboot?"STUB_POWER reboot":"STUB_POWER poweroff");
    return atoi(getenv("FIXTURE_POWER_STATUS")?getenv("FIXTURE_POWER_STATUS"):"0");
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--finish-ui")) return finish_installation(true);
    if (argc==2 && !strcmp(argv[1],"--keyboard-ui")) {
        struct installation_plan plan;
        initialize_plan(&plan,REAL_INSTALLATION);
        strcpy(plan.password,"saved user secret");strcpy(plan.existing_password,"saved disk secret");
        char applied[64]="";
        const char *saved=getenv("FIXTURE_SAVED_KEYMAP");
        if (saved) {
            if (!cenere_select_keyboard(&plan,saved,false)) return 1;
            snprintf(applied,sizeof(applied),"%s",saved);
        }
        ui_active_plan=&plan;
        if (!ui_begin()) return 1;
        enum ui_result result=ui_keyboard_page(&plan,REAL_INSTALLATION,applied,sizeof(applied));
        ui_end();ui_active_plan=NULL;
        printf("KEYBOARD_RESULT keymap=%s user_password=%s disk_password=%s\n",plan.keymap,
               *plan.password?"saved":"cleared",*plan.existing_password?"saved":"cleared");
        return result==UI_NEXT?0:1;
    }
    return cenere_main(argc,argv);
}
""")
        cls.driver=cls.base/"flow"
        project=Path(__file__).resolve().parents[1]
        subprocess.run([*shlex.split(os.environ.get("CC","cc")),"-std=c11","-O2","-I",str(project),
                        str(source),"-o",str(cls.driver)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()

    def invoke(self,storage,confirmation="INSTALL",hostname="fixture",action=None,extra_env=None):
        values=["us","typing test",*storage,"yes","en_US.UTF-8","UTC",hostname,"person",
                "new secret","new secret",confirmation]
        if action is not None:values.append(action)
        return subprocess.run([str(self.driver),"--install","/dev/example"],input="\n".join(values)+"\n",
            text=True,capture_output=True,timeout=5,env={**os.environ,"TERM":"dumb",**(extra_env or {})})

    def keyboard_session(self, steps, extra_env=None, status=0):
        master, slave = os.openpty()
        original = termios.tcgetattr(slave)
        process = subprocess.Popen([str(self.driver), "--keyboard-ui"], stdin=slave, stdout=slave,
                                   stderr=slave, env={**os.environ, "TERM": "xterm", **(extra_env or {})})
        transcript = b""
        position = 0
        try:
            for expected, keys in steps:
                deadline = time.monotonic() + 5
                while expected not in transcript[position:]:
                    remaining = deadline - time.monotonic()
                    self.assertGreater(remaining, 0, transcript[-1000:])
                    if select.select([master], [], [], remaining)[0]:
                        transcript += os.read(master, 65536)
                position = transcript.index(expected, position) + len(expected)
                os.write(master, keys)
            self.assertEqual(process.wait(timeout=5), status)
            while select.select([master], [], [], 0)[0]:
                transcript += os.read(master, 65536)
            self.assertEqual(termios.tcgetattr(slave), original)
            for secret in (b"saved user secret", b"saved disk secret"):
                self.assertNotIn(secret, transcript)
            return transcript
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            os.close(master)
            os.close(slave)

    def test_keyboard_applies_before_optional_trial_and_does_not_reapply_unchanged_selection(self):
        transcript = self.keyboard_session([
            (b"Enter: select", b"\r"),
            (b"Enter: continue", b"\t"),
            (b"Enter: select", b"\r"),
            (b"Enter: continue", b"\r"),
        ])
        self.assertEqual(transcript.count(b"APPLIED_KEYBOARD"), 1)
        self.assertLess(transcript.index(b"APPLIED_KEYBOARD us"),
                        transcript.index(b"> Test the keyboard (optional)"))
        self.assertIn(b"keymap=us user_password=saved disk_password=saved", transcript)

    def test_keyboard_change_clears_saved_passwords_before_continuing(self):
        transcript = self.keyboard_session([
            (b"Enter: select", b"\r"),
            (b"Enter: continue", b"trial text\t"),
            (b"Enter: select", b"italian\r"),
            (b"> Variant", b"\x1b[B\r"),
            (b"Enter: continue", b"\r"),
        ], extra_env={"FIXTURE_SAVED_KEYMAP": "us"})
        self.assertEqual(transcript.count(b"APPLIED_KEYBOARD"), 1)
        self.assertIn(b"APPLIED_KEYBOARD it-us", transcript)
        self.assertIn(b"keymap=it-us user_password=cleared disk_password=cleared", transcript)

    def test_keyboard_apply_failure_blocks_trial_and_continuation(self):
        transcript = self.keyboard_session([(b"Enter: select", b"\r")],
                                           extra_env={"FIXTURE_KEYBOARD_FAILURE": "1"}, status=1)
        self.assertIn(b"Cannot unlock fixture storage", transcript)
        self.assertNotIn(b"> Test the keyboard (optional)", transcript)
        self.assertNotIn(b"Enter: continue", transcript)

    def test_default_hostname_uses_the_prepared_distribution(self):
        for image,expected in (("anser","anser"),("arch","arch"),("--my_distro--","my-distro"),("___","linux")):
            with self.subTest(image=image):
                result=self.invoke(["replace"],hostname="",extra_env={"FIXTURE_IMAGE_ID":image})
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertIn(f"STUB_INSTALL hostname={expected}",result.stdout)

    def test_custom_hostname_survives_returning_to_edit_the_summary(self):
        values=["us","typing test","replace","yes","en_US.UTF-8","UTC","my-computer","person",
                "new secret","new secret","edit",
                "us","typing test","replace","yes","en_US.UTF-8","UTC","","",
                "new secret","new secret","INSTALL"]
        result=subprocess.run([str(self.driver),"--install","/dev/example"],input="\n".join(values)+"\n",
            text=True,capture_output=True,timeout=5,env={**os.environ,"TERM":"dumb"})
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("STUB_INSTALL hostname=my-computer",result.stdout)

    def test_success_offers_direct_reboot_and_poweroff(self):
        for action in ("reboot","poweroff"):
            with self.subTest(action=action):
                result=self.invoke(["replace"],action=action)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(result.stdout.count("STUB_POWER"),1)
                self.assertIn(f"STUB_POWER {action}",result.stdout)
                self.assertEqual(result.stderr.count("Type INSTALL"),1)

    def test_failure_never_offers_or_runs_a_power_action(self):
        result=self.invoke(["replace"],action="reboot",extra_env={"FIXTURE_INSTALL_STATUS":"7"})
        self.assertEqual(result.returncode,7,result.stderr)
        self.assertNotIn("Next action",result.stderr)
        self.assertNotIn("STUB_POWER",result.stdout)

    def test_eof_empty_input_and_cancel_leave_the_live_environment_running(self):
        for action in (None,"","cancel"):
            with self.subTest(action=action):
                result=self.invoke(["replace"],action=action)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertNotIn("STUB_POWER",result.stdout)
                self.assertNotIn("Installation cancelled",result.stderr)

    def test_power_action_failure_preserves_its_status(self):
        result=self.invoke(["replace"],action="reboot",extra_env={"FIXTURE_POWER_STATUS":"23"})
        self.assertEqual(result.returncode,23,result.stderr)
        self.assertIn("Could not reboot (exit 23)",result.stderr)
        self.assertIn("installation is complete",result.stderr)

    def test_completion_menu_selects_actions_and_restores_the_terminal(self):
        for keys,expected in ((b"\r",b"STUB_POWER reboot"),(b"\x1b[B\r",b"STUB_POWER poweroff"),(b"\x1b",None)):
            with self.subTest(keys=keys):
                master,slave=os.openpty()
                original=termios.tcgetattr(slave)
                process=subprocess.Popen([str(self.driver),"--finish-ui"],stdin=slave,stdout=slave,stderr=slave,
                    env={**os.environ,"TERM":"xterm"})
                transcript=b""
                try:
                    deadline=time.monotonic()+5
                    while b"Enter: select" not in transcript:
                        remaining=deadline-time.monotonic()
                        self.assertGreater(remaining,0,transcript)
                        if select.select([master],[],[],remaining)[0]:transcript+=os.read(master,65536)
                    self.assertIn(b"Installation completed",transcript)
                    self.assertIn(b"4 of 4: Install",transcript)
                    self.assertNotIn(b"Type to filter",transcript)
                    os.write(master,keys)
                    self.assertEqual(process.wait(timeout=5),0)
                    while select.select([master],[],[],0)[0]:transcript+=os.read(master,65536)
                    if expected:self.assertIn(expected,transcript)
                    else:self.assertNotIn(b"STUB_POWER",transcript)
                    self.assertEqual(termios.tcgetattr(slave),original)
                finally:
                    if process.poll() is None:process.kill()
                    process.wait(timeout=5)
                    os.close(master);os.close(slave)

    def test_unlock_and_readonly_discovery_precede_preservation_and_account(self):
        result=self.invoke(["unlock","wrong secret","old secret","replace","yes"])
        self.assertEqual(result.returncode,0,result.stderr)
        order=["APPLIED_KEYBOARD","DISCOVER_LOCKED","Existing disk password","DISCOVER_UNLOCKED_HOME",
               "Preserve the existing home files","User password","Type INSTALL"]
        self.assertEqual([result.stderr.index(x) for x in order],sorted(result.stderr.index(x) for x in order))
        self.assertNotIn("Confirm existing",result.stderr)
        self.assertNotIn("Review action",result.stderr)
        for secret in ("wrong secret","old secret","new secret"):
            self.assertNotIn(secret,result.stdout+result.stderr)
        self.assertEqual(result.stdout.count("STUB_INSTALL"),1)

    def test_explicit_replacement_can_skip_unlock_and_preservation(self):
        result=self.invoke(["replace"])
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn("Existing disk password",result.stderr)
        self.assertNotIn("Preserve the existing home files",result.stderr)
        self.assertNotIn("Other systems on disk",result.stderr)
        self.assertIn("STUB_INSTALL",result.stdout)

    def test_failed_final_confirmation_cannot_start_installation(self):
        result=self.invoke(["unlock","old secret","replace","yes"],"install")
        self.assertEqual(result.returncode,1,result.stderr)
        self.assertNotIn("STUB_INSTALL",result.stdout)


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.assertEqual(os.geteuid(), 0, "integration tests require the sandbox")
        temporary = tempfile.TemporaryDirectory(prefix="cenere-test-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.source = self.base / "prepared root"
        # Exercise mountinfo's space, backslash, and tab escaping on every run.
        self.target = self.base / "target with spaces\\and\ttab"
        self.source.mkdir()
        self.mount_tmpfs(self.target)

        self.write(self.source, "etc/cenere/installable", "cenere-rootfs-v1\n")
        self.write(self.source, "etc/os-release", 'ID=anser\nNAME="Anser"\n')
        self.write(self.source, "etc/example.conf", "new configuration\n")
        self.write(self.source, "usr/bin/example", "example executable\n", 0o755)
        os.utime(self.source / "usr/bin/example", (1700000000, 1700000000))
        os.link(self.source / "etc/example.conf", self.source / "etc/hard-link")
        (self.source / "etc/symbolic-link").symlink_to("example.conf")
        os.setxattr(self.source / "etc/example.conf", "user.cenere", b"fixture")
        self.write(self.source, "home/person/document", "must not overwrite\n")
        self.write(self.source, "home/new-person/document", "must not be copied\n")

        self.write(self.target, "etc/example.conf", "old configuration\n")
        os.utime(self.target / "etc/example.conf", (1600000000, 1600000000))
        self.write(self.target, "obsolete", "must be deleted after confirmation\n")
        self.write(self.target, "home/person/document", "personal data\n", 0o600)
        self.write(self.target, "home/person/extra", "must not be deleted\n")
        (self.target / "home/person/link").symlink_to("document")
        self.original = snapshot(self.target)

    def write(self, root, name, contents, mode=0o644):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        path.chmod(mode)
        return path

    def mount_tmpfs(self, path):
        path.mkdir(parents=True, exist_ok=True)
        command("/usr/bin/mount", "-t", "tmpfs", "-o", "size=16m,nosuid,nodev", "tmpfs", path)
        self.addCleanup(command, "/usr/bin/umount", path)

    def assert_rejected(self, result, message):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertNotIn("Filesystem transfer completed", result.stdout)

    def test_copy_replaces_system_and_preserves_home(self):
        home_before = snapshot(self.target / "home")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Filesystem transfer completed", result.stdout)
        self.assertIn("not configured yet", result.stdout)
        self.assertEqual((self.target / "etc/example.conf").read_text(), "new configuration\n")
        self.assertFalse((self.target / "obsolete").exists())
        self.assertEqual(snapshot(self.target / "home"), home_before)

        installed = self.target / "usr/bin/example"
        self.assertEqual(stat.S_IMODE(installed.stat().st_mode), 0o755)
        self.assertEqual(installed.stat().st_mtime_ns, (self.source / "usr/bin/example").stat().st_mtime_ns)
        self.assertEqual(installed.stat().st_uid, (self.source / "usr/bin/example").stat().st_uid)
        self.assertEqual(installed.stat().st_gid, (self.source / "usr/bin/example").stat().st_gid)
        self.assertEqual((self.target / "etc/example.conf").stat().st_ino, (self.target / "etc/hard-link").stat().st_ino)
        self.assertEqual(os.readlink(self.target / "etc/symbolic-link"), "example.conf")
        self.assertEqual(os.getxattr(self.target / "etc/example.conf", "user.cenere"), b"fixture")

    def test_replaces_different_contents_with_equal_size_and_timestamp(self):
        source = self.source / "etc/example.conf"
        target = self.target / "etc/example.conf"
        os.utime(source, (1700000000, 1700000000))
        os.utime(target, (1700000000, 1700000000))
        self.assertEqual(source.stat().st_size, target.stat().st_size)
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_requires_exact_confirmation(self):
        for answer in ("", "\n", "yes\n", "install\n", "INSTALL \n", "INSTALL\x00extra\n"):
            with self.subTest(answer=answer):
                result = run_installer(self.source, self.target, answer=answer)
                message = "No confirmation received" if not answer else "Installation cancelled"
                self.assert_rejected(result, message)
                self.assertEqual(snapshot(self.target), self.original)

    def test_refuses_root_target(self):
        result = run_installer(self.source, "/", answer="INSTALL\n")
        self.assert_rejected(result, "running system's root")

    def test_refuses_overlapping_paths(self):
        inside_source = self.source / "destination"
        inside_target = self.target / "payload"
        inside_source.mkdir()
        inside_target.mkdir()
        for source, target in (
            (self.source, self.source),
            (self.source, inside_source),
            (inside_target, self.target),
        ):
            with self.subTest(source=source, target=target):
                result = run_installer(source, target, answer="INSTALL\n")
                self.assert_rejected(result, "must not overlap")

    def test_refuses_unmounted_target(self):
        target = self.base / "unmounted"
        target.mkdir()
        result = run_installer(self.source, target, answer="INSTALL\n")
        self.assert_rejected(result, "not an existing mountpoint")

    def test_refuses_read_only_target(self):
        command("/usr/bin/mount", "-o", "remount,bind,ro", self.target)
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "read-only")
        self.assertEqual(snapshot(self.target), self.original)

    def test_refuses_nested_mount_outside_home(self):
        self.mount_tmpfs(self.target / "var")
        before = snapshot(self.target)
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "nested target mounts outside /home")
        self.assertEqual(snapshot(self.target), before)

    def test_allows_mounts_at_and_below_home(self):
        self.mount_tmpfs(self.target / "home")
        self.write(self.target, "home/person/document", "separate home filesystem\n")
        self.mount_tmpfs(self.target / "home/person/documents")
        self.write(self.target, "home/person/documents/keep", "nested home filesystem\n")
        before = snapshot(self.target / "home")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(snapshot(self.target / "home"), before)

    def test_refuses_missing_payload_marker(self):
        (self.source / "etc/cenere/installable").unlink()
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "Payload marker is missing")
        self.assertEqual(snapshot(self.target), self.original)

    def test_refuses_invalid_payload_marker(self):
        (self.source / "etc/cenere/installable").write_text("unrecognized\n")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "unsupported format")
        self.assertEqual(snapshot(self.target), self.original)

    def test_copy_accepts_an_independent_arch_payload(self):
        self.write(self.source, "etc/os-release", 'ID=arch\nNAME="Arch Linux"\n')
        result=run_installer(self.source,self.target,answer="INSTALL\n")
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("Filesystem transfer completed",result.stdout)

    def test_refuses_missing_os_release(self):
        (self.source / "etc/os-release").unlink()
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "missing /etc/os-release")
        self.assertEqual(snapshot(self.target), self.original)

    def test_preserves_home_when_payload_home_is_a_symlink(self):
        shutil.rmtree(self.source / "home")
        (self.source / "home").symlink_to("etc")
        before = snapshot(self.target / "home")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.target / "home").is_symlink())
        self.assertEqual(snapshot(self.target / "home"), before)

    def test_preserves_home_when_payload_has_no_home(self):
        shutil.rmtree(self.source / "home")
        before = snapshot(self.target / "home")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(snapshot(self.target / "home"), before)

    def test_preserves_home_when_payload_home_is_a_file(self):
        shutil.rmtree(self.source / "home")
        self.write(self.source, "home", "must not replace home\n")
        before = snapshot(self.target / "home")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(snapshot(self.target / "home"), before)

    def test_preserves_target_home_symlink(self):
        personal = self.base / "personal files"
        shutil.move(self.target / "home", personal)
        (self.target / "home").symlink_to(personal)
        before = snapshot(personal)
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.target / "home").is_symlink())
        self.assertEqual(os.readlink(self.target / "home"), str(personal))
        self.assertEqual(snapshot(personal), before)

    def test_reports_rsync_failure(self):
        command("/usr/bin/mount", "--bind", "/usr/bin/false", "/usr/bin/rsync")
        self.addCleanup(command, "/usr/bin/umount", "/usr/bin/rsync")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "rsync failed; the target may be partially updated")

    def test_reports_rsync_execution_failure(self):
        replacement = self.write(self.base, "not-executable", "not an executable\n")
        command("/usr/bin/mount", "--bind", replacement, "/usr/bin/rsync")
        self.addCleanup(command, "/usr/bin/umount", "/usr/bin/rsync")
        result = run_installer(self.source, self.target, answer="INSTALL\n")
        self.assert_rejected(result, "Cannot execute /usr/bin/rsync")
        self.assertNotIn("target may be partially updated", result.stderr)
        self.assertEqual(snapshot(self.target), self.original)

    def test_guided_installation_rejects_directory_and_symlink_targets(self):
        alias = self.base / "target alias"
        alias.symlink_to(self.target)
        for destination in (self.target, alias, self.source, "/", "/nonexistent/disk"):
            with self.subTest(destination=destination):
                result = run_installer("--install", destination, answer=demo_answers())
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertNotIn("Type INSTALL", result.stderr)
                self.assertNotIn("Anser installation completed", result.stdout)
                self.assertNotIn("Filesystem transfer completed", result.stdout)
                self.assertNotIn("[SIMULATED]", result.stdout)
                self.assertNotIn("sample secret", result.stdout + result.stderr)
                self.assertEqual(snapshot(self.target), self.original)

    def test_guided_installation_eof_cannot_change_copy_target(self):
        result = run_installer(answer="")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("Type INSTALL", result.stderr)
        self.assertNotIn("Anser installation completed", result.stdout)
        self.assertNotIn("Filesystem transfer completed", result.stdout)
        self.assertNotIn("[SIMULATED]", result.stdout)
        self.assertEqual(snapshot(self.target), self.original)

    def test_demo_works_with_read_only_storage_and_no_rsync(self):
        # These mounts cover every normally writable location in this sandbox.
        # /usr/bin contains no formatter or boot utility; rsync is made unusable.
        replacement = self.write(self.base, "not-executable", "not an executable\n")
        command("/usr/bin/mount", "--bind", replacement, "/usr/bin/rsync")
        self.addCleanup(command, "/usr/bin/umount", "/usr/bin/rsync")
        command("/usr/bin/mount", "-o", "remount,bind,ro", self.target)
        command("/usr/bin/mount", "-o", "remount,bind,ro", "/tmp")
        command("/usr/bin/mount", "-o", "remount,bind,ro", "/run")
        try:
            result = run_installer("--demo", answer=demo_answers())
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Simulation completed", result.stdout)
            self.assertEqual(snapshot(self.target), self.original)
        finally:
            command("/usr/bin/mount", "-o", "remount,bind,rw", "/run")
            command("/usr/bin/mount", "-o", "remount,bind,rw", "/tmp")
            command("/usr/bin/mount", "-o", "remount,bind,rw", self.target)


def main():
    global PROGRAM
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("--sandbox", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    PROGRAM = args.program.resolve(strict=True)

    if args.sandbox:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(InstallationTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1

    required = {name: shutil.which(name) for name in ("bwrap", "mount", "umount", "false")}
    required["rsync"] = shutil.which(os.environ.get("RSYNC", "rsync"))
    missing = [name for name, path in required.items() if path is None]
    if missing:
        parser.error("missing test dependencies: " + ", ".join(missing) + "; see make help")

    suite = unittest.TestSuite([
        unittest.defaultTestLoader.loadTestsFromTestCase(CommandLineTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(DemoTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(TerminalWizardTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(GuidedOrderTests),
    ])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        return 1

    # /usr/bin is private so a local rsync can occupy the installer's fixed
    # /usr/bin/rsync path, and failure tests can replace it inside the sandbox.
    # Keep the existing read-only /proc: some containers forbid mounting procfs.
    sandbox = [
        required["bwrap"],
        "--unshare-user", "--unshare-net",
        "--uid", "0", "--gid", "0",
        "--cap-add", "CAP_SYS_ADMIN",
        "--die-with-parent",
        "--ro-bind", "/", "/",
        "--tmpfs", "/tmp",
        "--tmpfs", "/run",
        "--tmpfs", "/usr/bin",
        "--ro-bind", str(PROGRAM), "/run/cenere/cenere",
        "--ro-bind", str(Path(__file__).resolve()), "/run/cenere/test_cenere.py",
        "--ro-bind", str(Path(sys.executable).resolve()), "/usr/bin/python3",
    ]
    for name in ("mount", "umount", "false", "rsync"):
        sandbox.extend(["--ro-bind", required[name], "/usr/bin/" + name])
    sandbox.extend([
        "--setenv", "TMPDIR", "/tmp",
        "--chdir", "/tmp",
        "--", "/usr/bin/python3", "/run/cenere/test_cenere.py",
        "/run/cenere/cenere", "--sandbox",
    ])
    return subprocess.run(sandbox, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
