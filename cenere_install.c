#define _GNU_SOURCE
#include "cenere_install.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/magic.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MiB (UINT64_C(1024) * 1024)
#define GiB (MiB * 1024)
#define MAX_PARTS 128
#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define BIOS_TYPE "21686148-6449-6E6F-744E-656564454649"
#define LINUX_TYPE "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define HOME_TYPE "933AC7E1-2EB4-4F13-B844-0E14E2AEF915"

/* Utilities implement filesystem formats and boot protocols. This module owns
 * the reviewed plan, sequencing and preservation checks, never shell commands. */
static char problem[512];
static char stage[128];
static char work[64];
static bool mounted_root, mounted_home, mounted_boot, mounted_efi;
static bool root_open, probe_open, top_mounted;
static int flush_fds[4] = {-1, -1, -1, -1};
static int log_fd = -1;
static char log_path[PATH_MAX];
static bool activity_visible;
static cenere_progress_callback progress_callback;
static struct timespec installation_started;
static char progress_detail[512];
static bool write_all(int fd, const char *text, size_t length);

void cenere_set_progress_callback(cenere_progress_callback callback)
{
    if (callback && !progress_callback) {
        clock_gettime(CLOCK_MONOTONIC, &installation_started);
        progress_detail[0] = '\0';
    }
    progress_callback = callback;
}

static void report_progress(const char *detail)
{
    if (!progress_callback)
        return;
    if (detail)
        snprintf(progress_detail, sizeof(progress_detail), "%s", detail);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t elapsed = now.tv_sec - installation_started.tv_sec;
    if (now.tv_nsec < installation_started.tv_nsec)
        elapsed--;
    progress_callback(stage, progress_detail, log_path,
                      installation_started.tv_sec && elapsed > 0 ? (uint64_t)elapsed : 0);
}

static void clear_activity(void)
{
    if (activity_visible) {
        write_all(STDERR_FILENO,"\r\033[K",4);
        activity_visible=false;
    }
}

static bool write_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t n = write(fd, text, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        text += n;
        length -= (size_t)n;
    }
    return true;
}

static void log_text(const char *text, size_t length)
{
    if (log_fd >= 0 && !write_all(log_fd, text, length)) {
        close(log_fd);
        log_fd = -1;
        fprintf(stderr, "Cenere could not continue writing its log: %s\n", strerror(errno));
    }
}

static void status_message(const char *format, ...)
{
    char message[768];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (progress_callback)
        report_progress(message);
    else {
        clear_activity();
        fprintf(stderr, "\n[Cenere] %s\n", message);
    }
    if (log_fd >= 0) {
        time_t now = time(NULL);
        struct tm utc;
        char timestamp[32], line[832];
        gmtime_r(&now, &utc);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S UTC", &utc);
        int n = snprintf(line, sizeof(line), "\n[%s] %s\n", timestamp, message);
        if (n > 0 && (size_t)n < sizeof(line))
            log_text(line, (size_t)n);
    }
}

static void set_stage(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(stage, sizeof(stage), format, arguments);
    va_end(arguments);
    progress_detail[0] = '\0';
    status_message("%s", stage);
}

static bool kernel_images(const char *root, bool restore);

struct partition {
    char device[256], type[80], uuid[80], fs[32], fs_uuid[80];
    uint64_t start, size, original_size;
    unsigned number;
    bool keep;
};
struct storage {
    struct partition parts[MAX_PARTS];
    size_t count;
    uint64_t bytes, sector, first, last, payload, home_used;
    int home, esp, bios, root, boot, shrink, source_home, old_boot;
    bool btrfs_home, encrypted_home, reuse_btrfs, arch_btrfs;
    char root_subvol[32], home_subvol[32];
    uint64_t root_id, home_id, btrfs_available;
    uint64_t shrink_size;
    bool gpt, inline_home, ambiguous_home;
    char serial[256], raw_fs[32], table[32768];
};

static bool error(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(problem, sizeof(problem), format, arguments);
    va_end(arguments);
    return false;
}
const char *cenere_error(void)
{
    return problem;
}

/* Rsync finishes a progress record for every small file. Keep draining its
 * pipe, but display/log at most one update per second and the final update.
 * Only progress records are condensed; diagnostics remain complete. */
struct transfer_progress {
    char line[512], latest[512];
    size_t used;
    struct timespec last;
    bool pending, long_line, after_progress, on_screen;
};

static void progress_show(struct transfer_progress *p)
{
    if (!p->pending)
        return;
    if (progress_callback) {
        char bytes[32], rate[32], detail[128];
        unsigned percent;
        if (sscanf(p->latest, "%31s %u%% %31s", bytes, &percent, rate) == 3) {
            snprintf(detail, sizeof(detail), "Transferred %s | %u%% | %s", bytes, percent, rate);
            report_progress(detail);
        }
    } else {
        clear_activity();
        write_all(STDOUT_FILENO, isatty(STDOUT_FILENO) ? "\r\033[K" : "\r", isatty(STDOUT_FILENO) ? 4 : 1);
        write_all(STDOUT_FILENO, p->latest, strlen(p->latest));
    }
    log_text(p->latest, strlen(p->latest));
    log_text("\n", 1);
    clock_gettime(CLOCK_MONOTONIC, &p->last);
    p->pending = false;
    p->on_screen = true;
}

static void progress_line(struct transfer_progress *p, char delimiter)
{
    p->line[p->used] = '\0';
    char bytes[32], rate[32], elapsed[32];
    unsigned percent;
    const char *start = p->line;
    while (isspace((unsigned char)*start))
        start++;
    if (!p->long_line && isdigit((unsigned char)*start) &&
        sscanf(start, "%31s %u%% %31s %31s", bytes, &percent, rate, elapsed) == 4 &&
        percent <= 100 && strstr(rate, "B/s") && strchr(elapsed, ':')) {
        snprintf(p->latest, sizeof(p->latest), "%s", p->line);
        p->pending = true;
        p->after_progress = true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!p->last.tv_sec || now.tv_sec - p->last.tv_sec > 1 ||
            (now.tv_sec - p->last.tv_sec == 1 && now.tv_nsec >= p->last.tv_nsec))
            progress_show(p);
    } else if (p->used || p->long_line || (delimiter == '\n' && !p->after_progress)) {
        progress_show(p);
        if (p->on_screen && !progress_callback) {
            write_all(STDOUT_FILENO, "\n", 1);
            p->on_screen = false;
        }
        if (!progress_callback)
            write_all(STDOUT_FILENO, p->line, p->used);
        log_text(p->line, p->used);
        if (delimiter) {
            if (!progress_callback)
                write_all(STDOUT_FILENO, &delimiter, 1);
            log_text(&delimiter, 1);
        }
        p->after_progress = false;
    } else if (delimiter == '\n')
        p->after_progress = false; /* Ignore the LF half of a progress CRLF. */
    p->used = 0;
    p->long_line = false;
}

static void progress_feed(struct transfer_progress *p, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\r' || c == '\n') {
            progress_line(p, c);
        } else {
            if (p->used == sizeof(p->line) - 1) {
                progress_show(p);
                if (p->on_screen && !progress_callback) {
                    write_all(STDOUT_FILENO, "\n", 1);
                    p->on_screen = false;
                }
                if (!progress_callback)
                    write_all(STDOUT_FILENO, p->line, p->used);
                log_text(p->line, p->used);
                p->used = 0;
                p->long_line = true;
            }
            p->line[p->used++] = c;
        }
    }
}

static void progress_finish(struct transfer_progress *p)
{
    /* Treat an unterminated final record the same as a complete one. */
    if (p->used)
        progress_line(p, '\0');
    progress_show(p);
    if (p->on_screen && !progress_callback)
        write_all(STDOUT_FILENO, "\n", 1);
}

static const char *string(json_object *object, const char *key)
{
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && value ? json_object_get_string(value)
                                                                   : "";
}
static uint64_t number(json_object *object, const char *key)
{
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && value ? json_object_get_uint64(value)
                                                                   : 0;
}

/* Return the actual exit code. Captured output is bounded and never truncated
 * into a successful parse. Secrets use stdin and are never printed. */
