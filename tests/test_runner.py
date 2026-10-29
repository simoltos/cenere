#!/usr/bin/env python3
"""Exercise backend command running and file writes; no storage utilities execute.

The tiny driver includes cenere_install.c to expose its static command function.
Build with the same compiler/header/library settings used by Cenere. All driver,
child, captured output and log files live in a temporary directory under Cenere's test-results/.
"""

import hashlib
import os
from pathlib import Path
import re
import select
import shlex
import stat
import subprocess
import sys
import tempfile
import time
import unittest


PROJECT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#include "cenere_install.c"
#include <signal.h>

static void progress_sink(const char *phase, const char *detail, const char *path, uint64_t elapsed)
{
    (void)path;
    printf("FRAME %llu|%s|%s\n", (unsigned long long)elapsed, phase, detail);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 7 && argc != 8) return 2;
    signal(SIGPIPE, SIG_IGN);
    size_t size = 0, allocated = 4096;
    char *input = malloc(allocated);
    if (!input) return 3;
    for (;;) {
        if (size + 1 == allocated) {
            allocated *= 2;
            char *grown = realloc(input, allocated);
            if (!grown) return 3;
            input = grown;
        }
        ssize_t count = read(STDIN_FILENO, input + size, allocated - size - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return 3;
        if (!count) break;
        size += (size_t)count;
    }
    input[size] = 0;
    size_t capacity = strtoull(argv[4], NULL, 10);
    char *captured = capacity ? calloc(capacity, 1) : NULL;
    if (capacity && !captured) return 3;
    log_fd = open(argv[5], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log_fd < 0) return 3;
    snprintf(stage, sizeof(stage), "Runner regression");
    if (getenv("CENERE_RUNNER_GUI")) cenere_set_progress_callback(progress_sink);
    const char *arguments[] = {argv[1], argv[2], argv[3], argc == 8 ? "--info=progress2" : NULL, NULL};
    const char *scan[]={"/usr/bin/du","--all","--null","--block-size=1","--one-file-system","--",argv[2],NULL};
    int result = command(!strcmp(argv[3],"scan") ? scan : arguments, input, captured, capacity, false);
    if (!strcmp(argv[3], "sequence") && !result) {
        set_stage("Second command");
        result = command(arguments, input, captured, capacity, false);
    }
    int capture_fd = open(argv[6], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (capture_fd < 0 || (captured && !write_all(capture_fd, captured, strlen(captured)))) return 3;
    close(capture_fd);
    if (log_fd >= 0) close(log_fd);
    explicit_bzero(input, size);
    free(input);
    free(captured);
    return result < 0 ? 124 : result;
}
'''
CHILD = r'''
import hashlib, os, sys, time
mode = sys.argv[1]
if mode == "pipes":
    os.write(1, b"O" * (1024 * 1024))
    os.write(2, b"E" * (1024 * 1024))
    payload = sys.stdin.buffer.read()
    print("\nINPUT_SHA256=" + hashlib.sha256(payload).hexdigest(), flush=True)
elif mode == "chunks":
    for index in range(4096):
        os.write(1, (f"{index:04d}:" + "x" * 251 + "\n").encode())
    os.write(2, b"STDERR_COMPLETE\n")
elif mode == "heartbeat":
    sys.stdin.buffer.read()
    print("CHILD_STARTED", flush=True)
    time.sleep(12)
    print("CHILD_FINISHED", flush=True)
elif mode == "sequence":
    os.write(1, b"\x1b[2JRAW_SCREEN_ERASE\n")
    os.write(2, b"RAW_PACKAGE_DIAGNOSTIC\n")
    time.sleep(1.3)
elif mode in ("exit37", "exit0"):
    print("EARLY_EXIT", flush=True)
    sys.exit(37 if mode == "exit37" else 0)
elif mode in ("progress_cr", "progress_crlf"):
    os.write(2, b"STDERR_START\n" + b"E" * 65536 + b"\nSTDERR_END\n")
    delimiter = b"\r\n" if mode.endswith("crlf") else b"\r"
    for index in range(300):
        record = f"  {index + 1}.00K {index // 3}% 1.00MB/s 0:00:01 (xfr#{index})".encode() + delimiter
        os.write(1, record[:11])
        time.sleep(0.001)
        os.write(1, record[11:])
        if index == 150:
            os.write(2, b"MID_PROGRESS_ERROR\n")
        time.sleep(0.008)
    os.write(1, b"  999.00K 100% 1.00MB/s 0:00:01 (xfr#FINAL)")
    payload = sys.stdin.buffer.read()
    os.write(2, b"INPUT_SHA256=" + hashlib.sha256(payload).hexdigest().encode() + b"\n")
elif mode == "diagnostics":
    os.write(1, b"ordinary line\n" + b"D" * 2048 + b"\nordinary unterminated")
    os.write(2, b"  1.00K 50% 1.00MB/s 0:00:01\r\n" * 300 + b"actual error\n")
    sys.exit(37)
'''

CACHE_DRIVER = r'''
#define _GNU_SOURCE
#include <sys/statfs.h>
#include <sys/stat.h>
#include <linux/magic.h>
#include <stdint.h>
#include <string.h>
static unsigned long fake_flags = 1, fake_type = SQUASHFS_MAGIC;
static uint64_t fake_mount = 1, fake_inode = 1;
static unsigned fake_major = 8, fake_minor = 1, fake_mask = STATX_INO | STATX_MNT_ID;
static int fake_fs_error, fake_identity_error;
static int source_statfs(const char *path, struct statfs *result)
{
    (void)path;
    if (fake_fs_error) return -1;
    memset(result, 0, sizeof(*result));
    result->f_type = fake_type;
    result->f_flags = fake_flags;
    return 0;
}
static int source_statx(int directory, const char *path, int flags, unsigned mask, struct statx *result)
{
    (void)directory; (void)path; (void)flags; (void)mask;
    if (fake_identity_error) return -1;
    memset(result, 0, sizeof(*result));
    result->stx_mask = fake_mask;
    result->stx_mnt_id = fake_mount;
    result->stx_ino = fake_inode;
    result->stx_dev_major = fake_major;
    result->stx_dev_minor = fake_minor;
    return 0;
}
#define statfs(path, result) source_statfs(path, result)
#define statx(directory, path, flags, mask, result) source_statx(directory, path, flags, mask, result)
#include "cenere_install.c"
int main(void)
{
    char path[4096];
    unsigned long long mount, inode;
    while (scanf("%4095s %lu %lu %llu %llu %u %u %u %d %d", path, &fake_flags, &fake_type,
                 &mount, &inode, &fake_major, &fake_minor, &fake_mask, &fake_fs_error, &fake_identity_error) == 10) {
        fake_mount = mount; fake_inode = inode;
        printf("%llu\n", (unsigned long long)payload_size(path));
        fflush(stdout);
    }
    return 0;
}
'''

FILE_DRIVER = r'''
#include "cenere_install.c"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    umask(0077);
    return write_file(argv[1], strtoul(argv[2], NULL, 8), "%s", "replacement\n") ? 0 : 1;
}
'''


class RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = PROJECT / "test-results"
        directory.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="runner-", dir=directory)
        cls.base = Path(cls.temporary.name)
        cls.driver = cls.base / "driver"
        (cls.base / "driver.c").write_text(DRIVER)
        cls.child = cls.base / "child.py"
        cls.child.write_text(CHILD)
        # User-provided compiler options support the prepared Arch environment
        # and the host's json-c headers/library without installing extra tools.
        flags = shlex.split(os.environ.get("CPPFLAGS", ""))
        libraries = shlex.split(os.environ.get("LDLIBS", "-ljson-c"))
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                   *flags, "-I", str(PROJECT), str(cls.base / "driver.c"),
                   "-o", str(cls.driver), *libraries]
        subprocess.run(command, check=True, capture_output=True, text=True)
        cls.cache_driver = cls.base / "cache-driver"
        (cls.base / "cache-driver.c").write_text(CACHE_DRIVER)
        command[command.index(str(cls.base / "driver.c"))] = str(cls.base / "cache-driver.c")
        command[command.index(str(cls.driver))] = str(cls.cache_driver)
        subprocess.run(command, check=True, capture_output=True, text=True)
        cls.file_driver = cls.base / "file-driver"
        (cls.base / "file-driver.c").write_text(FILE_DRIVER)
        command[command.index(str(cls.base / "cache-driver.c"))] = str(cls.base / "file-driver.c")
        command[command.index(str(cls.cache_driver))] = str(cls.file_driver)
        subprocess.run(command, check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def arguments(self, mode, capacity, transfer=False):
        log = self.base / (self.id().split(".")[-1] + ".log")
        captured = log.with_suffix(".capture")
        arguments = [str(self.driver), sys.executable, str(self.child), mode, str(capacity),
                     str(log), str(captured)]
        if transfer:
            arguments.append("transfer")
        return arguments, log, captured

    def write_file(self, path, mode):
        return subprocess.run([str(self.file_driver), str(path), f"{mode:o}"],
                              capture_output=True, timeout=5)

    def test_written_modes_are_exact_under_private_umask(self):
        for mode in (0o444, 0o644, 0o600):
            for existing in (False, True):
                with self.subTest(mode=oct(mode), existing=existing):
                    path = self.base / f"permissions-{mode:o}-{existing}"
                    if existing:
                        path.write_text("old contents\n")
                        path.chmod(0o644 if mode == 0o600 else 0o600)
                    result = self.write_file(path, mode)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), mode)
                    self.assertEqual(path.read_bytes(), b"replacement\n")

    def test_file_write_refuses_symlink_without_changing_target(self):
        target = self.base / "symlink-target"
        target.write_bytes(b"untouched\n")
        target.chmod(0o600)
        link = self.base / "symlink-config"
        link.symlink_to(target)
        result = self.write_file(link, 0o644)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(link.is_symlink())
        self.assertEqual(target.read_bytes(), b"untouched\n")
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)

    def test_large_stdin_and_simultaneous_output_do_not_deadlock_or_log_secret(self):
        payload = (b"private-stdin-" + os.urandom(32).hex().encode()) * 32768
        args, log, captured = self.arguments("pipes", 2 * 1024 * 1024)
        result = subprocess.run(args, input=payload, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr[-1000:])
        expected = b"O" * (1024 * 1024) + b"\nINPUT_SHA256=" + hashlib.sha256(payload).hexdigest().encode() + b"\n"
        self.assertEqual(captured.read_bytes(), expected)
        logged = log.read_bytes()
        self.assertEqual(logged.count(b"E"), 1024 * 1024)
        self.assertEqual(logged.count(b"O"), 1024 * 1024)
        self.assertIn(expected[1024 * 1024:], logged)
        self.assertFalse(payload[:77] in logged, "Private stdin appeared in the log")
        self.assertFalse(payload[:77] in result.stdout + result.stderr, "Private stdin appeared in output")

    def test_split_large_capture_is_complete(self):
        args, log, captured = self.arguments("chunks", 2 * 1024 * 1024)
        result = subprocess.run(args, input=b"", capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr[-1000:])
        expected = b"".join((f"{index:04d}:" + "x" * 251 + "\n").encode() for index in range(4096))
        self.assertEqual(captured.read_bytes(), expected)
        self.assertIn(b"STDERR_COMPLETE\n", log.read_bytes())

    def test_capture_overflow_is_not_successful_truncation(self):
        args, log, captured = self.arguments("chunks", 128)
        result = subprocess.run(args, input=b"", capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 125, result.stderr[-1000:])
        self.assertLess(len(captured.read_bytes()), 128)
        self.assertIn(b"4095:" + b"x" * 251 + b"\n", log.read_bytes())

    def test_early_exit_with_unread_input_preserves_failure_and_refuses_false_success(self):
        payload = (b"unread-private-" + os.urandom(32).hex().encode()) * 32768
        for mode, expected in (("exit37", 37), ("exit0", 125)):
            with self.subTest(mode=mode):
                args, log, captured = self.arguments(mode, 4096)
                result = subprocess.run(args, input=payload, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stderr[-1000:])
                self.assertEqual(captured.read_bytes(), b"EARLY_EXIT\n")
                self.assertFalse(payload[:77] in log.read_bytes(), "Unread private stdin appeared in the log")

    def test_elapsed_activity_updates_one_terminal_line_before_completion(self):
        args,log,captured=self.arguments("heartbeat",4096)
        master,slave=os.openpty()
        self.addCleanup(os.close,master);self.addCleanup(os.close,slave)
        process=subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=slave)
        self.addCleanup(lambda:process.kill() if process.poll() is None else None)
        output=b"";deadline=time.monotonic()+3
        while b"elapsed" not in output and time.monotonic()<deadline:
            if select.select([master],[],[],0.2)[0]:output+=os.read(master,65536)
        self.assertIn(b"elapsed",output)
        self.assertIsNone(process.poll())
        process.communicate(timeout=15)
        self.assertEqual(process.returncode,0)
        while select.select([master],[],[],0)[0]:output+=os.read(master,65536)
        self.assertGreaterEqual(output.count(b"\r\x1b[K[Cenere]"),5)
        self.assertNotIn(b"still running",output)
        self.assertNotIn(b"\n",output)
        self.assertEqual(captured.read_bytes(),b"CHILD_STARTED\nCHILD_FINISHED\n")
        self.assertIn(b"Activity: Runner regression",log.read_bytes())

    def test_transfer_progress_is_sampled_and_final_record_survives_split_cr_and_crlf(self):
        payload = (b"private-transfer-" + os.urandom(32).hex().encode()) * 32768
        for mode in ("progress_cr", "progress_crlf"):
            with self.subTest(delimiter=mode):
                args, log, captured = self.arguments(mode, 0, transfer=True)
                result = subprocess.run(args, input=payload, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr[-1000:])
                frames = re.findall(rb"xfr#([^)]*)", result.stdout)
                self.assertGreaterEqual(len(frames), 3, "Progress was held until child exit")
                self.assertLessEqual(len(frames), 6, "Intermediate progress was not reduced to roughly one Hz")
                self.assertEqual(frames[-1], b"FINAL")
                expected_error = b"STDERR_START\n" + b"E" * 65536 + b"\nSTDERR_END\nMID_PROGRESS_ERROR\nINPUT_SHA256=" + hashlib.sha256(payload).hexdigest().encode() + b"\n"
                self.assertEqual(result.stderr, expected_error)
                logged = log.read_bytes()
                self.assertEqual(re.findall(rb"xfr#([^)]*)", logged), frames)
                self.assertEqual(logged.count(b"E"), expected_error.count(b"E"))
                self.assertFalse(payload[:77] in logged, "Private transfer input appeared in log")

    def test_transfer_keeps_ordinary_long_and_unterminated_stdout_and_all_stderr_errors(self):
        args, log, captured = self.arguments("diagnostics", 0, transfer=True)
        result = subprocess.run(args, input=b"", capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 37)
        self.assertEqual(result.stdout, b"ordinary line\n" + b"D" * 2048 + b"\nordinary unterminated")
        expected_error = b"  1.00K 50% 1.00MB/s 0:00:01\r\n" * 300 + b"actual error\n"
        self.assertEqual(result.stderr, expected_error)
        self.assertIn(expected_error, log.read_bytes())

    def test_transfer_flag_does_not_filter_captured_output(self):
        args, log, captured = self.arguments("chunks", 2 * 1024 * 1024, transfer=True)
        result = subprocess.run(args, input=b"", capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr[-1000:])
        expected = b"".join((f"{index:04d}:" + "x" * 251 + "\n").encode() for index in range(4096))
        self.assertEqual(captured.read_bytes(), expected)

    def test_screen_progress_clock_is_continuous_across_commands_and_raw_output_stays_in_log(self):
        args,log,captured=self.arguments("sequence",0)
        result=subprocess.run(args,input=b"",capture_output=True,timeout=10,
                              env={**os.environ,"CENERE_RUNNER_GUI":"1"})
        self.assertEqual(result.returncode,0,result.stderr)
        frames=re.findall(rb"FRAME (\d+)\|([^|\n]+)\|",result.stdout)
        seconds=[int(value) for value,_ in frames]
        self.assertEqual(seconds,sorted(seconds))
        self.assertGreaterEqual(seconds[-1],2)
        second=[int(value) for value,phase in frames if phase==b"Second command"]
        self.assertGreaterEqual(second[0],1)
        self.assertNotIn(b"RAW_SCREEN_ERASE",result.stdout)
        self.assertNotIn(b"\x1b[2J",result.stdout)
        self.assertEqual(result.stderr,b"")
        self.assertEqual(log.read_bytes().count(b"RAW_SCREEN_ERASE"),2)
        self.assertEqual(log.read_bytes().count(b"RAW_PACKAGE_DIAGNOSTIC"),2)

    def test_directory_measurement_reports_counts_and_preserves_du_total(self):
        root=self.base/'scan tree';root.mkdir()
        for index in range(100):
            (root/(f'private-name-{index}-'+('x'*100)+'\nwith\ttabs')).write_bytes(b'data'*1024)
        os.link(next(root.iterdir()),root/'hardlink')
        (root/'symlink').symlink_to('/does-not-exist')
        expected=int(subprocess.check_output(['/usr/bin/du','-sx','--block-size=1',str(root)]).split(b'\t')[0])
        args,log,captured=self.arguments('scan',64)
        args[2]=str(root)
        result=subprocess.run(args,input=b'',capture_output=True,timeout=10,
                              env={**os.environ,'CENERE_RUNNER_GUI':'1'})
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(int(captured.read_text()),expected)
        self.assertRegex(result.stdout,rb'Checked [1-9][0-9]* files and directories')
        self.assertNotIn(b'private-name',result.stdout+log.read_bytes())

    def test_screen_transfer_progress_has_no_ambiguous_rsync_clock_and_preserves_failure_diagnostics(self):
        args,log,captured=self.arguments("progress_crlf",0,transfer=True)
        result=subprocess.run(args,input=b"",capture_output=True,timeout=10,
                              env={**os.environ,"CENERE_RUNNER_GUI":"1"})
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(b"Transferred 999.00K | 100% | 1.00MB/s",result.stdout)
        self.assertNotIn(b"0:00:01",result.stdout)
        self.assertNotIn(b"MID_PROGRESS_ERROR",result.stdout)
        self.assertEqual(result.stderr,b"")
        self.assertIn(b"MID_PROGRESS_ERROR",log.read_bytes())
        args,log,captured=self.arguments("diagnostics",0,transfer=True)
        result=subprocess.run(args,input=b"",capture_output=True,timeout=10,
                              env={**os.environ,"CENERE_RUNNER_GUI":"1"})
        self.assertEqual(result.returncode,37)
        self.assertIn(b"actual error\n",log.read_bytes())
        self.assertNotIn(b"actual error",result.stdout)
        self.assertEqual(result.stderr,b"")

    def cache_process(self):
        process = subprocess.Popen([str(self.cache_driver)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        def cleanup():
            process.stdin.close()
            process.wait(timeout=5)
            process.stdout.close()
        self.addCleanup(cleanup)
        return process

    def cache_size(self, process, path, **changes):
        metadata = dict(flags=1, kind=0x73717368, mount=1, inode=1, major=8, minor=1,
                        mask=0x1100, fs_error=0, identity_error=0)
        metadata.update(changes)
        process.stdin.write(str(path) + " " + " ".join(str(value) for value in metadata.values()) + "\n")
        process.stdin.flush()
        self.assertTrue(select.select([process.stdout], [], [], 10)[0], "Source-size helper did not return")
        return int(process.stdout.readline())

    def source_tree(self, name):
        path = self.base / name
        path.mkdir()
        (path / "data").write_bytes(os.urandom(4096))
        return path

    def grow(self, path):
        with (path / "data").open("ab") as stream:
            stream.write(os.urandom(256 * 1024))

    def test_verified_readonly_squashfs_reuses_measurement(self):
        process = self.cache_process()
        path = self.source_tree("immutable")
        first = self.cache_size(process, path)
        self.grow(path)
        self.assertEqual(self.cache_size(process, path), first)

    def test_source_cache_requires_policy_and_complete_unchanged_identity(self):
        cases = {"rw": dict(flags=0), "other-fs": dict(kind=0xEF53),
                 "missing-mount-mask": dict(mask=0x100), "missing-inode-mask": dict(mask=0x1000),
                 "statfs-error": dict(fs_error=1), "statx-error": dict(identity_error=1),
                 "mount": dict(mount=2), "inode": dict(inode=2),
                 "major": dict(major=9), "minor": dict(minor=2)}
        for name, changes in cases.items():
            with self.subTest(change=name):
                process = self.cache_process()
                path = self.source_tree(name)
                first = self.cache_size(process, path)
                self.grow(path)
                second = self.cache_size(process, path, **changes)
                self.assertGreater(second, first)
                self.grow(path)
                self.assertGreater(self.cache_size(process, path), second)

    def test_changed_source_path_and_failed_measurement_are_not_reused(self):
        process = self.cache_process()
        first_path = self.source_tree("first-path")
        second_path = self.source_tree("second-path")
        first = self.cache_size(process, first_path)
        self.grow(second_path)
        self.assertGreater(self.cache_size(process, second_path), first)
        missing = self.base / "missing-source"
        self.assertEqual(self.cache_size(process, missing), (1 << 64) - 1)
        missing.mkdir()
        (missing / "data").write_bytes(os.urandom(4096))
        self.assertLess(self.cache_size(process, missing), (1 << 64) - 1)

    def test_mutable_home_or_source_measurements_keep_refreshing(self):
        process = self.cache_process()
        path = self.source_tree("mutable-home")
        first = self.cache_size(process, path, kind=0xEF53, flags=1)
        self.grow(path)
        second = self.cache_size(process, path, kind=0xEF53, flags=1)
        self.grow(path)
        self.assertGreater(second, first)
        self.assertGreater(self.cache_size(process, path, kind=0xEF53, flags=1), second)


if __name__ == "__main__":
    unittest.main(verbosity=2)