static int command(const char *const args[], const char *input, char *output, size_t capacity,
                   bool quiet)
{
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, status = 0;
    bool stream = (log_fd >= 0 || progress_callback) && !quiet;
    bool progress = false;
    struct transfer_progress transfer = {0};
    for (size_t i = 0; stream && !output && args[i]; i++)
        if (!strcmp(args[i], "--info=progress2"))
            progress = true;
    if ((input && pipe2(in, O_CLOEXEC)) || ((output || stream) && pipe2(out, O_CLOEXEC)) ||
        (stream && pipe2(err, O_CLOEXEC)))
        goto failed;
    pid_t child = fork();
    if (child < 0)
        goto failed;
    if (child == 0) {
        if (input)
            dup2(in[0], STDIN_FILENO);
        else {
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0)
                dup2(fd, STDIN_FILENO);
        }
        if (out[1] >= 0)
            dup2(out[1], STDOUT_FILENO);
        if (err[1] >= 0)
            dup2(err[1], STDERR_FILENO);
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                if (!output)
                    dup2(fd, STDOUT_FILENO);
            }
        }
        for (int i = 0; i < 2; i++) {
            if (in[i] >= 0)
                close(in[i]);
            if (out[i] >= 0)
                close(out[i]);
            if (err[i] >= 0)
                close(err[i]);
        }
        setenv("LC_ALL", "C", 1);
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    if (in[0] >= 0)
        close(in[0]);
    if (out[1] >= 0)
        close(out[1]);
    if (err[1] >= 0)
        close(err[1]);
    struct pollfd pipes[3] = {{in[1], POLLOUT, 0}, {out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    for (size_t i = 0; i < 3; i++)
        if (pipes[i].fd >= 0)
            fcntl(pipes[i].fd, F_SETFL, O_NONBLOCK);
    size_t length = input ? strlen(input) : 0, sent = 0, used = 0;
    bool overflow = false, finished = false, io_failed = false;
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    now = started;
    time_t last_notice = -1, last_log_notice=0;
    if (log_fd >= 0)
        dprintf(log_fd, "Running %s\n", args[0]);
    /* Drain stdout/stderr while feeding stdin. A utility may produce output
     * before reading a large partition script; sequential pipe I/O can deadlock. */
    while (!finished || pipes[0].fd >= 0 || pipes[1].fd >= 0 || pipes[2].fd >= 0) {
        if (pipes[0].fd >= 0 && sent == length) {
            close(pipes[0].fd);
            pipes[0].fd = -1;
        }
        if (pipes[0].fd < 0 && pipes[1].fd < 0 && pipes[2].fd < 0 && log_fd < 0 && !progress_callback) {
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            io_failed = waited < 0;
            finished = true;
            break;
        }
        int ready = poll(pipes, 3, 250);
        if (ready < 0 && errno != EINTR) {
            io_failed = true;
            break;
        }
        if (pipes[0].fd >= 0 && pipes[0].revents) {
            ssize_t n = write(pipes[0].fd, input + sent, length - sent);
            if (n > 0)
                sent += (size_t)n;
            else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                io_failed = true;
                close(pipes[0].fd);
                pipes[0].fd = -1;
            }
        }
        for (size_t i = 1; i < 3; i++) {
            if (pipes[i].fd < 0 || !pipes[i].revents)
                continue;
            char buffer[4096];
            ssize_t n = read(pipes[i].fd, buffer, sizeof(buffer));
            if (n > 0) {
                clear_activity();
                if (i == 1 && progress) {
                    progress_feed(&transfer, buffer, (size_t)n);
                    continue;
                }
                if (i == 1 && output) {
                    if (used + (size_t)n >= capacity)
                        overflow = true;
                    if (!overflow) {
                        memcpy(output + used, buffer, (size_t)n);
                        used += (size_t)n;
                    }
                } else {
                    if (i == 2 && progress && transfer.on_screen && !progress_callback) {
                        write_all(STDOUT_FILENO, "\n", 1);
                        transfer.on_screen = false;
                    }
                    if (!progress_callback)
                        write_all(i == 1 ? STDOUT_FILENO : STDERR_FILENO, buffer, (size_t)n);
                }
                if (stream)
                    log_text(buffer, (size_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                if (n < 0)
                    io_failed = true;
                close(pipes[i].fd);
                pipes[i].fd = -1;
            }
        }
        if (!finished) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
                finished = true;
            else if (waited < 0 && errno != EINTR) {
                io_failed = true;
                finished = true;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        time_t elapsed = now.tv_sec - started.tv_sec;
        if ((log_fd >= 0 || progress_callback) && !finished && elapsed != last_notice) {
            if (progress_callback)
                report_progress(NULL);
            else if (isatty(STDERR_FILENO) && !(progress && transfer.on_screen)) {
                fprintf(stderr,"\r\033[K[Cenere] %s | %ld:%02ld elapsed",stage,
                        (long)elapsed/60,(long)elapsed%60);
                fflush(stderr);
                activity_visible=true;
            }
            last_notice=elapsed;
            if (log_fd >= 0 && elapsed-last_log_notice>=10) {
                dprintf(log_fd,"Activity: %s (%ld seconds elapsed)\n",stage,(long)elapsed);
                last_log_notice=elapsed;
            }
        }
    }
    for (size_t i = 0; i < 3; i++)
        if (pipes[i].fd >= 0)
            close(pipes[i].fd);
    if (!finished)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    clear_activity();
    report_progress(NULL);
    if (output)
        output[used] = '\0';
    if (progress)
        progress_finish(&transfer);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    if (code == 0 && (overflow || io_failed))
        code = 125;
    if (log_fd >= 0)
        dprintf(log_fd, "Finished %s: exit %d (%ld seconds)\n", args[0], code,
                (long)(now.tv_sec - started.tv_sec));
    return code;
failed:
    for (size_t i = 0; i < 2; i++) {
        if (in[i] >= 0)
            close(in[i]);
        if (out[i] >= 0)
            close(out[i]);
        if (err[i] >= 0)
            close(err[i]);
    }
    return 127;
}
#define RUN(...) command((const char *const[]){__VA_ARGS__, NULL}, NULL, NULL, 0, false)
#define READ(buf, ...)                                                                             \
    command((const char *const[]){__VA_ARGS__, NULL}, NULL, buf, sizeof(buf), true)

int cenere_power(bool reboot)
{
    return RUN("/usr/bin/systemctl", reboot ? "reboot" : "poweroff");
}

static bool run(const char *const args[], const char *input)
{
    int code = command(args, input, NULL, 0, false);
    return code == 0 || error("%s failed (exit %d) during %s", args[0], code, stage);
}
#define DO(...) run((const char *const[]){__VA_ARGS__, NULL}, NULL)

static bool directory(const char *path)
{
    struct stat st;
    if (lstat(path, &st) == 0)
        return S_ISDIR(st.st_mode) && st.st_uid == 0
                   ? true
                   : error("Unsafe installation directory: %s", path);
    return errno == ENOENT && mkdir(path, 0700) == 0
               ? true
               : error("Cannot create %s: %s", path, strerror(errno));
}

static bool begin_log(void)
{
    clock_gettime(CLOCK_MONOTONIC, &installation_started);
    progress_detail[0] = '\0';
    if (!directory("/var/log/cenere"))
        return false;
    if (chmod("/var/log/cenere", 0700))
        return error("Cannot protect the installation log directory");
    strcpy(log_path, "/var/log/cenere/install-XXXXXX.log");
    log_fd = mkstemps(log_path, 4);
    if (log_fd < 0)
        return error("Cannot create installation log: %s", strerror(errno));
    fcntl(log_fd, F_SETFD, FD_CLOEXEC);
    if (unlink("/var/log/cenere/latest.log") && errno != ENOENT)
        return error("Cannot update installation log link");
    if (symlink(strrchr(log_path, '/') + 1, "/var/log/cenere/latest.log"))
        return error("Cannot update installation log link");
    status_message("Installation log: %s", log_path);
    return true;
}

static bool save_log(const char *target)
{
    if (log_fd < 0)
        return true;
    char path[PATH_MAX];
    const char *folders[] = {"/var", "/var/log", "/var/log/cenere", NULL};
    for (size_t i = 0; folders[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", target, folders[i]);
        if (!directory(path))
            return false;
    }
    snprintf(path, sizeof(path), "%s/var/log/cenere/installation.log", target);
    int input = open(log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (input < 0 || output < 0) {
        if (input >= 0)
            close(input);
        if (output >= 0)
            close(output);
        return error("Cannot save installation log in the installed system");
    }
    char buffer[8192];
    ssize_t n;
    bool ok = true;
    while ((n = read(input, buffer, sizeof(buffer))) != 0) {
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || !write_all(output, buffer, (size_t)n)) {
            ok = false;
            break;
        }
    }
    if (fsync(output))
        ok = false;
    close(input);
    close(output);
    return ok || error("Cannot flush the saved installation log");
}
static bool workspace(void)
{
    if (*work)
        return true;
    strcpy(work, "/run/cenere-XXXXXX");
    if (!mkdtemp(work))
        return error("Cannot create installer workspace");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/probe", work);
    return directory(path);
}
static void trim(char *text)
{
    text[strcspn(text, "\r\n")] = '\0';
}

static json_object *lsblk(const char *device)
{
    char data[65536];
    const char *args[] = {"/usr/bin/lsblk",
                          "--json",
                          "--list",
                          "--bytes",
                          "--paths",
                          "--output",
                          "NAME,TYPE,SIZE,LOG-SEC,RO,SERIAL,MAJ:MIN,PKNAME,MOUNTPOINTS,FSTYPE,UUID",
                          device,
                          NULL};
    if (!device)
        args[7] = NULL;
    if (command(args, NULL, data, sizeof(data), true) != 0) {
        error("Cannot inspect block devices");
        return NULL;
    }
    json_object *object = json_tokener_parse(data), *blocks = NULL;
    if (!object || !json_object_object_get_ex(object, "blockdevices", &blocks) ||
        !json_object_is_type(blocks, json_type_array)) {
        if (object)
            json_object_put(object);
        error("Invalid block-device inventory");
        return NULL;
    }
    return object;
}
static bool safe_disk(const char *device, json_object *inventory)
{
    struct stat st;
    if (strncmp(device, "/dev/", 5) != 0 || stat(device, &st) != 0 || !S_ISBLK(st.st_mode))
        return error("Destination must be a whole block disk: %s", device);
    json_object *blocks = NULL;
    json_object_object_get_ex(inventory, "blockdevices", &blocks);
    bool found = false;
    for (size_t i = 0; i < json_object_array_length(blocks); i++) {
        json_object *b = json_object_array_get_idx(blocks, i), *mounts = NULL;
        if (strcmp(string(b, "name"), device) == 0) {
            found = strcmp(string(b, "type"), "disk") == 0 && number(b, "ro") == 0;
            if (!found)
                return error("Destination is not a writable whole disk");
        }
        if (json_object_object_get_ex(b, "mountpoints", &mounts) && mounts) {
            for (size_t j = 0; j < json_object_array_length(mounts); j++) {
                json_object *m = json_object_array_get_idx(mounts, j);
                if (m && *json_object_get_string(m))
                    return error("Disk is in use (%s mounted at %s)", string(b, "name"),
                                 json_object_get_string(m));
            }
        }
        const char *type = string(b, "type");
        if (strcmp(type, "disk") && strcmp(type, "part"))
            return error("Disk has active mapped/stacked devices: %s", string(b, "name"));
        if (!strcmp(string(b, "fstype"), "swap")) {
            FILE *swaps = fopen("/proc/swaps", "r");
            char line[PATH_MAX + 256];
            if (!swaps)
                return error("Cannot inspect active swap");
            bool active = false;
            while (fgets(line, sizeof(line), swaps)) {
                char name[PATH_MAX];
                if (sscanf(line, "%4095s", name) == 1 && !strcmp(name, string(b, "name")))
                    active = true;
            }
            fclose(swaps);
            if (active)
                return error("Disk contains active swap");
        }
        unsigned maj = 0, min = 0;
        if (sscanf(string(b, "maj:min"), "%u:%u", &maj, &min) != 2)
            return error("Missing device identity");
        char holders[256];
        snprintf(holders, sizeof(holders), "/sys/dev/block/%u:%u/holders", maj, min);
        DIR *dir = opendir(holders);
        if (!dir)
            return error("Cannot inspect device holders");
        struct dirent *entry;
        bool busy = false;
        while ((entry = readdir(dir)))
            if (entry->d_name[0] != '.')
                busy = true;
        closedir(dir);
        if (busy)
            return error("Disk has active holders");
    }
    return found || error("Disk identity is absent from inventory");
}

size_t cenere_disks(struct cenere_disk *disks, size_t limit)
{
    json_object *all = lsblk(NULL), *blocks = NULL;
    if (!all)
        return 0;
    json_object_object_get_ex(all, "blockdevices", &blocks);
    size_t count = 0;
    for (size_t i = 0; i < json_object_array_length(blocks) && count < limit; i++) {
        json_object *b = json_object_array_get_idx(blocks, i);
        if (strcmp(string(b, "type"), "disk") || number(b, "ro"))
            continue;
        const char *device = string(b, "name");
        json_object *one = lsblk(device);
        bool safe = one && safe_disk(device, one);
        if (one)
            json_object_put(one);
        if (!safe)
            continue;
        snprintf(disks[count].device, sizeof(disks[count].device), "%s", device);
        snprintf(disks[count].label, sizeof(disks[count].label), "%s — %.1f GiB%s%s", device,
                 (double)number(b, "size") / GiB, *string(b, "serial") ? " — " : "",
                 string(b, "serial"));
        count++;
    }
    json_object_put(all);
    return count;
}

static bool read_storage(const char *device, struct storage *s, char *snapshot, size_t capacity)
{
    memset(s, 0, sizeof(*s));
    s->home = s->esp = s->bios = s->root = s->boot = s->shrink = s->source_home = s->old_boot = -1;
    json_object *inventory = lsblk(device), *blocks = NULL;
    if (!inventory)
        return false;
    if (!safe_disk(device, inventory)) {
        json_object_put(inventory);
        return false;
    }
    json_object_object_get_ex(inventory, "blockdevices", &blocks);
    for (size_t i = 0; i < json_object_array_length(blocks); i++) {
        json_object *b = json_object_array_get_idx(blocks, i);
        if (!strcmp(string(b, "name"), device)) {
            s->bytes = number(b, "size");
            s->sector = number(b, "log-sec");
            snprintf(s->serial, sizeof(s->serial), "%s", string(b, "serial"));
            snprintf(s->raw_fs, sizeof(s->raw_fs), "%s", string(b, "fstype"));
        }
    }
    if (s->sector < 512 || s->sector > 4096 || s->bytes < 20 * GiB) {
        json_object_put(inventory);
        return error("Disk must be at least 20 GiB with supported logical sectors");
    }
    int code = READ(s->table, "/usr/bin/sfdisk", "--json", device);
    json_object *table = code == 0 ? json_tokener_parse(s->table) : NULL, *pt = NULL, *parts = NULL;
    if (table && json_object_object_get_ex(table, "partitiontable", &pt)) {
        s->gpt = !strcmp(string(pt, "label"), "gpt");
        if (number(pt, "sectorsize") && number(pt, "sectorsize") != s->sector) {
            json_object_put(table);
            json_object_put(inventory);
            return error("Partition sector size mismatch");
        }
        s->first = number(pt, "firstlba");
        s->last = number(pt, "lastlba");
        if (json_object_object_get_ex(pt, "partitions", &parts)) {
            if (json_object_array_length(parts) > MAX_PARTS) {
                json_object_put(table);
                json_object_put(inventory);
                return error("Too many partitions");
            }
            for (size_t i = 0; i < json_object_array_length(parts); i++) {
                json_object *p = json_object_array_get_idx(parts, i);
                struct partition *part = &s->parts[s->count++];
                snprintf(part->device, sizeof(part->device), "%s", string(p, "node"));
                snprintf(part->type, sizeof(part->type), "%s", string(p, "type"));
                snprintf(part->uuid, sizeof(part->uuid), "%s", string(p, "uuid"));
                part->start = number(p, "start");
                part->size = number(p, "size");
                part->original_size = part->size;
                const char *suffix = part->device + strlen(device);
                if (*suffix == 'p')
                    suffix++;
                part->number = (unsigned)strtoul(suffix, NULL, 10);
                if (part->number < 1 || part->number > MAX_PARTS || !part->size ||
                    part->start + part->size < part->start ||
                    part->start + part->size > s->bytes / s->sector) {
                    json_object_put(table);
                    json_object_put(inventory);
                    return error("Invalid partition geometry");
                }
                for (size_t j = 0; j < json_object_array_length(blocks); j++) {
                    json_object *b = json_object_array_get_idx(blocks, j);
                    if (!strcmp(string(b, "name"), part->device)) {
                        snprintf(part->fs, sizeof(part->fs), "%s", string(b, "fstype"));
                        snprintf(part->fs_uuid, sizeof(part->fs_uuid), "%s", string(b, "uuid"));
                    }
                }
                if (!strcasecmp(part->type, ESP_TYPE) && !strcmp(part->fs, "vfat"))
                    s->esp = (int)i;
                if (!strcasecmp(part->type, BIOS_TYPE))
                    s->bios = (int)i;
            }
        }
    } else {
        /* A failed table read is safe only if blkid confirms no on-disk label. */
        char label[128];
        int detected = READ(label, "/usr/bin/blkid", "-p", "-s", "PTTYPE", "-o", "value", device);
        if (detected == 0 || (detected != 2 && detected != 0)) {
            if (table)
                json_object_put(table);
            json_object_put(inventory);
            return error(
                "Cannot read existing partition table; refusing to classify disk as empty");
        }
        s->table[0] = '\0';
    }
    if (!s->first)
        s->first = MiB / s->sector;
    if (!s->last)
        s->last = s->bytes / s->sector - 34;
    struct stat identity;
    if (stat(device, &identity) != 0) {
        if (table)
            json_object_put(table);
        json_object_put(inventory);
        return error("Cannot bind destination device identity");
    }
    int n =
        snprintf(snapshot, capacity, "%llu:%llu:%s:%llu:%llu:%s\n%s", (unsigned long long)s->bytes,
                 (unsigned long long)s->sector, s->serial, (unsigned long long)identity.st_rdev,
                 (unsigned long long)identity.st_ino, s->raw_fs, s->table);
    if (table)
        json_object_put(table);
    json_object_put(inventory);
    return (n >= 0 && (size_t)n < capacity) || error("Disk snapshot is too large");
}

/* A distribution supplies one small descriptor and the resolved package sets.
 * Cenere deliberately does not infer desktop, services or privilege policy. */
static bool image_source(const char *root, struct installation_plan *plan)
{
    char path[PATH_MAX];
    struct stat st;
    struct statfs filesystem;
    if (statfs(root,&filesystem) || filesystem.f_type!=SQUASHFS_MAGIC || !(filesystem.f_flags&ST_RDONLY))
        return error("The prepared source must remain a read-only SquashFS");
    snprintf(path, sizeof(path), "%s/usr/share/cenere/image.json", root);
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 16384)
        return error("The prepared image descriptor is missing or invalid");
    json_object *image = json_object_from_file(path);
    if (!image)
        return error("Cannot read the prepared image descriptor");
    const char *id = string(image, "id"), *user = string(image, "live_user");
    bool ok = number(image, "version") == 1 && *id && strlen(id) < sizeof(plan->image_id) &&
              strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-_") == strlen(id) &&
              strlen(user) < sizeof(plan->live_user) && strcmp(user,"root") &&
              strspn(user, "abcdefghijklmnopqrstuvwxyz0123456789-_") == strlen(user);
    if (ok) {
        snprintf(plan->image_id, sizeof(plan->image_id), "%s", id);
        snprintf(plan->live_user, sizeof(plan->live_user), "%s", user);
        snprintf(path, sizeof(path), "%s/usr/share/cenere/packages.final", root);
        ok = access(path, R_OK) == 0;
    }
    json_object_put(image);
    return ok || error("Unsupported or incomplete prepared image descriptor");
}
static bool find_source(struct installation_plan *plan)
{
    FILE *mounts = fopen("/proc/self/mounts", "r");
    char device[PATH_MAX], path[PATH_MAX], type[128], options[512];
    if (!mounts)
        return error("Cannot inspect the live image mounts");
    bool found = false;
    while (fscanf(mounts, "%4095s %4095s %127s %511s %*d %*d\n", device, path, type, options) == 4) {
        if (!strcmp(type, "squashfs") && (strstr(options, "ro,") || !strcmp(options, "ro")) &&
            image_source(path, plan)) {
            snprintf(plan->source, sizeof(plan->source), "%s", path);
            found = true;
            break;
        }
    }
    fclose(mounts);
    problem[0] = '\0';
    return found || error("No prepared read-only system image was found; boot a Cenere-enabled ISO");
}

static bool keyboard_atom(const char *text)
{
    return strlen(text) < 64 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-,:") == strlen(text);
}

/* Use systemd's console/XKB correspondence rather than grouping arbitrary kbd
 * filenames. Xkeyboard-config supplies the human-readable layout/variant names. */
static void keyboard_names(struct cenere_keyboard *map, bool demo)
{
    static const char *const names[][2] = {
        {"us","English (US)"}, {"gb","English (UK)"}, {"it","Italian"},
        {"de","German"}, {"fr","French"}, {"es","Spanish"}, {"ch","Swiss"},
        {"be","Belgian"}, {"br","Portuguese (Brazil)"}, {"pt","Portuguese"},
        {"ru","Russian"}, {"ua","Ukrainian"}, {"pl","Polish"}, {"nl","Dutch"},
        {"no","Norwegian"}, {"se","Swedish"}, {"fi","Finnish"}, {"dk","Danish"},
        {"cz","Czech"}, {"sk","Slovak"}, {"hu","Hungarian"}, {"ro","Romanian"},
        {"gr","Greek"}, {"tr","Turkish"}, {"jp","Japanese"}, {"il","Hebrew"}
    };
    char layout[64], variant[64];
    snprintf(layout, sizeof(layout), "%s", map->layout);
    layout[strcspn(layout, ",")] = 0;
    snprintf(variant, sizeof(variant), "%s", map->variant);
    variant[strcspn(variant, ",")] = 0;
    snprintf(map->name, sizeof(map->name), "%s", layout);
    for (size_t i=0; i<sizeof(names)/sizeof(names[0]); i++)
        if (!strcmp(layout,names[i][0])) snprintf(map->name,sizeof(map->name),"%s",names[i][1]);
    snprintf(map->variant_name, sizeof(map->variant_name), "%s", *variant ? variant : "Default");
    const char *override = demo ? getenv("CENERE_XKB_NAMES") : NULL;
    FILE *file = fopen(override ? override : "/usr/share/X11/xkb/rules/evdev.lst", "r");
    if (!file && !override) file=fopen("/usr/share/xkeyboard-config-2/rules/evdev.lst","r");
    if (!file) return;
    char line[512], section[32]="";
    while (fgets(line,sizeof(line),file)) {
        if (line[0]=='!') { sscanf(line,"! %31s",section); continue; }
        char code[64], owner[64]; int used=0;
        if (!strcmp(section,"layout") && sscanf(line," %63s %n",code,&used)==1 && !strcmp(code,layout)) {
            trim(line+used); snprintf(map->name,sizeof(map->name),"%s",line+used);
        } else if (*variant && !strcmp(section,"variant") &&
                   sscanf(line," %63s %63[^:]: %n",code,owner,&used)==2 &&
                   !strcmp(code,variant) && !strcmp(owner,layout)) {
            trim(line+used); snprintf(map->variant_name,sizeof(map->variant_name),"%s",line+used);
        }
    }
    fclose(file);
}

size_t cenere_keyboards(struct cenere_keyboard *maps, size_t limit, bool demo)
{
    const char *override = demo ? getenv("CENERE_KEYBOARD_MAP") : NULL;
    FILE *file = fopen(override ? override : "/usr/share/systemd/kbd-model-map", "r");
    size_t count=0;
    if (file) {
        char line[1024];
        while (count<limit && fgets(line,sizeof(line),file)) {
            struct cenere_keyboard map={0};
            if (line[0]=='#' || sscanf(line,"%63s %63s %63s %63s %127s",
                map.keymap,map.layout,map.model,map.variant,map.options)!=5) continue;
            if (!strcmp(map.model,"-")) *map.model=0;
            if (!strcmp(map.variant,"-")) *map.variant=0;
            if (!strcmp(map.options,"-")) *map.options=0;
            if (!keyboard_atom(map.keymap) || !keyboard_atom(map.layout) ||
                !keyboard_atom(map.model) || !keyboard_atom(map.variant) ||
                strspn(map.options,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-,:")!=strlen(map.options))
                continue;
            bool duplicate=false;
            for (size_t i=0;i<count;i++)
                if (!strcmp(map.keymap,maps[i].keymap) ||
                    (!strcmp(map.layout,maps[i].layout) && !strcmp(map.model,maps[i].model) &&
                     !strcmp(map.variant,maps[i].variant) && !strcmp(map.options,maps[i].options))) {
                    /* Prefer the canonical short name (it rather than it2). */
                    if (!strcmp(map.layout,maps[i].layout) && !strcmp(map.variant,maps[i].variant) &&
                        !strcmp(map.keymap,map.layout)) snprintf(maps[i].keymap,sizeof(maps[i].keymap),"%s",map.keymap);
                    duplicate=true; break;
                }
            if (!duplicate) { keyboard_names(&map,demo); maps[count++]=map; }
        }
        fclose(file);
    }
    if (!count && demo && !override) {
        const char *const codes[][3]={{"us","us",""},{"uk","gb",""},{"it","it",""},
            {"de","de",""},{"de-latin1-nodeadkeys","de","nodeadkeys"},{"fr","fr",""},{"es","es",""}};
        for (size_t i=0;i<sizeof(codes)/sizeof(codes[0]) && count<limit;i++) {
            struct cenere_keyboard map={0};
            snprintf(map.keymap,sizeof(map.keymap),"%s",codes[i][0]);
            snprintf(map.layout,sizeof(map.layout),"%s",codes[i][1]);
            snprintf(map.model,sizeof(map.model),"pc105");
            snprintf(map.variant,sizeof(map.variant),"%s",codes[i][2]);
            keyboard_names(&map,true); maps[count++]=map;
        }
    }
    return count;
}

bool cenere_select_keyboard(struct installation_plan *plan, const char *keymap, bool demo)
{
    struct cenere_keyboard maps[256];
    size_t count=cenere_keyboards(maps,256,demo);
    for (size_t i=0;i<count;i++) if (!strcmp(keymap,maps[i].keymap)) {
        snprintf(plan->keymap,sizeof(plan->keymap),"%s",maps[i].keymap);
        snprintf(plan->keymap_group,sizeof(plan->keymap_group),"%.*s",(int)strcspn(maps[i].layout,","),maps[i].layout);
        snprintf(plan->xkb_layout,sizeof(plan->xkb_layout),"%s",maps[i].layout);
        snprintf(plan->xkb_model,sizeof(plan->xkb_model),"%s",maps[i].model);
        snprintf(plan->xkb_variant,sizeof(plan->xkb_variant),"%s",maps[i].variant);
        snprintf(plan->xkb_options,sizeof(plan->xkb_options),"%s",maps[i].options);
        return true;
    }
    return error("This console layout has no supported console/graphical correspondence");
}

bool cenere_apply_keyboard(struct installation_plan *plan)
{
    if (!cenere_select_keyboard(plan,plan->keymap,false) || !find_source(plan)) return false;
    if (RUN("/usr/bin/localectl","--no-convert","set-keymap",plan->keymap) ||
        RUN("/usr/bin/localectl","--no-convert","set-x11-keymap",plan->xkb_layout,
            plan->xkb_model,plan->xkb_variant,plan->xkb_options))
        return error("Cannot apply the selected keyboard to the live system");
    char hook[PATH_MAX+64];
    snprintf(hook,sizeof(hook),"%s/usr/lib/cenere/integration",plan->source);
    if (access(hook,F_OK)==0 && RUN(hook,"keyboard",plan->live_user,plan->xkb_layout,
                                   plan->xkb_model,plan->xkb_variant,plan->xkb_options))
        return error("The image could not apply the selected keyboard to its live desktop");
    return true;
}

static uint64_t tree_size(const char *path)
{
    char output[256];
    if (READ(output, "/usr/bin/du", "-sx", "--block-size=1", path) != 0)
        return UINT64_MAX;
    char *end;
    errno = 0;
    uint64_t n = strtoull(output, &end, 10);
    return !errno && end != output ? n : UINT64_MAX;
}

static uint64_t payload_size(const char *path)
{
    static char cached_path[PATH_MAX];
    static struct statx cached;
    static uint64_t size = UINT64_MAX;
    struct statfs fs;
    struct statx identity;
    bool immutable = statfs(path, &fs) == 0 && fs.f_type == SQUASHFS_MAGIC &&
                     (fs.f_flags & ST_RDONLY) &&
                     statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_MNT_ID,
                           &identity) == 0 &&
                     (identity.stx_mask & (STATX_INO | STATX_MNT_ID)) ==
                         (STATX_INO | STATX_MNT_ID);
    if (immutable && size != UINT64_MAX && !strcmp(path, cached_path) &&
        identity.stx_mnt_id == cached.stx_mnt_id && identity.stx_ino == cached.stx_ino &&
        identity.stx_dev_major == cached.stx_dev_major &&
        identity.stx_dev_minor == cached.stx_dev_minor)
        return size;
    uint64_t measured = tree_size(path);
    if (immutable && measured != UINT64_MAX) {
        snprintf(cached_path, sizeof(cached_path), "%s", path);
        cached = identity;
        size = measured;
    } else {
        cached_path[0] = '\0';
        size = UINT64_MAX;
    }
    return measured;
}
static bool resolve_partition(const struct storage *s, const char *source, int *index)
{
    for (size_t i = 0; i < s->count; i++) {
        const struct partition *p = &s->parts[i];
        if (!strcmp(source, p->device) ||
            (!strncmp(source, "UUID=", 5) && !strcmp(source + 5, p->fs_uuid)) ||
            (!strncmp(source, "PARTUUID=", 9) && !strcasecmp(source + 9, p->uuid))) {
            *index = (int)i;
            return true;
        }
    }
    return false;
}
/* Inspection never replays an existing filesystem journal/tree log. Encrypted
 * probes use a read-only mapping owned by this process and keep secrets in RAM. */
static bool close_probe(const char *probe, bool mounted)
{
    bool ok = !mounted || RUN("/usr/bin/umount", probe) == 0;
    if (probe_open && ok) {
        ok = RUN("/usr/bin/cryptsetup", "close", "cenere-probe") == 0;
        if (ok)
            probe_open = false;
    }
    return ok || error("Cannot close the read-only storage inspection");
}

static bool mount_probe(const struct partition *p, const struct installation_plan *plan,
                        const char *probe, const char *type, const char *options)
{
    const char *device = p->device;
    if (!strcmp(p->fs, "crypto_LUKS")) {
        if (probe_open || !*plan->existing_password)
            return error("The existing disk password is required for preservation");
        if (!run((const char *const[]){"/usr/bin/cryptsetup", "open", "--readonly",
                    "--key-file", "-", device, "cenere-probe", NULL}, plan->existing_password))
            return error("Cannot unlock the existing storage for read-only inspection");
        probe_open = true;
        device = "/dev/mapper/cenere-probe";
    }
    if (!strcmp(type, "btrfs")) {
        char super[16384];
        if (READ(super, "/usr/bin/btrfs", "inspect-internal", "dump-super", device)) {
            close_probe(probe, false);
            return error("Cannot inspect the existing Btrfs superblock");
        }
        bool log_found=false;
        for (char *line=strtok(super,"\n"); line; line=strtok(NULL,"\n")) {
            unsigned long long log_root;
            if (sscanf(line,"log_root %llu",&log_root)==1) {
                log_found=true;
                if (log_root) {
                    close_probe(probe,false);
                    return error("Existing Btrfs filesystem needs journal recovery; boot it and shut down cleanly first");
                }
            }
        }
        if (!log_found) { close_probe(probe,false); return error("Cannot verify Btrfs journal state"); }
    }
    if (RUN("/usr/bin/mount", "-t", type, "-o", options, device, probe)) {
        close_probe(probe, false);
        return error("Cannot inspect the existing filesystem read-only");
    }
    return true;
}

static bool subvolume_id(const char *path, uint64_t *id)
{
    struct stat st;
    char output[128], *end;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_ino != 256 ||
        READ(output, "/usr/bin/btrfs", "inspect-internal", "rootid", path))
        return false;
    trim(output);
    errno = 0;
    *id = strtoull(output, &end, 10);
    return !errno && *id > 5 && end != output && !*end;
}

static bool discover_btrfs(struct storage *s, struct installation_plan *plan,
                           size_t index, const char *probe)
{
    const struct partition *p = &s->parts[index];
    if (!mount_probe(p, plan, probe, "btrfs", "ro,rescue=nologreplay,subvolid=5"))
        return false;
    bool ok = true, recognized = false;
    int fd=open(probe,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct btrfs_ioctl_fs_info_args info={0};
    if (fd<0 || ioctl(fd,BTRFS_IOC_FS_INFO,&info) || info.num_devices!=1) {
        if (fd>=0) close(fd);
        close_probe(probe,true);
        return error("Preservation requires a single-device Btrfs filesystem");
    }
    close(fd);
    const char *roots[] = {"@", "root", NULL};
    for (size_t i = 0; roots[i]; i++) {
        char path[PATH_MAX], line[1024];
        uint64_t root_id, home_id;
        snprintf(path, sizeof(path), "%s/%s", probe, roots[i]);
        if (!subvolume_id(path, &root_id))
            continue;
        snprintf(path, sizeof(path), "%s/%s/etc/fstab", probe, roots[i]);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        char home_name[32] = "";
        int old_boot = -1;
        bool matching_root = false;
        while (fgets(line, sizeof(line), file)) {
            char device[256], point[128], type[32], options[512];
            if (line[0] == '#' || sscanf(line, "%255s %127s %31s %511s",
                                       device, point, type, options) != 4)
                continue;
            if (!strcmp(point, "/boot") && !strcmp(type, "ext4")) {
                if (!resolve_partition(s, device, &old_boot) ||
                    strcmp(s->parts[old_boot].fs, "ext4"))
                    old_boot = -1;
            }
            if (strcmp(type, "btrfs") || (strcmp(point, "/home") && strcmp(point, "/")))
                continue;
            char uuid_value[128];
            const char *block = probe_open ? "/dev/mapper/cenere-probe" : p->device;
            if (READ(uuid_value, "/usr/bin/blkid", "-s", "UUID", "-o", "value", block))
                continue;
            trim(uuid_value);
            if (strncmp(device, "UUID=", 5) || strcmp(device + 5, uuid_value))
                continue;
            for (char *option = strtok(options, ","); option; option = strtok(NULL, ",")) {
                if (strncmp(option, "subvol=", 7))
                    continue;
                const char *name = option + 7;
                if (*name == '/')
                    name++;
                if (!strcmp(point, "/") && !strcmp(name, roots[i]))
                    matching_root = true;
                if (!strcmp(point, "/home") && (!strcmp(name, "@home") || !strcmp(name, "home")))
                    snprintf(home_name, sizeof(home_name), "%s", name);
            }
        }
        fclose(file);
        if (!matching_root || !*home_name || old_boot < 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", probe, home_name);
        if (!subvolume_id(path, &home_id))
            continue;
        recognized = true;
        if (s->home >= 0) {
            s->ambiguous_home = true;
            continue;
        }
        s->home = (int)index;
        s->btrfs_home = true;
        s->encrypted_home = !strcmp(p->fs, "crypto_LUKS");
        s->old_boot = old_boot;
        s->root_id = root_id;
        s->home_id = home_id;
        snprintf(s->root_subvol, sizeof(s->root_subvol), "%s", roots[i]);
        snprintf(s->home_subvol, sizeof(s->home_subvol), "%s", home_name);
        char size_text[128], *end;
        if (READ(size_text,"/usr/bin/du","-s","--block-size=1",path)) {
            ok=error("Cannot measure the Btrfs home"); break;
        }
        errno=0; s->home_used=strtoull(size_text,&end,10);
        if (errno || end==size_text) { ok=error("Invalid Btrfs home size"); break; }
        snprintf(path,sizeof(path),"%s/%s/usr/bin/pacman",probe,roots[i]);
        s->arch_btrfs=access(path,X_OK)==0;
        snprintf(path,sizeof(path),"%s/%s/usr/bin/mkinitcpio",probe,roots[i]);
        s->arch_btrfs=s->arch_btrfs && access(path,X_OK)==0;
        if (s->arch_btrfs) {
            snprintf(path,sizeof(path),"%s/%s",probe,roots[i]);
            if (!kernel_images(path,false)) {ok=false;break;}
        }
        snprintf(path,sizeof(path),"%s/@cenere-previous",probe);
        struct stat previous;
        if (!lstat(path,&previous)) {
            ok=error("A previous interrupted Btrfs installation requires recovery before reinstalling"); break;
        }
        struct statvfs fs;
        if (s->home_used == UINT64_MAX || statvfs(probe, &fs)) {
            ok = error("Cannot measure the existing Btrfs filesystem");
            break;
        }
        s->btrfs_available = (uint64_t)fs.f_bavail * fs.f_frsize;
        size_t used = strlen(plan->systems);
        snprintf(plan->systems + used, sizeof(plan->systems) - used,
                 "%sBtrfs system (%s, subvolumes %s and %s)", used ? "; " : "",
                 p->device, roots[i], home_name);
    }
    /* Unknown Btrfs layouts may contain home data too. Offer preservation and
     * refuse it safely instead of omitting that choice on an unsupported layout. */
    if (ok && !recognized)
        s->ambiguous_home = true;
    return close_probe(probe, true) && ok;
}

static bool mount_preserved_home(const struct storage *s, const struct installation_plan *plan,
                                 const char *probe)
{
    const struct partition *p = &s->parts[s->source_home];
    char options[128];
    snprintf(options, sizeof(options), "ro,rescue=nologreplay,subvol=%s", s->home_subvol);
    return mount_probe(p, plan, probe, s->btrfs_home ? "btrfs" : "ext4",
                       s->btrfs_home ? options : "ro,noload");
}

static bool discover(struct storage *s, struct installation_plan *plan)
{
    if (!workspace())
        return false;
    char probe[128];
    snprintf(probe, sizeof(probe), "%s/probe", work);
    plan->systems[0] = '\0';
    plan->existing_home = false;
    plan->locked_storage = false;
    for (size_t i = 0; i < s->count; i++) {
        struct partition *p = &s->parts[i];
        if (!strcmp(p->fs, "crypto_LUKS") && !*plan->existing_password) {
            plan->locked_storage = true;
            continue;
        }
        if (!strcmp(p->fs, "btrfs") || !strcmp(p->fs, "crypto_LUKS")) {
            if (!discover_btrfs(s, plan, i, probe))
                return false;
            continue;
        }
        if (strcmp(p->fs, "ext4"))
            continue;
        if (RUN("/usr/bin/mount", "-t", "ext4", "-o", "ro,noload", p->device, probe) != 0)
            continue; /* The partition still appears as data in the review. */
        char file[PATH_MAX], line[512], name[256] = "";
        snprintf(file, sizeof(file), "%s/etc/os-release", probe);
        FILE *f = fopen(file, "r");
        if (f) {
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "PRETTY_NAME=", 12) || (!*name && !strncmp(line, "ID=", 3))) {
                    char *value = strchr(line, '=') + 1;
                    trim(value);
                    if (*value == '"' || *value == '\'') {
                        value++;
                        value[strcspn(value, "\"'")] = '\0';
                    }
                    snprintf(name, sizeof(name), "%s", value);
                }
            fclose(f);
        }
        if (*name) {
            size_t used = strlen(plan->systems);
            snprintf(plan->systems + used, sizeof(plan->systems) - used, "%s%s (%s)",
                     used ? "; " : "", name, p->device);
            snprintf(file, sizeof(file), "%s/etc/fstab", probe);
            f = fopen(file, "r");
            int home = -1;
            if (f) {
                while (fgets(line, sizeof(line), f)) {
                    char source[256], mountpoint[256], fs[32];
                    if (line[0] == '#')
                        continue;
                    if (sscanf(line, "%255s %255s %31s", source, mountpoint, fs) == 3 &&
                        !strcmp(mountpoint, "/home")) {
                        if (!strcmp(fs, "ext4") && resolve_partition(s, source, &home) &&
                            !strcmp(s->parts[home].fs, "ext4")) {
                            if (s->home >= 0 && s->home != home)
                                s->ambiguous_home = true;
                            s->home = home;
                        } else
                            s->ambiguous_home = true;
                    }
                }
                fclose(f);
            }
            if (home < 0 && !s->ambiguous_home) {
                snprintf(file, sizeof(file), "%s/home", probe);
                DIR *dir = opendir(file);
                if (dir) {
                    struct dirent *entry;
                    while ((entry = readdir(dir)))
                        if (entry->d_name[0] != '.')
                            s->inline_home = true;
                    closedir(dir);
                }
            }
        }
        if (RUN("/usr/bin/umount", probe) != 0)
            return error("Cannot unmount discovery filesystem");
    }
    if (s->home >= 0 && !s->btrfs_home) {
        if (RUN("/usr/bin/mount", "-t", "ext4", "-o", "ro,noload", s->parts[s->home].device,
                probe) != 0)
            return error("Cannot inspect the existing home read-only");
        s->home_used = tree_size(probe);
        int result = RUN("/usr/bin/umount", probe);
        if (result || s->home_used == UINT64_MAX)
            return error("Cannot measure the existing home");
        plan->existing_home = true;
    } else if (s->btrfs_home || s->inline_home || s->ambiguous_home || plan->locked_storage)
        plan->existing_home = true;
    s->source_home = s->home;
    plan->existing_encryption = s->encrypted_home;
    plan->existing_os = s->count > 0 || *s->raw_fs;
    if (plan->existing_os && !*plan->systems)
        snprintf(plan->systems, sizeof(plan->systems),
                 "Existing partitions/data (including any unreadable systems)");
    return true;
}

bool cenere_inspect(struct installation_plan *plan)
{
    struct storage s;
    problem[0] = '\0';
    char canonical[PATH_MAX];
    if (!realpath(plan->target, canonical))
        return error("Cannot resolve destination disk");
    snprintf(plan->target, sizeof(plan->target), "%s", canonical);
    if (!read_storage(plan->target, &s, plan->snapshot, sizeof(plan->snapshot)) ||
        !discover(&s, plan) || !find_source(plan))
        return false;
    plan->disk_bytes = s.bytes;
    plan->existing_encryption=s.encrypted_home;
    return true;
}

static uint64_t align_up(uint64_t value, uint64_t unit)
{
    return ((value + unit - 1) / unit) * unit;
}
static void free_extent(const struct storage *s, uint64_t *start, uint64_t *size)
{
    uint64_t cursor = align_up(s->first, MiB / s->sector);
    *start = *size = 0;
    /* Consume every preserved interval that extends beyond the cursor,
     * including partitions beginning before an alignment boundary. */
    for (size_t pass = 0; pass <= s->count; pass++) {
        uint64_t next = s->last + 1, end = s->last + 1;
        for (size_t i = 0; i < s->count; i++) {
            const struct partition *p = &s->parts[i];
            if (p->keep && p->start + p->size > cursor && p->start < next) {
                next = p->start;
                end = p->start + p->size;
            }
        }
        uint64_t gap = next > cursor ? next - cursor : 0;
        gap = (gap / (MiB / s->sector)) * (MiB / s->sector);
        if (gap > *size) {
            *start = cursor;
            *size = gap;
        }
        if (next == s->last + 1)
            break;
        cursor = align_up(end, MiB / s->sector);
    }
}
static int append_partition(struct storage *s, const char *disk, uint64_t start, uint64_t size,
                            const char *type)
{
    if (s->count >= MAX_PARTS)
        return -1;
    unsigned n = 1;
    for (;; n++) {
        bool used = false;
        for (size_t i = 0; i < s->count; i++)
            if (s->parts[i].keep && s->parts[i].number == n)
                used = true;
        if (!used)
            break;
    }
    if (n > MAX_PARTS)
        return -1;
    struct partition *p = &s->parts[s->count];
    *p = (struct partition){.start = start, .size = size, .number = n, .keep = true};
    snprintf(p->type, sizeof(p->type), "%s", type);
    snprintf(p->device, sizeof(p->device), "%s%s%u", disk,
             isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", n);
    return (int)s->count++;
}

static bool plan_storage(struct storage *s, struct installation_plan *plan)
{
    if ((plan->locked_storage && !plan->discard_encrypted) ||
        (plan->discard_encrypted && (plan->preserve_home || plan->alongside)))
        return error("Locked contents must be inspected or explicitly selected for replacement");
    s->payload = payload_size(plan->source);
    if (s->payload == UINT64_MAX)
        return error("Cannot measure the prepared payload");
    if (plan->alongside && !s->gpt)
        return error("Keeping existing systems requires a GPT disk");
    if (plan->preserve_home && !s->gpt)
        return error("Home preservation on an MBR disk requires a separate backup before "
                     "converting to GPT; no changes made");
    if (plan->preserve_home && (s->inline_home || s->ambiguous_home || s->home < 0))
        return error("Preservation requires an identified ext4 home or Btrfs root/home subvolumes; "
                     "this layout cannot be changed safely");
    int old_home = s->source_home;
    s->reuse_btrfs = plan->preserve_home && s->btrfs_home && s->arch_btrfs && !plan->alongside &&
                     plan->encrypt == s->encrypted_home;
    plan->reuse_storage = s->reuse_btrfs;
    if (s->reuse_btrfs) {
        if (s->old_boot < 0 || s->esp < 0 || (strcmp(plan->firmware,"uefi") && s->bios < 0) ||
            s->btrfs_available < s->payload + 2 * GiB)
            return error("Btrfs preservation requires existing boot partitions and free space "
                         "for a new root while retaining the previous root");
        for (size_t i = 0; i < s->count; i++)
            s->parts[i].keep = true;
        s->root = s->home = old_home;
        s->boot = s->old_boot;
        snprintf(plan->layout, sizeof(plan->layout),
                 "\n  KEEP all partitions and existing encryption settings\n"
                 "  REPLACE root subvolume %s on %s (ID %llu)\n"
                 "  KEEP home subvolume %s (ID %llu); root/home share free space\n"
                 "  KEEP boot filesystem %s; update system boot files\n",
                 s->root_subvol, s->parts[s->root].device, (unsigned long long)s->root_id,
                 s->home_subvol, (unsigned long long)s->home_id, s->parts[s->boot].device);
        return true;
    }
    if (!plan->preserve_home || old_home < 0)
        s->home = -1;
    for (size_t i = 0; i < s->count; i++)
        s->parts[i].keep =
            plan->alongside || (int)i == s->home || (int)i == s->esp || (int)i == s->bios;
    if (!s->gpt && !plan->alongside) {
        s->count = 0;
        s->home = s->esp = s->bios = -1;
        s->first = MiB / s->sector;
        s->last = s->bytes / s->sector - 34;
    }
    uint64_t minimum_root = s->payload + s->payload / 2 + 4 * GiB;
    if (minimum_root < 16 * GiB)
        minimum_root = 16 * GiB;
    if (plan->preserve_home)
        minimum_root += s->home_used + 2 * GiB;
    uint64_t overhead = 2 * GiB + (s->esp < 0 ? 512 * MiB : 0) + (s->bios < 0 ? MiB : 0);
    uint64_t required = minimum_root + overhead;
    uint64_t start, size;
    free_extent(s, &start, &size);
    /* For coexistence, use free space first, then safely shrink one filesystem.
     * Filesystem shrink always precedes reducing its partition boundary. */
    if (size * s->sector < required && plan->alongside) {
        for (size_t i = 0; i < s->count; i++) {
            struct partition *p = &s->parts[i];
            if ((int)i == old_home || (int)i == s->esp || p->size * s->sector < required + 8 * GiB)
                continue;
            uint64_t used = UINT64_MAX;
            if (!strcmp(p->fs, "ext4")) {
                char probe[128];
                snprintf(probe, sizeof(probe), "%s/probe", work);
                if (RUN("/usr/bin/mount", "-t", "ext4", "-o", "ro,noload", p->device, probe) != 0)
                    continue;
                struct statvfs fs;
                if (!statvfs(probe, &fs))
                    used = ((uint64_t)fs.f_blocks - fs.f_bfree) * fs.f_frsize;
                if (RUN("/usr/bin/umount", probe) != 0)
                    return error("Cannot unmount shrink candidate");
            } else if (!strcmp(p->fs, "ntfs")) {
                char info[8192];
                if (READ(info, "/usr/bin/ntfsresize", "--info", p->device) != 0)
                    continue;
                const char *line = strstr(info, "You might resize at ");
                if (line)
                    used = strtoull(line + 20, NULL, 10);
            }
            uint64_t reserve = used == UINT64_MAX ? UINT64_MAX : used + used / 4 + 4 * GiB;
            if (reserve == UINT64_MAX || reserve + required > p->size * s->sector)
                continue;
            uint64_t original = p->size,
                     new_size = (p->size - (align_up(required, MiB) / s->sector)) /
                                (MiB / s->sector) * (MiB / s->sector);
            p->size = new_size;
            free_extent(s, &start, &size);
            if (size * s->sector < required) {
                p->size = original;
                continue;
            }
            s->shrink = (int)i;
            s->shrink_size = new_size;
            break;
        }
    }
    if (size * s->sector < required)
        return error("Not enough safely usable space: need at least %.1f GiB; no disk changes made",
                     (double)required / GiB);
    uint64_t cursor = start;
    if (s->bios < 0) {
        s->bios = append_partition(s, plan->target, cursor, MiB / s->sector, BIOS_TYPE);
        cursor += MiB / s->sector;
    }
    if (s->esp < 0) {
        s->esp = append_partition(s, plan->target, cursor, 512 * MiB / s->sector, ESP_TYPE);
        cursor += 512 * MiB / s->sector;
    }
    s->boot = append_partition(s, plan->target, cursor, 2 * GiB / s->sector, LINUX_TYPE);
    cursor += 2 * GiB / s->sector;
    uint64_t root_size = (start + size - cursor) / (MiB / s->sector) * (MiB / s->sector);
    s->root = append_partition(s, plan->target, cursor, root_size, LINUX_TYPE);
    s->home = s->root;
    if (s->root < 0 || s->home < 0 || s->boot < 0 || s->esp < 0 || s->bios < 0)
        return error("No partition slots remain");
    size_t used = (size_t)snprintf(plan->layout, sizeof(plan->layout), "\n");
    for (size_t i = 0; i < s->count; i++) {
        const struct partition *p = &s->parts[i];
        const char *action = !p->original_size     ? "CREATE"
                             : !p->keep            ? "DELETE"
                             : (int)i == s->shrink ? "SHRINK"
                             : "KEEP";
        int n =
            snprintf(plan->layout + used, sizeof(plan->layout) - used, "  %s %s: %.2f GiB%s%s%s\n",
                     action, p->device, (double)p->size * s->sector / GiB, *p->fs ? " " : "", p->fs,
                     (int)i == s->root   ? " (Btrfs system and home, shared space)"
                     : (int)i == s->boot ? " (/boot)"
                     : (int)i == s->source_home && plan->preserve_home ? " (source home retained; files copied and verified)"
                     : (int)i == s->esp  ? " (EFI)"
                     : (int)i == s->bios ? " (BIOS boot)"
                                         : "");
        if (n < 0 || (size_t)n >= sizeof(plan->layout) - used)
            return error("Storage review is too large");
        used += (size_t)n;
        if ((int)i == s->shrink)
            used += (size_t)snprintf(
                plan->layout + used, sizeof(plan->layout) - used,
                "    Previous size: %.2f GiB; preserve start, UUID, type and attributes.\n",
                (double)p->original_size * s->sector / GiB);
    }
    return true;
}

static bool kernel_images(const char *, bool);
static bool configuration_preflight(const struct storage *, const struct installation_plan *);

bool cenere_preflight(struct installation_plan *plan)
{
    set_stage("Checking the installation plan and available disk space");
    struct storage s;
    char snapshot[65536];
    if (!read_storage(plan->target, &s, snapshot, sizeof(snapshot)))
        return false;
    if (strcmp(snapshot, plan->snapshot))
        return error("Disk identity or layout changed after selection; select it again");
    if (!discover(&s, plan) || !image_source(plan->source,plan) || !plan_storage(&s, plan) ||
        !kernel_images(plan->source, false) || !configuration_preflight(&s, plan))
        return false;
    const char *tools[] = {
        "rsync", "sfdisk",       "udevadm", "mkfs.ext4", "mkfs.fat",   "cryptsetup", "arch-chroot",
        "blkid", "wipefs", "grub-install", "e2fsck",  "resize2fs", "ntfsresize", "loadkeys", "btrfs", "mkfs.btrfs", NULL};
    for (size_t i = 0; tools[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/usr/bin/%s", tools[i]);
        if (access(path, X_OK))
            return error("Required installation utility is missing: %s", tools[i]);
    }
    return true;
}

static bool write_file(const char *path, mode_t mode, const char *format, ...)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0)
        return error("Cannot write %s: %s", path, strerror(errno));
    /* System configuration must retain its specified access permissions even
     * when the installer is launched with a private umask. */
    if (fchmod(fd, mode) != 0) {
        int saved_errno = errno;
        close(fd);
        return error("Cannot set permissions on %s: %s", path, strerror(saved_errno));
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        return false;
    }
    va_list args;
    va_start(args, format);
    int count = vfprintf(file, format, args);
    va_end(args);
    bool ok = count >= 0 && fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0)
        ok = false;
    return ok || error("Cannot flush %s", path);
}
static bool uuid(const char *device, const char *type, char *value, size_t capacity)
{
    /* Probe the device itself: cached metadata and colliding old signatures
     * must never supply the UUID for a newly formatted filesystem. */
    char result[512], found[128] = "", detected[64] = "";
    if (READ(result, "/usr/bin/blkid", "--probe", "--output", "export",
             "--match-tag", "TYPE", "--match-tag", "UUID", device) != 0)
        return error("Cannot unambiguously identify %s on %s", type, device);
    char *saved = NULL;
    for (char *line = strtok_r(result, "\n", &saved); line; line = strtok_r(NULL, "\n", &saved)) {
        if (!strncmp(line, "DEVNAME=", 8) && !strcmp(line + 8, device))
            continue;
        else if (!strncmp(line, "TYPE=", 5) && !*detected && strlen(line + 5) < sizeof(detected))
            snprintf(detected, sizeof(detected), "%s", line + 5);
        else if (!strncmp(line, "UUID=", 5) && !*found && strlen(line + 5) < sizeof(found))
            snprintf(found, sizeof(found), "%s", line + 5);
        else
            return error("Unexpected filesystem identity on %s", device);
    }
    if (strcmp(detected, type) || !*found || strlen(found) >= capacity ||
        strspn(found, "0123456789abcdefABCDEF-") != strlen(found))
        return error("Expected %s with a valid UUID on %s; found %s", type, device, detected);
    char alias[PATH_MAX];
    struct stat source, resolved;
    snprintf(alias, sizeof(alias), "/dev/disk/by-uuid/%s", found);
    if (stat(device, &source) || !S_ISBLK(source.st_mode) || stat(alias, &resolved) ||
        !S_ISBLK(resolved.st_mode) || source.st_rdev != resolved.st_rdev)
        return error("UUID %s does not resolve to %s", found, device);
    snprintf(value, capacity, "%s", found);
    return true;
}

static bool clear_new_partition(const struct partition *partition)
{
    /* Existing/preserved partitions are never eligible, even when their number
     * is also used by a replacement entry in the confirmed partition plan. */
    if (partition->original_size || !partition->keep || !*partition->device)
        return error("Refusing to erase signatures from a retained partition");
    return DO("/usr/bin/wipefs", "--all", "--force", "--lock=yes", "--", partition->device);
}
static bool mount_fs(const char *device, const char *path, const char *type, bool *flag)
{
    if (!directory(path) || !DO("/usr/bin/mount", "-t", type, device, path))
        return false;
    *flag = true;
    int index = flag == &mounted_root   ? 0
                : flag == &mounted_home ? 1
                : flag == &mounted_boot ? 2
                                        : 3;
    flush_fds[index] = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    return flush_fds[index] >= 0 || error("Cannot open mounted filesystem for durable flush");
}
static bool mount_subvolume(const char *device, const char *path, const char *subvolume,
                            bool *flag)
{
    char options[128];
    snprintf(options,sizeof(options),"noatime,compress=zstd:3,subvol=%s",subvolume);
    if (!directory(path) || !DO("/usr/bin/mount","-t","btrfs","-o",options,device,path)) return false;
    *flag=true;
    int index=flag==&mounted_root ? 0 : 1;
    flush_fds[index]=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    return flush_fds[index]>=0 || error("Cannot open the mounted Btrfs subvolume for flushing");
}
static bool flush_filesystem(int index)
{
    return flush_fds[index] >= 0 && syncfs(flush_fds[index]) == 0
               ? true
               : error("Filesystem writeback failed during %s: %s", stage, strerror(errno));
}
static bool copy_tree(const char *source, const char *target, bool verify, bool home)
{
    char from[PATH_MAX + 2], to[PATH_MAX + 2], diff[4096];
    snprintf(from, sizeof(from), "%s/", source);
    snprintf(to, sizeof(to), "%s/", target);
    const char *args[40];
    size_t n = 0;
    args[n++] = "/usr/bin/rsync";
    args[n++] = home ? "-aHAXS" : "-aHAXSx";
    args[n++] = "--numeric-ids";
    if (verify) {
        args[n++] = "--checksum";
        args[n++] = "--dry-run";
        args[n++] = "--itemize-changes";
    } else {
        args[n++] = "--info=progress2";
        args[n++] = "--outbuf=L";
        args[n++] = "--human-readable";
        args[n++] = "--no-inc-recursive";
    }
    if (!home) {
        static const char *excluded[] = {"/home/***",
                                         "/dev/***",
                                         "/proc/***",
                                         "/sys/***",
                                         "/run/***",
                                         "/tmp/***",
                                         "/var/cache/pacman/pkg/***",
                                         "/var/log/***",
                                         "/etc/machine-id",
                                         "/var/lib/dbus/machine-id",
                                         "/etc/resolv.conf",
                                         NULL};
        for (size_t i = 0; excluded[i]; i++) {
            args[n++] = "--exclude";
            args[n++] = excluded[i];
        }
    }
    args[n++] = from;
    args[n++] = to;
    args[n] = NULL;
    int code = command(args, NULL, verify ? diff : NULL, sizeof(diff), false);
    return (code == 0 && (!verify || !*diff)) ||
           error("%s %s failed during %s%s", home ? "Home" : "System",
                 verify ? "verification" : "transfer", stage,
                 verify && code == 0 ? ": contents or metadata differ" : "");
}

static bool partition_disk(const struct storage *s, const struct installation_plan *plan)
{
    char backup[PATH_MAX], dump[32768];
    snprintf(backup, sizeof(backup), "%s/partition-table.before", work);
    if (READ(dump, "/usr/bin/sfdisk", "--dump", plan->target) == 0 &&
        !write_file(backup, 0600, "%s", dump))
        return false;
    set_stage("Preparing the reviewed disk layout");
    if (s->shrink >= 0) {
        const struct partition *p = &s->parts[s->shrink];
        uint64_t fs_bytes = p->size * s->sector - 256 * MiB;
        char size[64];
        set_stage("Resizing the existing filesystem on %s", p->device);
        if (!strcmp(p->fs, "ext4")) {
            int code = RUN("/usr/bin/e2fsck", "-f", "-p", p->device);
            if (code != 0 && code != 1)
                return error("Filesystem check refused shrink (exit %d)", code);
            snprintf(size, sizeof(size), "%lluK", (unsigned long long)(fs_bytes / 1024));
            if (!DO("/usr/bin/resize2fs", p->device, size))
                return false;
            char info[8192];
            if (READ(info, "/usr/bin/dumpe2fs", "-h", p->device) != 0)
                return error("Cannot verify shrunken filesystem");
            char *blocks = strstr(info, "Block count:"), *block_size = strstr(info, "Block size:");
            if (!blocks || !block_size ||
                strtoull(blocks + 12, NULL, 10) * strtoull(block_size + 11, NULL, 10) >
                    p->size * s->sector)
                return error("Shrunken filesystem does not fit planned partition");
        } else {
            snprintf(size, sizeof(size), "%llu", (unsigned long long)fs_bytes);
            if (!DO("/usr/bin/ntfsresize", "--no-action", "--size", size, p->device) ||
                !run((const char *const[]){"/usr/bin/ntfsresize", "--size", size, p->device, NULL},
                     "y\n"))
                return false;
        }
    }
    /* Preserve GUIDs, attributes, names and exact starts through the original
     * sfdisk dump; omit only entries explicitly reviewed for replacement. */
    char script[32768] = "";
    size_t used = 0;
    if (s->gpt) {
        char original[32768];
        if (READ(original, "/usr/bin/sfdisk", "--dump", plan->target) != 0)
            return error("Cannot save existing partition table");
        char *save = NULL;
        for (char *line = strtok_r(original, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *colon = strstr(line, " : ");
            if (!colon) {
                if (strncmp(line, "device:", 7))
                    used += (size_t)snprintf(script + used, sizeof(script) - used, "%s\n", line);
                continue;
            }
            *colon = '\0';
            for (size_t i = 0; i < s->count; i++) {
                const struct partition *p = &s->parts[i];
                if (!p->keep || !*p->uuid || strcmp(line, p->device))
                    continue;
                const char *fields = colon + 3;
                if ((int)i == s->shrink) {
                    char fields_copy[4096];
                    snprintf(fields_copy, sizeof(fields_copy), "%s", fields);
                    char *at = strstr(fields_copy, "size=");
                    char *after = at ? strchr(at, ',') : NULL;
                    if (!at || !after)
                        return error("Cannot retain partition metadata while shrinking");
                    *at = '\0';
                    used += (size_t)snprintf(script + used, sizeof(script) - used,
                                             "%s : %ssize=%llu%s\n", p->device, fields_copy,
                                             (unsigned long long)p->size, after);
                } else
                    used += (size_t)snprintf(script + used, sizeof(script) - used, "%s : %s\n",
                                             p->device, fields);
            }
        }
    } else
        used = (size_t)snprintf(script, sizeof(script), "label: gpt\nunit: sectors\n\n");
    for (size_t i = 0; i < s->count; i++) {
        const struct partition *p = &s->parts[i];
        if (p->keep && !*p->uuid)
            used += (size_t)snprintf(
                script + used, sizeof(script) - used, "%s : start=%llu, size=%llu, type=%s\n",
                p->device, (unsigned long long)p->start, (unsigned long long)p->size, p->type);
        if (used >= sizeof(script))
            return error("Partition script exceeds supported size");
    }
    if (!run((const char *const[]){"/usr/bin/sfdisk", "--lock", "--wipe", "never",
                                   "--wipe-partitions", "never", plan->target, NULL},
             script) ||
        !DO("/usr/bin/udevadm", "settle"))
        return false;
    char json[32768];
    if (READ(json, "/usr/bin/sfdisk", "--json", plan->target) != 0)
        return error("Cannot verify resulting partition table");
    json_object *object = json_tokener_parse(json), *pt = NULL, *parts = NULL;
    bool valid = object && json_object_object_get_ex(object, "partitiontable", &pt) &&
                 json_object_object_get_ex(pt, "partitions", &parts);
    if (valid)
        for (size_t i = 0; i < s->count; i++)
            if (s->parts[i].keep) {
                const struct partition *p = &s->parts[i];
                bool found = false;
                for (size_t j = 0; j < json_object_array_length(parts); j++) {
                    json_object *q = json_object_array_get_idx(parts, j);
                    if (!strcmp(p->device, string(q, "node")))
                        found = number(q, "start") == p->start && number(q, "size") == p->size &&
                                !strcasecmp(string(q, "type"), p->type) &&
                                (!*p->uuid || !strcasecmp(string(q, "uuid"), p->uuid));
                }
                if (!found)
                    valid = false;
            }
    if (object)
        json_object_put(object);
    return valid || error("Written partition geometry does not match the reviewed plan");
}

static bool encrypt_device(const char *device, const char *name, const char *password, bool format)
{
    if (format && !run((const char *const[]){"/usr/bin/cryptsetup", "luksFormat", "--type", "luks2",
                                             "--batch-mode", "--key-file", "-", device, NULL},
                       password))
        return false;
    return run(
        (const char *const[]){"/usr/bin/cryptsetup", "open", "--key-file", "-", device, name, NULL},
        password);
}

static bool configure_system(const struct storage *s, const struct installation_plan *plan,
                             const char *target, const char *root_device, const char *home_device);

static bool inside(const char *target, const char *input, char *output, size_t capacity,
                   const char *const args[])
{
    size_t count = 0;
    while (args[count]) count++;
    const char **argv = calloc(count + 3, sizeof(*argv));
    if (!argv) return error("Out of memory");
    argv[0] = "/usr/bin/arch-chroot";
    argv[1] = target;
    for (size_t i = 0; i < count; i++) argv[i + 2] = args[i];
    int code = command(argv, input, output, capacity, output != NULL);
    free(argv);
    return code == 0 || error("Target command %s failed (exit %d) during %s", args[0], code, stage);
}
#define TARGET(...) inside(target, NULL, NULL, 0, (const char *const[]){__VA_ARGS__, NULL})
#define FILE_AT(relative, mode, ...)                                                               \
    (snprintf(path, sizeof(path), "%s%s", target, relative), write_file(path, mode, __VA_ARGS__))

/* Generated by the builder from the signed dependency resolution. The final
 * manifest is authoritative: remove only ISO additions, using pacman itself. */
struct package_manifest {
    char *raw, *storage;
    const char **items;
    size_t count;
};
static void free_manifest(struct package_manifest *m)
{
    free(m->raw); free(m->storage); free(m->items);
    *m = (struct package_manifest){0};
}
static bool package_atom(const char *s)
{
    return *s && strlen(s) < 192 &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._+:-") == strlen(s);
}
static bool read_manifest(const char *root, const char *name, bool versions,
                          struct package_manifest *m)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/usr/share/cenere/%s", root, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 262144) {
        if (fd >= 0) close(fd);
        return error("Missing or invalid package manifest: %s", name);
    }
    m->raw = calloc((size_t)st.st_size + 1, 1);
    m->items = calloc(4097, sizeof(*m->items));
    if (!m->raw || !m->items) { close(fd); free_manifest(m); return error("Out of memory"); }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, m->raw + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used != (size_t)st.st_size || memchr(m->raw, 0, used) || m->raw[used-1] != '\n')
        goto invalid;
    m->storage = strdup(m->raw);
    if (!m->storage) goto invalid;
    char *save, *line = strtok_r(m->storage, "\n", &save);
    while (line) {
        char *space = strchr(line, ' ');
        if (versions) {
            if (!space || !package_atom(space + 1)) goto invalid;
            *space = 0;
        } else if (space) goto invalid;
        if (!package_atom(line) || m->count >= 4096 ||
            (m->count && strcmp(m->items[m->count-1], line) >= 0)) goto invalid;
        m->items[m->count++] = line;
        line = strtok_r(NULL, "\n", &save);
    }
    if (m->count) return true;
invalid:
    free_manifest(m);
    return error("Invalid or unsorted package manifest: %s", name);
}
static bool manifest_has(const struct package_manifest *m, const char *name)
{
    for (size_t i=0; i<m->count; i++) if (!strcmp(m->items[i], name)) return true;
    return false;
}
static bool manifest_contract(const char *root, struct package_manifest *final,
                              struct package_manifest *explicit, struct package_manifest *live)
{
    if (!read_manifest(root, "packages.final", true, final) ||
        !read_manifest(root, "packages.explicit", false, explicit) ||
        !read_manifest(root, "packages.live-only", false, live)) return false;
    if (manifest_has(final,"cenere") || !manifest_has(live,"cenere") ||
        !manifest_has(final,"base") || !manifest_has(final,"pacman") ||
        !manifest_has(final,"systemd") || !manifest_has(final,"mkinitcpio") ||
        !manifest_has(final,"grub"))
        return error("The image must select an Arch/systemd/mkinitcpio/GRUB system without the installer");
    for (size_t i=0; i<explicit->count; i++)
        if (!manifest_has(final,explicit->items[i]))
            return error("An explicit system package is absent from the final package set");
    for (size_t i=0; i<live->count; i++)
        if (manifest_has(final,live->items[i]))
            return error("The image marks a system dependency as live-only");
    return true;
}
static bool target_package_operation(const char *target, const char *operation,
                                     const char *option, const struct package_manifest *packages)
{
    const char **args = calloc(packages->count + 6, sizeof(*args));
    if (!args) return error("Out of memory");
    args[0]="/usr/bin/pacman"; args[1]=operation; args[2]=option; args[3]="--";
    for (size_t i=0; i<packages->count; i++) args[i+4]=packages->items[i];
    bool ok=inside(target,NULL,NULL,0,args);
    free(args);
    return ok;
}
static bool finish_package_selection(const struct installation_plan *plan, const char *target)
{
    struct package_manifest final={0}, explicit={0}, live={0};
    bool ok = manifest_contract(plan->source,&final,&explicit,&live);
    set_stage("Removing the installer and live-only packages from the installed system");
    if (ok) ok=target_package_operation(target,"-R","--noconfirm",&live);
    if (ok) ok=target_package_operation(target,"-D","--asdeps",&final);
    if (ok) ok=target_package_operation(target,"-D","--asexplicit",&explicit);
    char *installed=calloc(262145,1);
    if (!installed) ok=error("Out of memory");
    if (ok) ok=inside(target,NULL,installed,262145,
                     (const char *const[]){"/usr/bin/pacman","-Q",NULL});
    if (ok && strcmp(installed,final.raw))
        ok=error("Installed packages/versions differ from the reviewed final system");
    if (ok) status_message("Final package set verified: %zu packages; Cenere and %zu live-only packages removed",
                           final.count,live.count-1);
    free(installed);
    free_manifest(&final); free_manifest(&explicit); free_manifest(&live);
    return ok;
}

/* Archiso exports /boot kernels into the ISO boot tree. The signed kernel
 * packages also retain their images and package names under /usr/lib/modules. */
static bool kernel_images(const char *root, bool restore)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/usr/lib/modules", root);
    DIR *dir = opendir(path);
    if (!dir)
        return error("Payload kernel modules are missing");
    struct dirent *entry;
    size_t count = 0;
    bool ok = true;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        char base[128] = "", image[PATH_MAX], destination[PATH_MAX];
        snprintf(path, sizeof(path), "%s/usr/lib/modules/%s/pkgbase", root, entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        bool have = fgets(base, sizeof(base), file) != NULL && fgetc(file)==EOF && !ferror(file);
        fclose(file);
        trim(base);
        if (!have || !*base || strspn(base, "abcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(base)) {
            ok = error("Invalid kernel package name");
            break;
        }
        snprintf(image, sizeof(image), "%s/usr/lib/modules/%s/vmlinuz", root, entry->d_name);
        struct stat st;
        if (stat(image, &st) || !S_ISREG(st.st_mode)) {
            ok = error("Payload kernel image is missing: %s", base);
            break;
        }
        count++;
        if (restore) {
            snprintf(destination, sizeof(destination), "%s/boot/vmlinuz-%s", root, base);
            if (!DO("/usr/bin/cp", "--", image, destination)) {
                ok = false;
                break;
            }
        }
    }
    closedir(dir);
    return ok && (count > 0 || error("Payload contains no installable kernels"));
}

static bool configure_initramfs(const char *target, bool encrypt)
{
    char path[PATH_MAX+64], hooks[256];
    snprintf(hooks,sizeof(hooks),
        "HOOKS=(base systemd autodetect microcode modconf kms keyboard sd-vconsole block %sfilesystems fsck)\n",
        encrypt?"sd-encrypt ":"");
    snprintf(path,sizeof(path),"%s/etc/mkinitcpio.conf.d/cenere.conf",target);
    if (!write_file(path,0644,"%s",hooks)) return false;
    /* Presets with ALL_config bypass conf.d. Preserve the image's other settings
     * and put the same storage/keyboard hooks in its standard main config. */
    snprintf(path,sizeof(path),"%s/etc/mkinitcpio.conf",target);
    int fd=open(path,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0644);
    struct stat st;
    if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid()) {
        if (fd>=0) close(fd);
        return error("Cannot safely configure the main mkinitcpio configuration");
    }
    const char *notice="\n# Cenere storage and keyboard configuration\n";
    bool ok=write_all(fd,notice,strlen(notice)) &&
            write_all(fd,hooks,strlen(hooks));
    if (close(fd)) ok=false;
    return ok || error("Cannot finish the mkinitcpio configuration");
}

static bool archive_contains(const char *listing, const char *wanted)
{
    for (const char *at=listing; *at;) {
        const char *end=strchr(at,'\n');
        size_t length=end?(size_t)(end-at):strlen(at);
        if (length>=2 && !strncmp(at,"./",2)) {at+=2;length-=2;}
        if (length==strlen(wanted) && !memcmp(at,wanted,length)) return true;
        if (!end) break;
        at=end+1;
    }
    return false;
}

static bool initramfs_contract(const char *listing, bool encrypt)
{
    const char *required[]={"usr/lib/systemd/systemd","usr/lib/systemd/systemd-vconsole-setup",
                            "usr/bin/loadkeys","etc/vconsole.conf",NULL};
    for (size_t i=0;required[i];i++)
        if (!archive_contains(listing,required[i]))
            return error("Generated initramfs lacks %s",required[i]);
    if (encrypt && (!archive_contains(listing,"usr/lib/systemd/systemd-cryptsetup") ||
        !archive_contains(listing,"usr/lib/systemd/system-generators/systemd-cryptsetup-generator") ||
        !archive_contains(listing,"etc/crypttab")))
        return error("Generated initramfs cannot unlock the selected encrypted root");
    return true;
}

static bool verify_initramfs(const struct installation_plan *plan, const char *target)
{
    set_stage("Verifying the final initramfs keyboard and disk-unlock support");
    char path[PATH_MAX+512];
    snprintf(path,sizeof(path),"%s/usr/lib/modules",target);
    DIR *dir=opendir(path);
    if (!dir) return error("Cannot inspect installed kernels");
    size_t capacity=4*1024*1024, count=0;
    char *listing=calloc(capacity,1);
    bool ok=listing!=NULL;
    if (!ok) error("Out of memory");
    struct dirent *entry;
    while (ok && (entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        snprintf(path,sizeof(path),"%s/usr/lib/modules/%s/pkgbase",target,entry->d_name);
        FILE *file=fopen(path,"r");
        if (!file) continue;
        char base[128]="",image[192];
        bool have=fgets(base,sizeof(base),file)!=NULL && fgetc(file)==EOF && !ferror(file);
        fclose(file); trim(base);
        if (!have || !*base || strspn(base,"abcdefghijklmnopqrstuvwxyz0123456789-_")!=strlen(base)) {
            ok=error("Invalid installed kernel package name"); break;
        }
        snprintf(image,sizeof(image),"/boot/initramfs-%s.img",base);
        const char *args[]={"/usr/bin/arch-chroot",target,"/usr/bin/lsinitcpio","--nocolor","--list",image,NULL};
        int code=command(args,NULL,listing,capacity,false);
        if (code) ok=error("Cannot inspect %s (exit %d)",image,code);
        else ok=initramfs_contract(listing,plan->encrypt);
        count++;
    }
    free(listing);closedir(dir);
    return ok && (count>0 || error("No generated initramfs was verified"));
}

/* A reused /boot must not offer kernels belonging to the replaced root.
 * Use the old root's validated package metadata, never a broad /boot deletion:
 * other loaders, EFI files and unrelated boot material remain intact. */
static bool clear_previous_kernels(const char *previous, const char *target)
{
    if (!kernel_images(previous,false)) return false;
    char path[PATH_MAX+512];
    snprintf(path,sizeof(path),"%s/usr/lib/modules",previous);
    DIR *dir=opendir(path);
    if (!dir) return error("Cannot inspect the previous kernel packages");
    struct dirent *entry; bool ok=true;
    while (ok && (entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        snprintf(path,sizeof(path),"%s/usr/lib/modules/%s/pkgbase",previous,entry->d_name);
        FILE *file=fopen(path,"r");
        if (!file) continue;
        char base[128]=""; bool have=fgets(base,sizeof(base),file)!=NULL && fgetc(file)==EOF && !ferror(file);
        fclose(file); trim(base);
        if (!have || !*base || strspn(base,"abcdefghijklmnopqrstuvwxyz0123456789-_")!=strlen(base)) {
            ok=error("Invalid previous kernel package name"); break;
        }
        const char *patterns[]={"vmlinuz-%s","initramfs-%s.img","initramfs-%s-fallback.img"};
        for (size_t i=0;i<sizeof(patterns)/sizeof(patterns[0]);i++) {
            char name[192];snprintf(name,sizeof(name),patterns[i],base);
            snprintf(path,sizeof(path),"%s/boot/%s",target,name);
            if (unlink(path) && errno!=ENOENT) {
                ok=error("Cannot replace the previous kernel boot file: %s",name); break;
            }
        }
    }
    closedir(dir);
    return ok;
}

static bool configuration_preflight(const struct storage *s, const struct installation_plan *plan)
{
    char path[PATH_MAX + 256], resolved[PATH_MAX], line[512];
    struct installation_plan keyboard=*plan;
    if (!cenere_select_keyboard(&keyboard,plan->keymap,false) ||
        strcmp(keyboard.xkb_layout,plan->xkb_layout) || strcmp(keyboard.xkb_model,plan->xkb_model) ||
        strcmp(keyboard.xkb_variant,plan->xkb_variant) || strcmp(keyboard.xkb_options,plan->xkb_options))
        return error("Keyboard correspondence changed after selection");
    snprintf(path, sizeof(path), "%s/usr/share/zoneinfo/%s", plan->source, plan->timezone);
    char prefix[PATH_MAX + 256];
    snprintf(prefix, sizeof(prefix), "%s/usr/share/zoneinfo/", plan->source);
    if (!realpath(path, resolved) || strncmp(resolved, prefix, strlen(prefix)))
        return error("Selected timezone is unavailable or outside zoneinfo");
    if (strcmp(plan->locale, "C.UTF-8")) {
        snprintf(path, sizeof(path), "%s/etc/locale.gen", plan->source);
        FILE *file = fopen(path, "r");
        bool found = false;
        if (!file)
            return error("Payload locale catalogue is missing");
        while (fgets(line, sizeof(line), file)) {
            char *at = line;
            while (*at == '#' || isspace((unsigned char)*at))
                at++;
            char name[128], encoding[128];
            if (sscanf(at, "%127s %127s", name, encoding) == 2 && !strcmp(name, plan->locale) &&
                !strcmp(encoding, "UTF-8"))
                found = true;
        }
        fclose(file);
        if (!found)
            return error("Selected locale is unavailable in the payload");
    }
    if (command((const char *const[]){"/usr/bin/loadkeys", "--mktable", plan->keymap, NULL}, NULL,
                NULL, 0, true) != 0)
        return error("Selected console keyboard layout is unavailable");
    snprintf(path,sizeof(path),"%s/usr/lib/cenere/integration",plan->source);
    struct stat hook;
    if (!lstat(path,&hook)) {
        if (!S_ISREG(hook.st_mode) || hook.st_uid!=0 || !(hook.st_mode&S_IXUSR) ||
            (hook.st_mode&(S_IWGRP|S_IWOTH))) return error("Unsafe or non-executable image integration");
    } else if (errno!=ENOENT) return error("Cannot inspect image integration");
    if (!*plan->password || !*plan->username)
        return error("Account name and password are required");
    struct package_manifest final={0}, explicit={0}, live={0};
    bool manifests_ok=manifest_contract(plan->source,&final,&explicit,&live);
    free_manifest(&final); free_manifest(&explicit); free_manifest(&live);
    if (!manifests_ok) return false;
    /* Account/group names must be free in the immutable payload before any
     * disk is modified. The temporary live account is removed on installation. */
    for (size_t kind = 0; kind < 2; kind++) {
        snprintf(path, sizeof(path), "%s/etc/%s", plan->source, kind ? "group" : "passwd");
        FILE *file = fopen(path, "r");
        if (!file)
            return error("Payload account database is missing");
        bool conflict = false;
        while (fgets(line, sizeof(line), file)) {
            char name[128];
            if (sscanf(line, "%127[^:]", name) == 1 && strcmp(name, plan->live_user) &&
                !strcmp(name, plan->username))
                conflict = true;
        }
        fclose(file);
        if (conflict)
            return error("Chosen username conflicts with a system account/group");
    }
    if (plan->preserve_home) {
        const char *device = s->parts[s->source_home].device;
        if (!s->btrfs_home) {
        char header[8192];
        if (READ(header, "/usr/bin/dumpe2fs", "-h", device) != 0)
            return error("Cannot inspect the home journal state");
        char *features = strstr(header, "Filesystem features:"),
             *state = strstr(header, "Filesystem state:");
        bool clean = features && state;
        if (features) {
            char *end = strchr(features, '\n');
            if (end)
                *end = '\0';
            if (strstr(features, "needs_recovery"))
                clean = false;
        }
        if (state) {
            state += strlen("Filesystem state:");
            while (*state == ' ')
                state++;
            char *end = strchr(state, '\n');
            if (end)
                *end = '\0';
            if (strcmp(state, "clean"))
                clean = false;
        }
        if (!clean)
            return error("Existing home has an unclean filesystem or pending journal recovery; no "
                         "changes made");
        if (command((const char *const[]){"/usr/bin/e2fsck", "-f", "-n", device, NULL}, NULL, NULL,
                    0, true) != 0)
            return error("Existing home needs filesystem recovery/checking before it can be "
                         "preserved safely");
        }
        char probe[128];
        snprintf(probe, sizeof(probe), "%s/probe", work);
        if (!mount_preserved_home(s, plan, probe))
            return error("Cannot inspect preserved account ownership");
        snprintf(path, sizeof(path), "%s/%s", probe, plan->username);
        struct stat st;
        bool present = lstat(path, &st) == 0, valid = true;
        if (s->reuse_btrfs) {
            char property[128];
            if (READ(property,"/usr/bin/btrfs","property","get","-t","s",probe,"ro") ||
                strcmp(property,"ro=false\n")) valid=false;
        }
        if (present && (!S_ISDIR(st.st_mode) || st.st_uid < 1000 || st.st_gid < 1000))
            valid = false;
        if (present && valid) {
            snprintf(path, sizeof(path), "%s/etc/passwd", plan->source);
            FILE *file = fopen(path, "r");
            if (!file)
                valid = false;
            else {
                while (fgets(line, sizeof(line), file)) {
                    char name[128];
                    unsigned id;
                    if (sscanf(line, "%127[^:]:%*[^:]:%u", name, &id) == 2 &&
                        strcmp(name, plan->live_user) &&
                        (!strcmp(name, plan->username) || id == st.st_uid))
                        valid = false;
                }
                fclose(file);
            }
        }
        if (!close_probe(probe, true))
            return error("Cannot unmount account discovery filesystem");
        if (!valid)
            return error("Preserved home ownership/account conflicts with the installed system");
    }
    return true;
}

static bool configure_system(const struct storage *s, const struct installation_plan *plan,
                             const char *target, const char *root_device, const char *home_device)
{
    char path[PATH_MAX], root_uuid[128], home_uuid[128], boot_uuid[128], efi_uuid[128];
    char root_luks[128] = "";
    set_stage("Preparing the installed system");
    for (const char **name = (const char *[]){"dev", "proc", "sys", "run", "tmp", "var/log", NULL};
         *name; name++) {
        snprintf(path, sizeof(path), "%s/%s", target, *name);
        if (!directory(path))
            return false;
        chmod(path, !strcmp(*name, "tmp") ? 01777 : 0755);
    }
    if (!FILE_AT("/etc/machine-id", 0444, "")) return false;
    snprintf(path, sizeof(path), "%s/var/lib/dbus/machine-id", target);
    if (unlink(path) && errno != ENOENT) return error("Cannot reset D-Bus identity");
    if (symlink("/etc/machine-id", path)) return error("Cannot link D-Bus identity");
    /* Formatting does not itself emit a device-change event. Refresh udev's
     * UUID links and require them to identify the same devices as the probe. */
    if (!DO("/usr/bin/udevadm", "trigger", "--action=change", "--settle",
            s->parts[s->root].device, root_device, s->parts[s->boot].device,
            s->parts[s->esp].device) ||
        !uuid(root_device, "btrfs", root_uuid, sizeof(root_uuid)) ||
        !uuid(home_device, "btrfs", home_uuid, sizeof(home_uuid)) ||
        !uuid(s->parts[s->boot].device, "ext4", boot_uuid, sizeof(boot_uuid)) ||
        !uuid(s->parts[s->esp].device, "vfat", efi_uuid, sizeof(efi_uuid)))
        return false;
    set_stage("Configuring filesystems and encryption");
    const char *root_subvol=s->reuse_btrfs ? s->root_subvol : "@";
    const char *home_subvol=s->reuse_btrfs ? s->home_subvol : "@home";
    if (strcmp(root_uuid,home_uuid)) return error("Root and home must share one Btrfs filesystem");
    if (!FILE_AT("/etc/fstab", 0644,
                 "# Generated by Cenere; root/home share one Btrfs filesystem.\n"
                 "UUID=%s / btrfs defaults,noatime,compress=zstd:3,subvol=%s 0 0\n"
                 "UUID=%s /home btrfs defaults,noatime,compress=zstd:3,subvol=%s 0 0\n"
                 "UUID=%s /boot ext4 defaults,noatime 0 2\nUUID=%s /boot/efi vfat umask=0077 0 2\n",
                 root_uuid,root_subvol,home_uuid,home_subvol,boot_uuid,efi_uuid) ||
        !FILE_AT("/etc/crypttab",0600,"# The shared root/home container is unlocked in the initramfs.\n"))
        return false;
    if (plan->encrypt) {
        if (!uuid(s->parts[s->root].device, "crypto_LUKS", root_luks, sizeof(root_luks)) ||
            !FILE_AT("/etc/crypttab.initramfs", 0600, "cenere-root UUID=%s none luks\n", root_luks))
            return false;
    } else if (!FILE_AT("/etc/crypttab.initramfs",0600,"")) return false;
    if (!configure_initramfs(target,plan->encrypt)) return false;
    set_stage("Setting language, keyboard, timezone and computer name");
    if (!FILE_AT("/etc/locale.conf", 0644, "LANG=%s\n", plan->locale) ||
        !FILE_AT("/etc/vconsole.conf", 0644, "KEYMAP=%s\n", plan->keymap) ||
        !FILE_AT("/etc/hostname", 0644, "%s\n", plan->hostname) ||
        !FILE_AT("/etc/hosts", 0644,
                 "127.0.0.1 localhost\n::1 localhost\n127.0.1.1 %s.localdomain %s\n",
                 plan->hostname, plan->hostname))
        return false;
    snprintf(path,sizeof(path),"%s/etc/X11",target);
    if (!directory(path)) return false;
    chmod(path,0755);
    snprintf(path,sizeof(path),"%s/etc/X11/xorg.conf.d",target);
    if (!directory(path)) return false;
    chmod(path,0755);
    if (!FILE_AT("/etc/X11/xorg.conf.d/00-keyboard.conf",0644,
        "# Selected in Cenere before passwords were entered.\n"
        "Section \"InputClass\"\n Identifier \"system-keyboard\"\n MatchIsKeyboard \"on\"\n"
        " Option \"XkbLayout\" \"%s\"\n Option \"XkbModel\" \"%s\"\n"
        " Option \"XkbVariant\" \"%s\"\n Option \"XkbOptions\" \"%s\"\nEndSection\n",
        plan->xkb_layout,plan->xkb_model,plan->xkb_variant,plan->xkb_options)) return false;
    if (strcmp(plan->locale, "C.UTF-8")) {
        if (!FILE_AT("/etc/locale.gen", 0644, "%s UTF-8\n", plan->locale) ||
            !TARGET("/usr/bin/locale-gen"))
            return false;
    }
    char zone[PATH_MAX];
    snprintf(zone, sizeof(zone), "%s/usr/share/zoneinfo/%s", target, plan->timezone);
    struct stat zone_stat;
    if (stat(zone, &zone_stat) || !S_ISREG(zone_stat.st_mode))
        return error("Selected timezone is unavailable");
    snprintf(path, sizeof(path), "%s/etc/localtime", target);
    if (unlink(path) && errno != ENOENT)
        return error("Cannot replace timezone link");
    snprintf(zone, sizeof(zone), "/usr/share/zoneinfo/%s", plan->timezone);
    if (symlink(zone, path))
        return error("Cannot set timezone");
    set_stage("Creating the user account");
    char account[2048];
    const char *get_live[] = {"/usr/bin/getent", "passwd", plan->live_user, NULL};
    if (*plan->live_user && inside(target, NULL, account, sizeof(account), get_live) &&
        !TARGET("/usr/bin/userdel", plan->live_user))
        return false;
    problem[0] = '\0';
    unsigned uid = 1000, gid = 1000;
    snprintf(path, sizeof(path), "%s/home/%s", target, plan->username);
    struct stat home_stat;
    if (lstat(path, &home_stat) == 0) {
        if (!S_ISDIR(home_stat.st_mode) || home_stat.st_uid < 1000 || home_stat.st_gid < 1000)
            return error("Preserved user home has unsafe ownership or is not a directory");
        uid = home_stat.st_uid;
        gid = home_stat.st_gid;
    } else if (errno != ENOENT)
        return error("Cannot inspect user home");
    else {
        char home_path[PATH_MAX];
        snprintf(home_path, sizeof(home_path), "%s/home", target);
        DIR *dir = opendir(home_path);
        if (!dir)
            return error("Cannot inspect existing home owners");
        struct dirent *entry;
        while ((entry = readdir(dir)))
            if (entry->d_name[0] != '.') {
                struct stat st;
                if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                    st.st_uid >= uid && st.st_uid < 60000)
                    uid = st.st_uid + 1;
            }
        closedir(dir);
        gid = uid;
    }
    char uid_text[32], gid_text[32];
    snprintf(uid_text, sizeof(uid_text), "%u", uid);
    snprintf(gid_text, sizeof(gid_text), "%u", gid);
    const char *get_uid[] = {"/usr/bin/getent", "passwd", uid_text, NULL};
    if (inside(target, NULL, account, sizeof(account), get_uid))
        return error("Preserved user ID conflicts with a system account");
    problem[0] = '\0';
    const char *get_gid[] = {"/usr/bin/getent", "group", gid_text, NULL};
    if (!inside(target, NULL, account, sizeof(account), get_gid) &&
        !TARGET("/usr/bin/groupadd", "--gid", gid_text, plan->username))
        return false;
    problem[0] = '\0';
    if (!TARGET("/usr/bin/useradd", "--uid", uid_text, "--gid", gid_text, "--groups", "wheel",
                "--shell", "/bin/bash", "--create-home", plan->username))
        return false;
    if (plan->preserve_home && lstat(path, &home_stat) == 0) {
        char user_home[128];
        snprintf(user_home, sizeof(user_home), "/home/%s/", plan->username);
        /* Add missing desktop defaults as the user. Existing files, directory
         * modes and timestamps stay intact; no recursive ownership rewrite. */
        if (!TARGET("/usr/bin/setpriv", "--reuid", uid_text, "--regid", gid_text, "--init-groups",
                    "/usr/bin/rsync", "-rlt", "--ignore-existing", "--omit-dir-times", "/etc/skel/",
                    user_home))
            return false;
    }
    char credentials[512];
    snprintf(credentials, sizeof(credentials), "%s:%s\n", plan->username, plan->password);
    bool password_ok =
        inside(target, credentials, NULL, 0, (const char *const[]){"/usr/bin/chpasswd", NULL});
    explicit_bzero(credentials, sizeof(credentials));
    if (!password_ok || !TARGET("/usr/bin/usermod", "--lock", "root"))
        return false;
    snprintf(path,sizeof(path),"%s/usr/lib/cenere/integration",target);
    if (access(path,F_OK)==0 && !TARGET("/usr/lib/cenere/integration","configure",plan->username,
        plan->xkb_layout,plan->xkb_model,plan->xkb_variant,plan->xkb_options)) return false;
    set_stage("Initializing package signing keys offline");
    /* Trust the vendor keyrings shipped by the installed distribution. */
    if (!TARGET("/usr/bin/pacman-key", "--init") ||
        !TARGET("/usr/bin/pacman-key", "--populate") ||
        !TARGET("/usr/bin/gpgconf", "--homedir", "/etc/pacman.d/gnupg", "--kill", "all"))
        return false;
    set_stage("Preparing kernels and boot files");
    if (s->reuse_btrfs) {
        char previous[128];
        snprintf(previous,sizeof(previous),"%s/btrfs/@cenere-previous",work);
        if (!clear_previous_kernels(previous,target)) return false;
    }
    if (!kernel_images(target, true) ||
        !TARGET("/usr/bin/mkinitcpio", "-P"))
        return false;
    /* Append selected-device boot settings while retaining package-provided
     * defaults and unrelated distribution settings. */
    snprintf(path, sizeof(path), "%s/etc/default/grub", target);
    FILE *grub = fopen(path, "a");
    if (!grub)
        return error("Cannot configure GRUB");
    fprintf(grub, "\n# Cenere installation\nGRUB_DISTRIBUTOR=%s\nGRUB_DISABLE_OS_PROBER=%s\n",
            plan->image_id, plan->alongside ? "false" : "true");
    fprintf(grub, "GRUB_CMDLINE_LINUX=\"rootflags=subvol=%s", root_subvol);
    if (plan->encrypt) fprintf(grub, " rd.luks.name=%s=cenere-root", root_luks);
    fprintf(grub, "\"\n");
    if (fclose(grub))
        return error("Cannot flush GRUB defaults");
    char boot_id[80];
    snprintf(boot_id,sizeof(boot_id),"%s",plan->image_id);
    if (plan->alongside)
        snprintf(boot_id, sizeof(boot_id), "%s-%.8s", plan->image_id, root_uuid);
    if (!strcmp(plan->firmware, "uefi")) {
        if (!TARGET("/usr/bin/grub-install", "--target=x86_64-efi", "--efi-directory=/boot/efi",
                    "--bootloader-id", boot_id, "--recheck"))
            return false;
        if (!*s->parts[s->esp].fs &&
            !TARGET("/usr/bin/grub-install", "--target=x86_64-efi", "--efi-directory=/boot/efi",
                    "--removable", "--no-nvram", "--recheck"))
            return false;
    } else if (!TARGET("/usr/bin/grub-install", "--target=i386-pc", "--recheck", plan->target))
        return false;
    return TARGET("/usr/bin/grub-mkconfig", "-o", "/boot/grub/grub.cfg");
}

int cenere_install(const struct installation_plan *given)
{
    struct installation_plan plan = *given;
    struct storage s;
    char snapshot[65536];
    char target[128], home[256], boot[256], efi[512], probe[128], backup[256], top[128];
    int result = 1;
    bool changed = false;
    int lock = open("/run/cenere.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_stat;
    if (lock < 0 || fstat(lock, &lock_stat) || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_uid != 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0)
            close(lock);
        fprintf(stderr, "Another installer is active, or the installer lock is unavailable.\n");
        return 1;
    }
    if (!workspace() || !begin_log())
        goto finish;
    snprintf(target, sizeof(target), "%s/target", work);
    snprintf(home, sizeof(home), "%s/home", target);
    snprintf(boot, sizeof(boot), "%s/boot", target);
    snprintf(efi, sizeof(efi), "%s/efi", boot);
    snprintf(probe, sizeof(probe), "%s/probe", work);
    snprintf(top, sizeof(top), "%s/btrfs", work);
    snprintf(backup, sizeof(backup), "%s/@cenere-previous", top);
    status_message("4 of 4: Installation | Keyboard > Disk > Preferences > Account > Summary > [Install]");
    set_stage("Rechecking the disk and confirmed installation plan");
    if (!read_storage(plan.target, &s, snapshot, sizeof(snapshot)) ||
        strcmp(snapshot, plan.snapshot)) {
        error("Disk changed or became active after confirmation; no changes made");
        goto finish;
    }
    if (!image_source(plan.source,&plan) || strcmp(plan.image_id,given->image_id) ||
        strcmp(plan.live_user,given->live_user) || !discover(&s, &plan) ||
        !plan_storage(&s, &plan) || !kernel_images(plan.source, false) ||
        !configuration_preflight(&s, &plan) || strcmp(plan.layout, given->layout)) {
        if (!*problem)
            error("The reviewed storage plan changed; no changes made");
        goto finish;
    }
    changed = true;
    if (!s.reuse_btrfs && !partition_disk(&s, &plan))
        goto finish;
    set_stage("Preparing the shared Btrfs system and home filesystem");
    const char *root_device=s.parts[s.root].device, *home_device=root_device;
    if (!s.reuse_btrfs && !clear_new_partition(&s.parts[s.root])) goto finish;
    if (plan.encrypt) {
        const char *secret=s.reuse_btrfs ? plan.existing_password : plan.password;
        if (!encrypt_device(root_device,"cenere-root",secret,!s.reuse_btrfs)) goto finish;
        root_open=true;
        root_device=home_device="/dev/mapper/cenere-root";
    }
    if (!s.reuse_btrfs && !DO("/usr/bin/mkfs.btrfs","-f","-L","cenere-system",root_device))
        goto finish;
    if (!directory(top) || !DO("/usr/bin/mount","-t","btrfs","-o","subvolid=5",root_device,top))
        goto finish;
    top_mounted=true;
    const char *root_subvol=s.reuse_btrfs ? s.root_subvol : "@";
    const char *home_subvol=s.reuse_btrfs ? s.home_subvol : "@home";
    char root_path[256], home_path[256];
    snprintf(root_path,sizeof(root_path),"%s/%s",top,root_subvol);
    snprintf(home_path,sizeof(home_path),"%s/%s",top,home_subvol);
    if (s.reuse_btrfs) {
        uint64_t root_id,home_id;
        struct stat previous;
        if (!subvolume_id(root_path,&root_id) || !subvolume_id(home_path,&home_id) ||
            root_id!=s.root_id || home_id!=s.home_id || !lstat(backup,&previous)) {
            error("Existing Btrfs subvolumes changed after confirmation"); goto finish;
        }
        if (rename(root_path,backup)) { error("Cannot retain the previous root subvolume"); goto finish; }
        status_message("Previous root retained at @cenere-previous until the installation is verified");
    }
    if (!DO("/usr/bin/btrfs","subvolume","create",root_path) ||
        (!s.reuse_btrfs && !DO("/usr/bin/btrfs","subvolume","create",home_path))) goto finish;
    if (!mount_subvolume(root_device,target,root_subvol,&mounted_root) ||
        !mount_subvolume(root_device,home,home_subvol,&mounted_home)) goto finish;
    if (plan.preserve_home && !s.reuse_btrfs) {
        set_stage("Copying the preserved home into the new shared filesystem");
        if (!mount_preserved_home(&s,&plan,probe)) goto finish;
        bool copied=copy_tree(probe,home,false,true) && copy_tree(probe,home,true,true);
        bool closed=close_probe(probe,true);
        if (!copied || !closed || !flush_filesystem(1)) goto finish;
        status_message("Home copy verified and flushed; the original source partition remains unchanged");
    }
    set_stage("Preparing the boot filesystem");
    if ((!s.reuse_btrfs && (!clear_new_partition(&s.parts[s.boot]) ||
                          !DO("/usr/bin/mkfs.ext4", "-F", "-L", "cenere-boot", s.parts[s.boot].device))) ||
        !mount_fs(s.parts[s.boot].device, boot, "ext4", &mounted_boot))
        goto finish;
    set_stage("Preparing the EFI system partition");
    if (!*s.parts[s.esp].fs && (!clear_new_partition(&s.parts[s.esp]) ||
                              !DO("/usr/bin/mkfs.fat", "-F", "32", s.parts[s.esp].device)))
        goto finish;
    if (!mount_fs(s.parts[s.esp].device, efi, "vfat", &mounted_efi))
        goto finish;
    set_stage("Copying the prepared system; this can take several minutes");
    if (!copy_tree(plan.source, target, false, false))
        goto finish;
    if (!configure_system(&s, &plan, target, root_device, home_device) ||
        !finish_package_selection(&plan, target) || !verify_initramfs(&plan,target))
        goto finish;
    set_stage("Finishing writes to the installed filesystems");
    for (int i = 0; i < 4; i++)
        if (!flush_filesystem(i))
            goto finish;
    if (s.reuse_btrfs) {
        if (!DO("/usr/bin/btrfs","subvolume","set-default",root_path)) goto finish;
        if (!DO("/usr/bin/btrfs","subvolume","delete","--recursive","--commit-after",backup) ||
            !flush_filesystem(0)) goto finish;
    }
    status_message("System setup completed; saving the installation log");
    if (!save_log(target) || !flush_filesystem(0))
        goto finish;
    result = 0;
finish:
    if (result)
        status_message("Installation stopped during %s: %s. %s", stage, problem,
                       changed ? "The target may be partially changed. Preserved source home data was not formatted; "
                                 "inspect @cenere-previous for any retained prior Btrfs root."
                               : "No disk modifications were started.");
    if (result && mounted_root && !save_log(target))
        status_message("The full log remains in the live environment: %s", log_path);
    set_stage("Unmounting the installed filesystems");
    for (int i = 0; i < 4; i++)
        if (flush_fds[i] >= 0) {
            close(flush_fds[i]);
            flush_fds[i] = -1;
        }
    if (mounted_efi) {
        if (RUN("/usr/bin/umount", efi)) {
            status_message("Cleanup failed; inspect %s before rebooting", log_path);
            result = 1;
        }
        mounted_efi = false;
    }
    if (mounted_boot) {
        if (RUN("/usr/bin/umount", boot)) {
            status_message("Cleanup failed; inspect %s before rebooting", log_path);
            result = 1;
        }
        mounted_boot = false;
    }
    if (mounted_home) {
        if (RUN("/usr/bin/umount", home)) {
            status_message("Cleanup failed; inspect %s before rebooting", log_path);
            result = 1;
        }
        mounted_home = false;
    }
    if (mounted_root) {
        if (RUN("/usr/bin/umount", target)) {
            status_message("Cleanup failed; inspect %s before rebooting", log_path);
            result = 1;
        }
        mounted_root = false;
    }
    if (top_mounted) {
        if (RUN("/usr/bin/umount",top)) {
            status_message("Cannot unmount the Btrfs top level; inspect %s",log_path); result=1;
        }
        top_mounted=false;
    }
    if (probe_open && !close_probe(probe,true)) result=1;
    if (root_open) {
        if (RUN("/usr/bin/cryptsetup", "close", "cenere-root")) {
            status_message("Cleanup failed; inspect %s before rebooting", log_path);
            result = 1;
        }
        root_open = false;
    }
    explicit_bzero(plan.password, sizeof(plan.password));
    explicit_bzero(plan.existing_password, sizeof(plan.existing_password));
    close(lock);
    snprintf(stage, sizeof(stage), "%s", result ? "Installation stopped" : "Installation completed");
    if (result)
        report_progress(*problem ? problem : "Inspect the installation log before continuing.");
    if (!result)
        status_message(
            "Installation completed. Remove the ISO/installation medium before booting the installed system.");
    if (*log_path && !progress_callback)
        fprintf(stderr, "Installation log: %s\n", log_path);
    if (log_fd >= 0) {
        fsync(log_fd);
        close(log_fd);
        log_fd = -1;
    }
    return result;
}
