#define _GNU_SOURCE

#include "cenere_install.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--demo | --help]\n"
            "       %s INSTALLABLE_ROOT MOUNTED_TARGET\n"
            "       %s --install DISK\n"
            "No arguments: guided real installation (requires root).\n"
            "--demo: terminal menu simulation with fictional storage; no root required.\n"
            "In a terminal, use arrow keys, Enter, and Esc to move through questions.\n"
            "CENERE_DEFAULT_USERNAME sets the suggested local username.\n"
            "Two paths: copy the prepared system onto an existing mounted target.\n"
            "--install selects the whole disk for the guided offline installation.\n",
            program, program, program);
}

static bool path_is_within(const char *parent, const char *candidate)
{
    size_t length = strlen(parent);

    if (strncmp(parent, candidate, length) != 0)
        return false;

    if (length == 1 && parent[0] == '/')
        return candidate[0] == '/';

    return candidate[length] == '\0' || candidate[length] == '/';
}

static bool decode_mount_path(const char *encoded, char *decoded, size_t size)
{
    size_t input = 0;
    size_t output = 0;

    while (encoded[input] != '\0' && encoded[input] != '\n') {
        unsigned char value = (unsigned char)encoded[input];

        if (encoded[input] == '\\' && isdigit((unsigned char)encoded[input + 1]) &&
            isdigit((unsigned char)encoded[input + 2]) &&
            isdigit((unsigned char)encoded[input + 3])) {
            value = (unsigned char)((encoded[input + 1] - '0') * 64 +
                                    (encoded[input + 2] - '0') * 8 + (encoded[input + 3] - '0'));
            input += 4;
        } else {
            input++;
        }

        if (output + 1 >= size) {
            errno = ENAMETOOLONG;
            return false;
        }
        decoded[output++] = (char)value;
    }

    decoded[output] = '\0';
    return true;
}

static bool mount_path_exists(const char *wanted)
{
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;

    if (mounts == NULL) {
        perror("Cannot read /proc/self/mountinfo");
        return false;
    }

    while (getline(&line, &capacity, mounts) != -1) {
        char *save = NULL;
        char *field = NULL;
        unsigned int index = 0;

        for (field = strtok_r(line, " ", &save); field != NULL;
             field = strtok_r(NULL, " ", &save), index++) {
            if (index == 4) {
                char decoded[PATH_MAX];

                if (decode_mount_path(field, decoded, sizeof(decoded)) &&
                    strcmp(decoded, wanted) == 0)
                    found = true;
                break;
            }
        }

        if (found)
            break;
    }

    free(line);
    fclose(mounts);
    return found;
}

static bool mount_path_is_read_write(const char *wanted)
{
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;
    bool writable = false;

    if (mounts == NULL) {
        perror("Cannot read /proc/self/mountinfo");
        return false;
    }

    while (getline(&line, &capacity, mounts) != -1) {
        char *save = NULL;
        char *field = NULL;
        unsigned int index = 0;
        bool this_mount = false;

        for (field = strtok_r(line, " ", &save); field != NULL;
             field = strtok_r(NULL, " ", &save), index++) {
            if (index == 4) {
                char decoded[PATH_MAX];

                if (!decode_mount_path(field, decoded, sizeof(decoded)))
                    break;
                this_mount = strcmp(decoded, wanted) == 0;
            } else if (index == 5 && this_mount) {
                char *option_save = NULL;
                char *option;

                found = true;
                for (option = strtok_r(field, ",", &option_save); option != NULL;
                     option = strtok_r(NULL, ",", &option_save)) {
                    if (strcmp(option, "rw") == 0) {
                        writable = true;
                        break;
                    }
                }
                break;
            }
        }

        if (found)
            break;
    }

    free(line);
    fclose(mounts);
    return found && writable;
}

static bool has_unsafe_nested_mount(const char *target)
{
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t capacity = 0;
    bool unsafe = false;
    size_t target_length = strlen(target);

    if (mounts == NULL) {
        perror("Cannot read /proc/self/mountinfo");
        return true;
    }

    while (getline(&line, &capacity, mounts) != -1) {
        char *save = NULL;
        char *field = NULL;
        unsigned int index = 0;

        for (field = strtok_r(line, " ", &save); field != NULL;
             field = strtok_r(NULL, " ", &save), index++) {
            if (index == 4) {
                char decoded[PATH_MAX];
                const char *relative;

                if (!decode_mount_path(field, decoded, sizeof(decoded))) {
                    unsafe = true;
                    break;
                }

                if (strncmp(decoded, target, target_length) != 0 || decoded[target_length] != '/')
                    break;

                relative = decoded + target_length;
                if (strncmp(relative, "/home", 5) != 0 ||
                    (relative[5] != '\0' && relative[5] != '/'))
                    unsafe = true;
                break;
            }
        }

        if (unsafe)
            break;
    }

    free(line);
    fclose(mounts);
    return unsafe;
}

static bool is_installable_payload(const char *root)
{
    char path[PATH_MAX];
    FILE *marker;
    FILE *release;
    char line[512];
    bool has_id = false;
    bool has_marker = false;
    int length = snprintf(path, sizeof(path), "%s/etc/cenere/installable", root);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Payload path is too long.\n");
        return false;
    }

    marker = fopen(path, "r");
    if (marker == NULL) {
        fprintf(stderr, "Payload marker is missing: %s\n", path);
        return false;
    }
    if (fgets(line, sizeof(line), marker) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        has_marker = strcmp(line, "cenere-rootfs-v1") == 0;
    }
    fclose(marker);
    if (!has_marker) {
        fprintf(stderr, "Payload marker has an unsupported format.\n");
        return false;
    }

    length = snprintf(path, sizeof(path), "%s/etc/os-release", root);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Payload path is too long.\n");
        return false;
    }

    release = fopen(path, "r");
    if (release == NULL) {
        fprintf(stderr, "Payload is missing /etc/os-release.\n");
        return false;
    }
    while (fgets(line, sizeof(line), release) != NULL) {
        if (strncmp(line, "ID=", 3) == 0) {
            char *value = line + 3;

            value[strcspn(value, "\r\n")] = '\0';
            if (*value == '"' || *value == '\'') {
                char quote = *value++;
                char *end = strchr(value, quote);
                if (end != NULL)
                    *end = '\0';
            }
            has_id = *value && strspn(value,"abcdefghijklmnopqrstuvwxyz0123456789-_")==strlen(value);
            break;
        }
    }
    fclose(release);

    if (!has_id)
        fprintf(stderr, "Payload /etc/os-release has no valid distribution identity.\n");
    return has_id;
}

enum execution_mode { REAL_INSTALLATION, DEMO_INSTALLATION };

enum installation_step {
    BACKUP_HOME,
    SETUP_ENCRYPTION,
    FORMAT_ROOT,
    FORMAT_HOME,
    MOUNT_ROOT,
    RESTORE_HOME,
    TRANSFER_ROOT,
    CONFIGURE_FSTAB,
    CONFIGURE_LOCALIZATION,
    CONFIGURE_NETWORK,
    CONFIGURE_ACCOUNTS,
    GENERATE_INITRAMFS,
    CONFIGURE_BOOT,
    STEP_COUNT
};

static bool complete_installation(const struct installation_plan *plan)
{
    return !plan->copy_only;
}

static bool valid_username(const char *value);

/* Questions and plan construction are shared. Demo inputs stay in memory. */
static void initialize_plan(struct installation_plan *plan, enum execution_mode mode)
{
    *plan = (struct installation_plan){.keymap = "us",
                                       .locale = "en_US.UTF-8",
                                       .timezone = "UTC",
                                       .keymap_group = "us",
                                       .xkb_layout = "us", .xkb_model = "pc105",
                                       .username = "user",
                                       .firmware = "uefi",
                                       .bootloader = "grub"};
    const char *external_name = getenv("CENERE_DEFAULT_USERNAME");
    if (external_name != NULL && strlen(external_name) < sizeof(plan->username) &&
        valid_username(external_name))
        strcpy(plan->username, external_name);
    if (mode == DEMO_INSTALLATION) {
        strcpy(plan->image_id, "demo");
        strcpy(plan->source, "/demo/system-root");
        strcpy(plan->target, "/demo/target");
        plan->existing_home = true;
        plan->preserve_home = true;
    } else {
        strcpy(plan->bootloader, "grub");
        if (access("/sys/firmware/efi", F_OK) != 0) {
            strcpy(plan->firmware, "bios");
            strcpy(plan->bootloader, "grub");
        }
    }
}

static size_t trim_line_ending(char *line, size_t length)
{
    if (length > 0 && line[length - 1] == '\n')
        length--;
    if (length > 0 && line[length - 1] == '\r')
        length--;
    line[length] = '\0';
    return length;
}

static bool printable_text(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == 127)
            return false;
    }
    return *value != '\0';
}

static bool configuration_name(const char *value)
{
    if (*value == '\0' || *value == '/' || strstr(value, "..") != NULL)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isalnum(*p) && strchr("_-./@", *p) == NULL)
            return false;
    }
    return true;
}

static bool valid_hostname(const char *value)
{
    size_t length = strlen(value);
    if (length == 0 || value[0] == '-' || value[length - 1] == '-')
        return false;
    for (const char *p = value; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '-')
            return false;
    }
    return true;
}

static void suggest_hostname(struct installation_plan *plan)
{
    /* Seed once from the prepared distribution, retaining edits when going back. */
    if (*plan->hostname)
        return;
    size_t length = 0;
    for (const char *p = plan->image_id; *p && length + 1 < sizeof(plan->hostname); p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
            plan->hostname[length++] = *p;
        else if (length && (*p == '-' || *p == '_'))
            plan->hostname[length++] = '-';
    }
    while (length && plan->hostname[length - 1] == '-')
        length--;
    plan->hostname[length] = '\0';
    if (!length)
        strcpy(plan->hostname, "linux");
}

static bool valid_username(const char *value)
{
    if (!((*value >= 'a' && *value <= 'z') || *value == '_') || strcmp(value, "root") == 0)
        return false;
    for (const char *p = value; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-')
            return false;
    }
    return true;
}

static bool choice_matches(const char *choices, const char *value)
{
    size_t length = strlen(value);
    while (*choices) {
        const char *end = strchr(choices, '/');
        size_t count = end ? (size_t)(end - choices) : strlen(choices);
        if (count == length && strncmp(choices, value, count) == 0)
            return true;
        if (end == NULL)
            break;
        choices = end + 1;
    }
    return false;
}

static bool ask_question(const char *label, char *value, size_t size, const char *choices,
                         bool (*validator)(const char *))
{
    char *answer = NULL;
    size_t capacity = 0;
    bool accepted = false;

    while (!accepted) {
        fprintf(stderr, "%s", label);
        if (choices != NULL)
            fprintf(stderr, " (%s)", choices);
        if (*value != '\0')
            fprintf(stderr, " [%s]", value);
        fprintf(stderr, ": ");
        fflush(stderr);
        ssize_t length = getline(&answer, &capacity, stdin);
        if (length < 0) {
            fprintf(stderr, "No input received. Installation cancelled.\n");
            break;
        }
        if (memchr(answer, '\0', (size_t)length) != NULL) {
            fprintf(stderr, "Input must not contain NUL characters.\n");
            continue;
        }
        size_t count = trim_line_ending(answer, (size_t)length);
        if (strcmp(answer, "cancel") == 0) {
            fprintf(stderr, "Installation cancelled.\n");
            break;
        }
        const char *candidate = count == 0 ? value : answer;
        if (strlen(candidate) >= size || !printable_text(candidate) ||
            (choices != NULL && !choice_matches(choices, candidate)) ||
            (validator != NULL && !validator(candidate))) {
            fprintf(stderr, "Invalid value; enter up to %zu characters", size - 1);
            if (choices != NULL)
                fprintf(stderr, " and choose %s", choices);
            fprintf(stderr, ".\n");
            continue;
        }
        if (count != 0)
            memcpy(value, answer, count + 1);
        accepted = true;
    }
    free(answer);
    return accepted;
}

static struct termios password_terminal;
static volatile sig_atomic_t password_echo_hidden;

static void interrupt_password(int number)
{
    if (password_echo_hidden)
        tcsetattr(STDIN_FILENO, TCSANOW, &password_terminal);
    /* Cancel on job suspension too, so the shell never inherits hidden input. */
    ssize_t written = write(STDERR_FILENO, "\n", 1);
    (void)written; /* Terminal output is best effort while exiting a signal handler. */
    _exit(128 + number);
}

/* Fixed buffers and unbuffered stdin keep credentials out of getline buffers. */
static bool read_password(const char *label, char *value, size_t size)
{
    const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP};
    struct sigaction previous[sizeof(signals) / sizeof(signals[0])];
    struct sigaction action = {.sa_handler = interrupt_password};
    size_t handlers = 0;
    bool terminal = isatty(STDIN_FILENO);
    bool success = false;

    explicit_bzero(value, size);
    if (terminal) {
        if (tcgetattr(STDIN_FILENO, &password_terminal) != 0) {
            perror("Cannot read terminal settings");
            return false;
        }
        sigemptyset(&action.sa_mask);
        for (; handlers < sizeof(signals) / sizeof(signals[0]); handlers++) {
            if (sigaction(signals[handlers], &action, &previous[handlers]) != 0) {
                perror("Cannot protect password input");
                goto restore;
            }
        }
        struct termios hidden = password_terminal;
        hidden.c_lflag &= ~(ECHO | ECHONL);
        password_echo_hidden = 1;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
            perror("Cannot hide password input");
            goto restore;
        }
    }
    fprintf(stderr, "%s: ", label);
    fflush(stderr);
    size_t length = 0;
    bool invalid = false;
    int byte;
    while ((byte = fgetc(stdin)) != EOF && byte != '\n') {
        if (byte == '\0' || length + 1 >= size)
            invalid = true;
        else
            value[length++] = (char)byte;
    }
    if (terminal)
        fprintf(stderr, "\n");
    if (byte == EOF) {
        fprintf(stderr, "No password received. Installation cancelled.\n");
    } else if (invalid) {
        fprintf(stderr, "Password input is invalid or too long. Installation cancelled.\n");
    } else {
        trim_line_ending(value, length);
        success = true;
    }

restore:
    if (password_echo_hidden) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &password_terminal) != 0) {
            perror("Cannot restore terminal settings");
            success = false;
        }
        password_echo_hidden = 0;
    }
    while (handlers > 0) {
        handlers--;
        if (sigaction(signals[handlers], &previous[handlers], NULL) != 0)
            success = false;
    }
    if (!success)
        explicit_bzero(value, size);
    return success;
}

static bool ask_secret(char *stored, size_t size, const char *label, const char *confirmation_label)
{
    char password[256] = {0};
    char confirmation[256] = {0};
    char first_prompt[128];
    bool accepted = false;
    if (size > sizeof(password))
        return false;

    for (;;) {
        snprintf(first_prompt, sizeof(first_prompt), "%s%s", label,
                 *stored ? " (Enter to keep current)" : "");
        if (!read_password(first_prompt, password, sizeof(password)))
            break;
        if (strcmp(password, "cancel") == 0) {
            fprintf(stderr, "Installation cancelled.\n");
            break;
        }
        if (*password == '\0') {
            if (*stored) {
                accepted = true;
                break;
            }
            fprintf(stderr, "Enter a value for %s.\n", label);
            continue;
        }
        if (!read_password(confirmation_label, confirmation, sizeof(confirmation)))
            break;
        if (strcmp(confirmation, "cancel") == 0) {
            fprintf(stderr, "Installation cancelled.\n");
            break;
        }
        if (strcmp(password, confirmation) == 0) {
            explicit_bzero(stored, size);
            memcpy(stored, password, strlen(password) + 1);
            accepted = true;
            break;
        }
        fprintf(stderr, "%s do not match. Please try again.\n",
                strcmp(label, "User password") == 0 ? "Passwords" : "Passphrases");
        explicit_bzero(password, sizeof(password));
        explicit_bzero(confirmation, sizeof(confirmation));
    }
    explicit_bzero(password, sizeof(password));
    explicit_bzero(confirmation, sizeof(confirmation));
    return accepted;
}

enum ui_page {
    UI_KEYMAP,
    UI_DESTINATION, UI_UNLOCK, UI_UNLOCK_PASSWORD, UI_OTHER_SYSTEMS,
    UI_PRESERVE_HOME, UI_ENCRYPTION,
    UI_LOCALE, UI_TIMEZONE, UI_HOSTNAME, UI_USERNAME,
    UI_PASSWORD, UI_PASSWORD_CONFIRM, UI_REVIEW, UI_PAGE_COUNT
};

enum ui_result { UI_NEXT, UI_BACK, UI_CANCEL };

struct ui_option {
    const char *label;
    const char *value;
};

static const struct ui_option demo_disks[] = {
    {"Demo disk A - 64 GiB", "/demo/target"},
    {"Demo disk B - 128 GiB", "/demo/target-b"},
};

/* Locale fallback for demo use; installed glibc data takes precedence. */
/* Source: https://github.com/bminor/glibc */
static const char bundled_locale_codes[] =
    "C.UTF-8 aa_DJ.UTF-8 af_ZA.UTF-8 an_ES.UTF-8 ar_AE.UTF-8 ar_BH.UTF-8 ar_DZ.UTF-8 "
    "ar_EG.UTF-8 ar_IQ.UTF-8 ar_JO.UTF-8 ar_KW.UTF-8 ar_LB.UTF-8 ar_LY.UTF-8 ar_MA.UTF-8 "
    "ar_OM.UTF-8 ar_QA.UTF-8 ar_SA.UTF-8 ar_SD.UTF-8 ar_SY.UTF-8 ar_TN.UTF-8 ar_YE.UTF-8 "
    "ast_ES.UTF-8 be_BY.UTF-8 bg_BG.UTF-8 bhb_IN.UTF-8 br_FR.UTF-8 bs_BA.UTF-8 ca_AD.UTF-8 "
    "ca_ES.UTF-8 ca_FR.UTF-8 ca_IT.UTF-8 cs_CZ.UTF-8 cy_GB.UTF-8 da_DK.UTF-8 de_AT.UTF-8 "
    "de_BE.UTF-8 de_CH.UTF-8 de_DE.UTF-8 de_IT.UTF-8 de_LI.UTF-8 de_LU.UTF-8 el_CY.UTF-8 "
    "el_GR.UTF-8 en_AU.UTF-8 en_BW.UTF-8 en_CA.UTF-8 en_DK.UTF-8 en_GB.UTF-8 en_HK.UTF-8 "
    "en_IE.UTF-8 en_NZ.UTF-8 en_PH.UTF-8 en_SC.UTF-8 en_SE.UTF-8 en_SG.UTF-8 en_US.UTF-8 "
    "en_ZA.UTF-8 en_ZW.UTF-8 es_AR.UTF-8 es_BO.UTF-8 es_CL.UTF-8 es_CO.UTF-8 es_CR.UTF-8 "
    "es_DO.UTF-8 es_EC.UTF-8 es_ES.UTF-8 es_GT.UTF-8 es_HN.UTF-8 es_MX.UTF-8 es_NI.UTF-8 "
    "es_PA.UTF-8 es_PE.UTF-8 es_PR.UTF-8 es_PY.UTF-8 es_SV.UTF-8 es_US.UTF-8 es_UY.UTF-8 "
    "es_VE.UTF-8 et_EE.ISO-8859-15 et_EE.UTF-8 eu_ES.UTF-8 fi_FI.UTF-8 fo_FO.UTF-8 fr_BE.UTF-8 "
    "fr_CA.UTF-8 fr_CH.UTF-8 fr_FR.UTF-8 fr_LU.UTF-8 ga_IE.UTF-8 gd_GB.UTF-8 gl_ES.UTF-8 "
    "gv_GB.UTF-8 he_IL.UTF-8 hr_HR.UTF-8 hsb_DE.UTF-8 hu_HU.UTF-8 hy_AM.ARMSCII-8 id_ID.UTF-8 "
    "is_IS.UTF-8 it_CH.UTF-8 it_IT.UTF-8 ja_JP.EUC-JP ja_JP.UTF-8 ka_GE.UTF-8 kk_KZ.UTF-8 "
    "kl_GL.UTF-8 ko_KR.EUC-KR ko_KR.UTF-8 ku_TR.UTF-8 kw_GB.UTF-8 lg_UG.UTF-8 lt_LT.UTF-8 "
    "ltg_LV.UTF-8 lv_LV.UTF-8 mg_MG.UTF-8 mi_NZ.UTF-8 mk_MK.UTF-8 ms_MY.UTF-8 mt_MT.UTF-8 "
    "nb_NO.UTF-8 nl_BE.UTF-8 nl_NL.UTF-8 nn_NO.UTF-8 oc_FR.UTF-8 om_KE.UTF-8 pl_PL.UTF-8 "
    "pt_BR.UTF-8 pt_PT.UTF-8 ro_RO.UTF-8 ru_RU.KOI8-R ru_RU.UTF-8 ru_UA.UTF-8 sk_SK.UTF-8 "
    "sl_SI.UTF-8 so_DJ.UTF-8 so_KE.UTF-8 so_SO.UTF-8 sq_AL.UTF-8 st_ZA.UTF-8 sv_FI.UTF-8 "
    "sv_SE.UTF-8 tcy_IN.UTF-8 tg_TJ.UTF-8 th_TH.UTF-8 tl_PH.UTF-8 tr_CY.UTF-8 tr_TR.UTF-8 "
    "uk_UA.UTF-8 uz_UZ.UTF-8 wa_BE.UTF-8 xh_ZA.UTF-8 yi_US.UTF-8 zh_CN.GB18030 zh_CN.GBK "
    "zh_CN.UTF-8 zh_HK.UTF-8 zh_SG.GBK zh_SG.UTF-8 zh_TW.EUC-TW zh_TW.UTF-8 zu_ZA.UTF-8";

static const struct ui_option preserve_options[] = {
    {"Yes, keep the existing home files", "yes"},
    {"No, replace the existing home files", "no"},
};

static const struct ui_option encryption_options[] = {
    {"No encryption", "no"},
    {"Yes, encrypt system and home", "yes"},
};

static const struct ui_option locales[] = {
    {"English (US)", "en_US.UTF-8"}, {"English (UK)", "en_GB.UTF-8"}, {"Italian", "it_IT.UTF-8"},
    {"German", "de_DE.UTF-8"},       {"French", "fr_FR.UTF-8"},       {"Spanish", "es_ES.UTF-8"},
};

/* Language names from Debian iso-codes ISO 639-3 data. */
struct ui_name {
    const char *code;
    const char *name;
};
static const struct ui_name language_names[] = {
    {"C", "System default"},
    {"aa", "Afar"},
    {"af", "Afrikaans"},
    {"an", "Aragonese"},
    {"ar", "Arabic"},
    {"ast", "Asturian"},
    {"be", "Belarusian"},
    {"bg", "Bulgarian"},
    {"bhb", "Bhili"},
    {"br", "Breton"},
    {"bs", "Bosnian"},
    {"ca", "Catalan"},
    {"cs", "Czech"},
    {"cy", "Welsh"},
    {"da", "Danish"},
    {"de", "German"},
    {"el", "Modern Greek (1453-)"},
    {"en", "English"},
    {"es", "Spanish"},
    {"et", "Estonian"},
    {"eu", "Basque"},
    {"fi", "Finnish"},
    {"fo", "Faroese"},
    {"fr", "French"},
    {"ga", "Irish"},
    {"gd", "Scottish Gaelic"},
    {"gl", "Galician"},
    {"gv", "Manx"},
    {"he", "Hebrew"},
    {"hr", "Croatian"},
    {"hsb", "Upper Sorbian"},
    {"hu", "Hungarian"},
    {"hy", "Armenian"},
    {"id", "Indonesian"},
    {"is", "Icelandic"},
    {"it", "Italian"},
    {"ja", "Japanese"},
    {"ka", "Georgian"},
    {"kk", "Kazakh"},
    {"kl", "Kalaallisut"},
    {"ko", "Korean"},
    {"ku", "Kurdish"},
    {"kw", "Cornish"},
    {"lg", "Ganda"},
    {"lt", "Lithuanian"},
    {"ltg", "Latgalian"},
    {"lv", "Latvian"},
    {"mg", "Malagasy"},
    {"mi", "Maori"},
    {"mk", "Macedonian"},
    {"ms", "Malay (macrolanguage)"},
    {"mt", "Maltese"},
    {"nb", "Norwegian Bokmål"},
    {"nl", "Dutch"},
    {"nn", "Norwegian Nynorsk"},
    {"oc", "Occitan (post 1500)"},
    {"om", "Oromo"},
    {"pl", "Polish"},
    {"pt", "Portuguese"},
    {"ro", "Romanian"},
    {"ru", "Russian"},
    {"sk", "Slovak"},
    {"sl", "Slovenian"},
    {"so", "Somali"},
    {"sq", "Albanian"},
    {"st", "Southern Sotho"},
    {"sv", "Swedish"},
    {"tcy", "Tulu"},
    {"tg", "Tajik"},
    {"th", "Thai"},
    {"tl", "Tagalog"},
    {"tr", "Turkish"},
    {"uk", "Ukrainian"},
    {"uz", "Uzbek"},
    {"wa", "Walloon"},
    {"xh", "Xhosa"},
    {"yi", "Yiddish"},
    {"zh", "Chinese"},
    {"zu", "Zulu"},
};

static const struct ui_option timezones[] = {
    {"UTC", "UTC"},
    {"Rome", "Europe/Rome"},
    {"London", "Europe/London"},
    {"Berlin", "Europe/Berlin"},
    {"New York", "America/New_York"},
    {"Los Angeles", "America/Los_Angeles"},
    {"Tokyo", "Asia/Tokyo"},
};

#define UI_OPTIONS(array) (sizeof(array) / sizeof((array)[0]))

struct ui_catalog {
    struct ui_option *options;
    size_t count;
    size_t capacity;
    bool failed;
};

static void ui_catalog_free(struct ui_catalog *catalog)
{
    for (size_t i = 0; i < catalog->count; i++) {
        free((char *)catalog->options[i].label);
        free((char *)catalog->options[i].value);
    }
    free(catalog->options);
    *catalog = (struct ui_catalog){0};
}

static bool ui_catalog_add(struct ui_catalog *catalog, const char *label, const char *value)
{
    for (size_t i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->options[i].value, value) == 0)
            return true;
    }
    if (catalog->count == catalog->capacity) {
        size_t next = catalog->capacity ? catalog->capacity * 2 : 64;
        struct ui_option *larger = realloc(catalog->options, next * sizeof(*larger));
        if (larger == NULL) {
            catalog->failed = true;
            return false;
        }
        catalog->options = larger;
        catalog->capacity = next;
    }
    char *name = strdup(label);
    char *code = strdup(value);
    if (name == NULL || code == NULL) {
        catalog->failed = true;
        free(name);
        free(code);
        return false;
    }
    catalog->options[catalog->count++] = (struct ui_option){.label = name, .value = code};
    return true;
}

static void ui_catalog_add_locale_file(struct ui_catalog *catalog, const char *path)
{
    FILE *input = fopen(path, "r");
    if (input == NULL)
        return;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, input) != -1) {
        char *cursor = line;
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (*cursor == '#')
            cursor++;
        while (isspace((unsigned char)*cursor))
            cursor++;
        char *end = cursor;
        while (*end && !isspace((unsigned char)*end))
            end++;
        *end = '\0';
        char *encoding = strchr(cursor, '/');
        if (encoding != NULL)
            *encoding = '\0';
        if (strchr(cursor, '.') && strlen(cursor) < 64 && configuration_name(cursor))
            ui_catalog_add(catalog, cursor, cursor);
    }
    free(line);
    fclose(input);
}

static void ui_catalog_add_codes(struct ui_catalog *catalog, const char *codes)
{
    char *list = strdup(codes);
    if (list == NULL) {
        catalog->failed = true;
        return;
    }
    char *save = NULL;
    for (char *code = strtok_r(list, " ", &save); code != NULL; code = strtok_r(NULL, " ", &save))
        ui_catalog_add(catalog, code, code);
    free(list);
}

static void ui_catalog_add_timezone_file(struct ui_catalog *catalog, const char *path)
{
    FILE *input = fopen(path, "r");
    if (input == NULL)
        return;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, input) != -1) {
        if (line[0] == '#')
            continue;
        char *save = NULL;
        strtok_r(line, "\t", &save);
        strtok_r(NULL, "\t", &save);
        char *zone = strtok_r(NULL, "\t\r\n", &save);
        if (zone && strlen(zone) < 96 && configuration_name(zone))
            ui_catalog_add(catalog, zone, zone);
    }
    free(line);
    fclose(input);
}

static int ui_compare_options(const void *left, const void *right)
{
    const struct ui_option *a = left, *b = right;
    return strcasecmp(a->label, b->label);
}

static const char *ui_language_name(const char *code)
{
    for (size_t i = 0; i < UI_OPTIONS(language_names); i++) {
        if (strcmp(language_names[i].code, code) == 0)
            return language_names[i].name;
    }
    return code;
}

struct ui_country {
    char code[4];
    char name[128];
};

static const char *ui_country_name(const char *code)
{
    static struct ui_country countries[256];
    static size_t count;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        FILE *input = fopen("/usr/share/zoneinfo/iso3166.tab", "r");
        if (input != NULL) {
            char line[256];
            while (count < UI_OPTIONS(countries) && fgets(line, sizeof(line), input)) {
                if (line[0] == '#')
                    continue;
                char *separator = strchr(line, '\t');
                if (separator == NULL || (size_t)(separator - line) != 2)
                    continue;
                *separator++ = '\0';
                separator[strcspn(separator, "\r\n")] = '\0';
                strcpy(countries[count].code, line);
                snprintf(countries[count].name, sizeof(countries[count].name), "%s", separator);
                count++;
            }
            fclose(input);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(countries[i].code, code) == 0)
            return countries[i].name;
    }
    return code;
}

static void ui_locale_label(const char *code, char *label, size_t size)
{
    char language[8] = "", country[4] = "";
    size_t length = strcspn(code, "_.@");
    if (length == 0 || length >= sizeof(language)) {
        snprintf(label, size, "%s", code);
        return;
    }
    memcpy(language, code, length);
    language[length] = '\0';
    const char *region = strchr(code, '_');
    if (region != NULL && strlen(region) >= 3) {
        country[0] = region[1];
        country[1] = region[2];
    }
    if (*country)
        snprintf(label, size, "%s - %s (%s)", ui_language_name(language), ui_country_name(country),
                 code);
    else
        snprintf(label, size, "%s (%s)", ui_language_name(language), code);
}

static void ui_timezone_label(const char *code, char *label, size_t size)
{
    const char *slash = strchr(code, '/');
    if (slash == NULL) {
        snprintf(label, size, "%s (%s)",
                 strcmp(code, "UTC") == 0 ? "Coordinated Universal Time" : code, code);
        return;
    }
    char place[128];
    snprintf(place, sizeof(place), "%s", slash + 1);
    for (char *p = place; *p; p++) {
        if (*p == '_')
            *p = ' ';
    }
    snprintf(label, size, "%.*s - %s (%s)", (int)(slash - code), code, place, code);
}

static struct ui_catalog ui_load_catalog(enum ui_page page)
{
    struct ui_catalog catalog = {0};
    const struct ui_option *defaults = NULL;
    size_t fallback_count = 0;
    const char *path = NULL;
    if (page == UI_LOCALE) {
        defaults = locales;
        fallback_count = UI_OPTIONS(locales);
        path = getenv("CENERE_LOCALE_FILE");
        ui_catalog_add_locale_file(&catalog, path ? path : "/etc/locale.gen");
        if (catalog.count == 0 && path == NULL)
            ui_catalog_add_locale_file(&catalog, "/usr/share/i18n/SUPPORTED");
    } else if (page == UI_TIMEZONE) {
        defaults = timezones;
        fallback_count = UI_OPTIONS(timezones);
        path = getenv("CENERE_TIMEZONE_FILE");
        ui_catalog_add_timezone_file(&catalog, path ? path : "/usr/share/zoneinfo/zone.tab");
        if (catalog.count != 0)
            ui_catalog_add(&catalog, "UTC", "UTC");
    }
    if (catalog.count == 0) {
        if (page == UI_LOCALE)
            ui_catalog_add_codes(&catalog, bundled_locale_codes);
        else
            for (size_t i = 0; i < fallback_count; i++)
                ui_catalog_add(&catalog, defaults[i].label, defaults[i].value);
    }
    {
        for (size_t i = 0; i < catalog.count; i++) {
            for (size_t j = 0; j < fallback_count; j++) {
                if (strcmp(catalog.options[i].value, defaults[j].value) == 0) {
                    char *name = strdup(defaults[j].label);
                    if (name != NULL) {
                        free((char *)catalog.options[i].label);
                        catalog.options[i].label = name;
                    } else
                        catalog.failed = true;
                    break;
                }
            }
        }
    }
    if (catalog.failed) {
        ui_catalog_free(&catalog);
        return catalog;
    }
    if (page == UI_LOCALE || page == UI_TIMEZONE) {
        for (size_t i = 0; i < catalog.count; i++) {
            char label[256];
            if (page == UI_LOCALE)
                ui_locale_label(catalog.options[i].value, label, sizeof(label));
            else
                ui_timezone_label(catalog.options[i].value, label, sizeof(label));
            char *name = strdup(label);
            if (name == NULL) {
                ui_catalog_free(&catalog);
                return catalog;
            }
            free((char *)catalog.options[i].label);
            catalog.options[i].label = name;
        }
    }
    qsort(catalog.options, catalog.count, sizeof(*catalog.options), ui_compare_options);
    return catalog;
}

static struct termios ui_saved_terminal;
static volatile sig_atomic_t ui_terminal_active;
static const struct installation_plan *ui_active_plan;
static const int ui_signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP};
static struct sigaction ui_old_actions[sizeof(ui_signals) / sizeof(ui_signals[0])];
static size_t ui_handler_count;

static void ui_interrupt(int number)
{
    if (ui_terminal_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &ui_saved_terminal);
        ssize_t written = write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
        (void)written;
    }
    _exit(128 + number);
}

static bool ui_begin(void)
{
    struct sigaction action = {.sa_handler = ui_interrupt};
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &ui_saved_terminal) != 0)
        return false;
    sigemptyset(&action.sa_mask);
    for (ui_handler_count = 0; ui_handler_count < UI_OPTIONS(ui_signals); ui_handler_count++) {
        if (sigaction(ui_signals[ui_handler_count], &action, &ui_old_actions[ui_handler_count]) !=
            0)
            goto failed;
    }
    raw = ui_saved_terminal;
    cfmakeraw(&raw);
    ui_terminal_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        goto failed;
    fputs("\033[?1049h\033[2J\033[H", stdout);
    fflush(stdout);
    return true;

failed:
    if (ui_terminal_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &ui_saved_terminal);
    ui_terminal_active = 0;
    while (ui_handler_count > 0) {
        ui_handler_count--;
        sigaction(ui_signals[ui_handler_count], &ui_old_actions[ui_handler_count], NULL);
    }
    return false;
}

static void ui_end(void)
{
    if (ui_terminal_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &ui_saved_terminal);
        ui_terminal_active = 0;
    }
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    while (ui_handler_count > 0) {
        ui_handler_count--;
        sigaction(ui_signals[ui_handler_count], &ui_old_actions[ui_handler_count], NULL);
    }
}

enum ui_key {
    UI_KEY_UP = 1000,
    UI_KEY_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
    UI_KEY_PAGE_UP,
    UI_KEY_PAGE_DOWN,
    UI_KEY_HOME,
    UI_KEY_END,
    UI_KEY_REDRAW,
    UI_KEY_EOF
};

static int ui_byte(void)
{
    unsigned char value;
    ssize_t count = read(STDIN_FILENO, &value, 1);
    if (count == 1)
        return value;
    return count == 0 ? UI_KEY_EOF : UI_KEY_REDRAW;
}

static int ui_key(void)
{
    int first = ui_byte();
    if (first != 27)
        return first;
    struct pollfd descriptor = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&descriptor, 1, 35) <= 0)
        return 27;
    int second = ui_byte();
    if (second != '[' && second != 'O')
        return 27;
    if (poll(&descriptor, 1, 35) <= 0)
        return UI_KEY_REDRAW;
    int third = ui_byte();
    switch (third) {
    case 'A':
        return UI_KEY_UP;
    case 'B':
        return UI_KEY_DOWN;
    case 'C':
        return UI_KEY_RIGHT;
    case 'D':
        return UI_KEY_LEFT;
    case 'H':
        return UI_KEY_HOME;
    case 'F':
        return UI_KEY_END;
    case '5':
    case '6':
        if (poll(&descriptor, 1, 35) > 0 && ui_byte() == '~')
            return third == '5' ? UI_KEY_PAGE_UP : UI_KEY_PAGE_DOWN;
        break;
    }
    return UI_KEY_REDRAW;
}

static size_t ui_columns(void)
{
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0)
        return 80;
    return size.ws_col;
}

static size_t ui_lines(void)
{
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row == 0)
        return 24;
    return size.ws_row;
}

static size_t ui_panel_width(void)
{
    size_t columns = ui_columns();
    return columns > 76 ? 72 : columns > 6 ? columns - 4 : columns;
}

static size_t ui_panel_left(void)
{
    return (ui_columns() - ui_panel_width()) / 2 + 1;
}

static void ui_center(size_t row, const char *message)
{
    size_t columns = ui_columns();
    size_t length = strlen(message);
    size_t start = length < columns ? (columns - length) / 2 + 1 : 1;
    printf("\033[%zu;%zuH%.*s", row, start, (int)columns, message);
}

static void ui_row(size_t row, const char *message)
{
    printf("\033[%zu;%zuH%.*s", row, ui_panel_left(), (int)ui_panel_width(), message);
}

static size_t ui_visible_rows(void)
{
    size_t lines = ui_lines();
    return lines > 16 ? lines - 13 : 2;
}

static bool ui_page_visible(enum ui_page page, enum execution_mode mode)
{
    if (page == UI_UNLOCK || page == UI_UNLOCK_PASSWORD)
        return mode == REAL_INSTALLATION && (ui_active_plan->locked_storage ||
               *ui_active_plan->existing_password) &&
               (page == UI_UNLOCK || !ui_active_plan->discard_encrypted);
    if (page == UI_OTHER_SYSTEMS)
        return mode == REAL_INSTALLATION && ui_active_plan->existing_os && !ui_active_plan->discard_encrypted;
    if (page == UI_PRESERVE_HOME)
        return ui_active_plan->existing_home && !ui_active_plan->discard_encrypted;
    return true;
}

static void ui_header(const char *title, enum ui_page page, enum execution_mode mode, bool cursor)
{
    const char *section = page == UI_KEYMAP ? "Keyboard preparation"
                        : page <= UI_ENCRYPTION ? "Disk preparation"
                        : page <= UI_TIMEZONE ? "1 of 4: Preferences"
                        : page < UI_REVIEW ? "2 of 4: Account"
                        : page == UI_REVIEW ? "3 of 4: Summary" : "4 of 4: Install";
    unsigned active=page==UI_KEYMAP ? 0 : page<=UI_ENCRYPTION ? 1 :
                    page<=UI_TIMEZONE ? 2 : page<UI_REVIEW ? 3 : page==UI_REVIEW ? 4 : 5;
    const char *phases[]={"Keyboard","Disk","Preferences","Account","Summary","Install"};
    char heading[128],path[128]="";
    snprintf(heading,sizeof(heading),"Cenere | %s | %s",mode==DEMO_INSTALLATION?"Demo":"Real",section);
    for (unsigned i=0;i<6;i++) {
        char part[32];
        snprintf(part,sizeof(part),"%s%s%s%s",i?" > ":"",i==active?"[":"",phases[i],i==active?"]":"");
        strncat(path,part,sizeof(path)-strlen(path)-1);
    }
    printf("\033[2J\033[H\033[?25%c",cursor?'h':'l');
    ui_center(1,heading);
    ui_center(2,path);
    ui_center(4,title);
}

static const char *ui_label(const struct ui_option *options, size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(options[i].value, value) == 0)
            return options[i].label;
    }
    return value;
}

static size_t ui_input(size_t row, const char *value, bool secret, const char *placeholder)
{
    size_t width = ui_panel_width(), visible = width > 2 ? width - 2 : 0;
    size_t length = strlen(value), start = length > visible ? length - visible : 0;
    char field[73] = "";
    if (width > 2) {
        field[0] = ' ';
        if (!length)
            snprintf(field + 1, sizeof(field) - 1, "%.*s", (int)visible, placeholder);
        else if (secret)
            memset(field + 1, '*', length - start);
        else
            memcpy(field + 1, value + start, length - start);
    }
    printf("\033[%zu;%zuH\033[7m%-*.*s\033[0m", row, ui_panel_left(),
           (int)width, (int)width, field);
    explicit_bzero(field, sizeof(field));
    return ui_panel_left() + (width > 2 ? 1 + length - start : 0);
}

static enum ui_result ui_text(const char *title, char *value, size_t size, bool secret,
                              bool keep_secret, bool (*validator)(const char *), enum ui_page page,
                              enum execution_mode mode, const char *error)
{
    char *draft = calloc(size, 1);
    if (draft == NULL)
        return UI_CANCEL;
    if (!secret)
        strcpy(draft, value);
    const char *problem = error;
    bool overflow = false;
    bool replace_default = !secret && *value != '\0';
    enum ui_result result = UI_CANCEL;
    for (;;) {
        ui_header(title, page, mode, true);
        if (secret) {
            ui_row(7, *value && keep_secret ? "Enter keeps your saved password. Typing replaces it."
                                            : "Your password is hidden as you type.");
        }
        size_t cursor = ui_input(9, draft, secret, secret ? "Type password..." : "Type here...");
        if (problem)
            ui_row(12, problem);
        ui_center(ui_lines() > 2 ? ui_lines() - 2 : 1, "Enter: continue   Esc: back");
        printf("\033[9;%zuH", cursor);
        fflush(stdout);
        int key = ui_key();
        if (key == 3 || key == UI_KEY_EOF) {
            result = UI_CANCEL;
            break;
        }
        if (key == 27 || key == UI_KEY_LEFT) {
            result = UI_BACK;
            break;
        }
        if (key == '\r' || key == '\n') {
            const char *candidate = *draft ? draft : value;
            if (secret && !keep_secret)
                candidate = draft;
            if (overflow || !printable_text(candidate) || strlen(candidate) >= size ||
                (validator != NULL && !validator(candidate))) {
                problem = "Enter a valid value to continue.";
                continue;
            }
            if (*draft && (secret || strcmp(draft, value) != 0)) {
                if (secret)
                    explicit_bzero(value, size);
                strcpy(value, draft);
            }
            result = UI_NEXT;
            break;
        }
        if (key == 127 || key == 8) {
            replace_default = false;
            size_t length = strlen(draft);
            if (length > 0)
                draft[length - 1] = '\0';
            overflow = false;
            problem = NULL;
        } else if (key == 21) {
            replace_default = false;
            explicit_bzero(draft, size);
            overflow = false;
            problem = NULL;
        } else if (key >= 32 && key <= 255 && key != 127) {
            if (replace_default) {
                explicit_bzero(draft, size);
                replace_default = false;
            }
            size_t length = strlen(draft);
            if (length + 1 < size) {
                draft[length] = (char)key;
                draft[length + 1] = '\0';
                problem = NULL;
            } else
                overflow = true;
        }
    }
    if (secret)
        explicit_bzero(draft, size);
    free(draft);
    return result;
}

static enum ui_result ui_menu(const char *title, const struct ui_option *options, size_t count,
                              char *value, size_t size, enum ui_page page, enum execution_mode mode)
{
    size_t *matches = calloc(count ? count : 1, sizeof(*matches));
    char search[80] = "";
    bool searchable = page != UI_PAGE_COUNT;
    size_t selected = 0;
    if (matches == NULL)
        return UI_CANCEL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(options[i].value, value) == 0)
            selected = i;
    }
    enum ui_result result = UI_CANCEL;
    for (;;) {
        size_t found = 0;
        bool selected_found = false;
        size_t selected_position = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcasestr(options[i].label, search) == NULL &&
                strcasestr(options[i].value, search) == NULL)
                continue;
            matches[found] = i;
            if (i == selected) {
                selected_found = true;
                selected_position = found;
            }
            found++;
        }
        if (found && !selected_found) {
            selected = matches[0];
            selected_position = 0;
        }
        ui_header(title, page, mode, searchable);
        if (page == UI_PAGE_COUNT)
            ui_row(5, "Remove the installation medium before the next boot.");
        if (searchable) {
            char filter[120];
            snprintf(filter, sizeof(filter), "[ %s ]", *search ? search : "Type to filter...");
            ui_row(6, filter);
        }
        if (found == 0)
            ui_row(8, "No matching choices.");
        else {
            size_t rows = ui_visible_rows();
            size_t first = selected_position >= rows ? selected_position - rows + 1 : 0;
            int width = (int)ui_panel_width();
            for (size_t i = first; i < found && i < first + rows; i++) {
                bool active = matches[i] == selected;
                printf("\033[%zu;%zuH%s%-*.*s%s", 8 + i - first, ui_panel_left(),
                       active ? "\033[7m" : "", width, width, options[matches[i]].label,
                       active ? "\033[0m" : "");
            }
            if (found > rows) {
                char position[64];
                snprintf(position, sizeof(position), "%zu / %zu", selected_position + 1, found);
                ui_center(ui_lines() > 4 ? ui_lines() - 4 : 1, position);
            }
        }
        ui_center(ui_lines() > 2 ? ui_lines() - 2 : 1,
                  page == UI_PAGE_COUNT ? "Enter: select   Esc: stay in the live environment"
                                        : "Enter: select   Esc: back");
        if (searchable)
            printf("\033[6;%zuH", ui_panel_left() + 2 + strlen(search));
        fflush(stdout);
        int key = ui_key();
        if (key == 3 || key == UI_KEY_EOF) {
            result = UI_CANCEL;
            break;
        }
        if (key == 27 || key == UI_KEY_LEFT) {
            result = UI_BACK;
            break;
        }
        if (key == '\r' || key == '\n') {
            if (found == 0)
                continue;
            const char *chosen = options[selected].value;
            if (strlen(chosen) >= size)
                continue;
            strcpy(value, chosen);
            result = UI_NEXT;
            break;
        }
        if (key == UI_KEY_UP || key == UI_KEY_PAGE_UP || key == UI_KEY_HOME) {
            if (found)
                selected =
                    matches[key == UI_KEY_HOME ? 0
                            : selected_position > (key == UI_KEY_UP ? 1 : ui_visible_rows())
                                ? selected_position - (key == UI_KEY_UP ? 1 : ui_visible_rows())
                                : 0];
        } else if (key == UI_KEY_DOWN || key == UI_KEY_PAGE_DOWN || key == UI_KEY_END) {
            if (found) {
                size_t next = key == UI_KEY_END ? found - 1
                                                : selected_position +
                                                      (key == UI_KEY_DOWN ? 1 : ui_visible_rows());
                selected = matches[next < found ? next : found - 1];
            }
        } else if (searchable && (key == 127 || key == 8)) {
            size_t length = strlen(search);
            if (length > 0)
                search[length - 1] = '\0';
        } else if (searchable && key >= 32 && key < 127) {
            size_t length = strlen(search);
            if (length + 1 < sizeof(search) && length + 4 < ui_panel_width()) {
                search[length] = (char)key;
                search[length + 1] = '\0';
            }
        }
    }
    free(matches);
    return result;
}

static enum ui_result ui_catalog_menu(const char *title, char *value, size_t size,
                                      enum ui_page page, enum execution_mode mode)
{
    struct ui_catalog catalog = ui_load_catalog(page);
    enum ui_result result = ui_menu(title, catalog.options, catalog.count, value, size, page, mode);
    ui_catalog_free(&catalog);
    return result;
}

static const char *ui_keymap_group_label(const char *group)
{
    static char label[160];
    struct cenere_keyboard maps[256];
    size_t count=cenere_keyboards(maps,256,true);
    for (size_t i=0;i<count;i++)
        if (strlen(group)==strcspn(maps[i].layout,",") && !strncmp(group,maps[i].layout,strlen(group))) {
            snprintf(label,sizeof(label),"%s",maps[i].name); return label;
        }
    return group;
}

struct ui_keyboard_list {
    struct ui_catalog catalog;
    char search[80];
    size_t selected, matches[256], count, position;
};

static void ui_keyboard_filter(struct ui_keyboard_list *list)
{
    list->count = 0;
    list->position = 0;
    for (size_t i = 0; i < list->catalog.count; i++) {
        if (!strcasestr(list->catalog.options[i].label, list->search) &&
            !strcasestr(list->catalog.options[i].value, list->search))
            continue;
        if (i == list->selected)
            list->position = list->count;
        list->matches[list->count++] = i;
    }
    if (list->count)
        list->selected = list->matches[list->position];
}

static size_t ui_keyboard_input(size_t row, size_t column, size_t width, const char *value,
                                const char *placeholder, bool focused)
{
    size_t visible = width > 2 ? width - 2 : 0;
    size_t length = strlen(value), start = length > visible ? length - visible : 0;
    const char *text = length ? value + start : placeholder;
    printf("\033[%zu;%zuH%s %-*.*s %s", row, column, focused ? "\033[7m" : "\033[2m",
           (int)visible, (int)visible, text, "\033[0m");
    return column + 1 + length - start;
}

static size_t ui_keyboard_draw_list(const struct ui_keyboard_list *list, const char *title,
                                    size_t column, size_t width, size_t rows, bool focused)
{
    printf("\033[6;%zuH%c %.*s", column, focused ? '>' : ' ', (int)width - 2, title);
    size_t cursor = ui_keyboard_input(7, column, width, list->search, "Type to filter...", focused);
    if (!list->count)
        printf("\033[9;%zuH%.*s", column, (int)width, "No matching choices.");
    else {
        size_t first = list->position >= rows ? list->position - rows + 1 : 0;
        for (size_t i = first; i < list->count && i < first + rows; i++) {
            bool selected = i == list->position;
            printf("\033[%zu;%zuH%s%c %-*.*s\033[0m", 9 + i - first, column,
                   selected && focused ? "\033[7m" : "", selected ? '>' : ' ',
                   (int)width - 2, (int)width - 2, list->catalog.options[list->matches[i]].label);
        }
        if (list->count > rows)
            printf("\033[%zu;%zuH%zu / %zu", 9 + rows, column, list->position + 1, list->count);
    }
    return cursor;
}

static void ui_keyboard_edit(char *value, size_t size, int key)
{
    size_t length = strlen(value);
    if ((key == 127 || key == 8) && length) {
        do {
            length--;
        } while (length && ((unsigned char)value[length] & 0xc0) == 0x80);
        value[length] = '\0';
    } else if (key == 21)
        explicit_bzero(value, size);
    else if (key >= 32 && key <= 255 && key != 127 && length + 1 < size) {
        value[length] = (char)key;
        value[length + 1] = '\0';
    }
}

static void ui_keyboard_list_key(struct ui_keyboard_list *list, int key, size_t rows)
{
    if (key == UI_KEY_UP || key == UI_KEY_PAGE_UP || key == UI_KEY_HOME) {
        size_t step = key == UI_KEY_UP ? 1 : rows;
        if (list->count)
            list->selected = list->matches[key == UI_KEY_HOME || list->position < step
                                               ? 0 : list->position - step];
    } else if (key == UI_KEY_DOWN || key == UI_KEY_PAGE_DOWN || key == UI_KEY_END) {
        size_t next = list->position + (key == UI_KEY_DOWN ? 1 : rows);
        if (list->count)
            list->selected = list->matches[key == UI_KEY_END || next >= list->count
                                               ? list->count - 1 : next];
    } else
        ui_keyboard_edit(list->search, sizeof(list->search), key);
}

static bool ui_keyboard_variants(struct ui_keyboard_list *variants,
                                 const struct cenere_keyboard *maps, size_t count,
                                 const char *group, const char *keymap)
{
    ui_catalog_free(&variants->catalog);
    *variants = (struct ui_keyboard_list){0};
    const char *preferred = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strlen(group) != strcspn(maps[i].layout, ",") ||
            strncmp(group, maps[i].layout, strlen(group)))
            continue;
        char label[256];
        snprintf(label, sizeof(label), "%.159s (%.63s)", maps[i].variant_name, maps[i].keymap);
        ui_catalog_add(&variants->catalog, label, maps[i].keymap);
        if (!preferred || !strcmp(maps[i].keymap, group))
            preferred = maps[i].keymap;
    }
    if (variants->catalog.failed || !variants->catalog.count)
        return false;
    qsort(variants->catalog.options, variants->catalog.count,
          sizeof(*variants->catalog.options), ui_compare_options);
    for (size_t i = 0; i < variants->catalog.count; i++)
        if (!strcmp(variants->catalog.options[i].value, preferred))
            variants->selected = i;
    for (size_t i = 0; i < variants->catalog.count; i++)
        if (!strcmp(variants->catalog.options[i].value, keymap))
            variants->selected = i;
    return true;
}

static enum ui_result ui_keyboard_page(struct installation_plan *plan, enum execution_mode mode,
                                       char *applied_keymap, size_t applied_size)
{
    struct cenere_keyboard maps[256];
    size_t count = cenere_keyboards(maps, UI_OPTIONS(maps), mode == DEMO_INSTALLATION);
    struct ui_keyboard_list layouts = {0}, variants = {0};
    enum { KEYBOARD_LAYOUT, KEYBOARD_VARIANT, KEYBOARD_TEST } focus = KEYBOARD_LAYOUT;
    enum ui_result result = UI_CANCEL;
    char group[64] = "", test[128] = "";

    for (size_t i = 0; i < count; i++) {
        char code[64];
        snprintf(code, sizeof(code), "%.*s", (int)strcspn(maps[i].layout, ","), maps[i].layout);
        ui_catalog_add(&layouts.catalog, maps[i].name, code);
    }
    if (layouts.catalog.failed || !layouts.catalog.count)
        goto done;
    qsort(layouts.catalog.options, layouts.catalog.count, sizeof(*layouts.catalog.options), ui_compare_options);
    for (size_t i = 0; i < layouts.catalog.count; i++)
        if (!strcmp(layouts.catalog.options[i].value, plan->keymap_group))
            layouts.selected = i;

    for (;;) {
        ui_keyboard_filter(&layouts);
        const char *selected_group = layouts.catalog.options[layouts.selected].value;
        if (strcmp(group, selected_group)) {
            snprintf(group, sizeof(group), "%s", selected_group);
            if (!ui_keyboard_variants(&variants, maps, count, group, plan->keymap))
                break;
        }
        ui_keyboard_filter(&variants);
        size_t lines = ui_lines(), width = ui_panel_width(), left = ui_panel_left();
        bool fits = ui_columns() >= 60 && lines >= 18;
        ui_header("Choose the keyboard layout", UI_KEYMAP, mode, fits);
        if (!fits) {
            ui_row(6, "Resize to at least 60 columns and 18 rows.");
            fflush(stdout);
            int key = ui_key();
            if (key == 3 || key == UI_KEY_EOF)
                break;
            if (key == 27) {
                result = UI_BACK;
                break;
            }
            continue;
        }
        size_t rows = lines - 16, column_width = (width - 4) / 2;
        size_t layout_cursor = ui_keyboard_draw_list(&layouts, "Layout", left, column_width,
                                                     rows, focus == KEYBOARD_LAYOUT);
        size_t variant_cursor = ui_keyboard_draw_list(&variants, "Variant", left + column_width + 4,
                                                      column_width, rows, focus == KEYBOARD_VARIANT);
        ui_row(lines - 6, focus == KEYBOARD_TEST ? "> Test the keyboard (optional)"
                                                : "  Test the keyboard (optional)");
        size_t test_cursor = ui_keyboard_input(lines - 5, left, width, test,
                                               *applied_keymap ? "Type here, or press Enter to continue."
                                                               : "Confirm a layout to try it.",
                                               focus == KEYBOARD_TEST);
        char active[416] = "Confirm the selection to activate the layout.";
        for (size_t i = 0; i < count; i++)
            if (!strcmp(maps[i].keymap, applied_keymap))
                snprintf(active, sizeof(active), "Active: %.159s - %.159s (%.63s)",
                         maps[i].name, maps[i].variant_name, maps[i].keymap);
        ui_row(lines - 3, active);
        ui_center(lines - 2, focus == KEYBOARD_TEST
                                ? "Enter: continue   Tab: layout   Esc: edit"
                                : "Enter: select   Tab: next field   Esc: back");
        printf("\033[%zu;%zuH", focus == KEYBOARD_TEST ? lines - 5 : 7,
               focus == KEYBOARD_LAYOUT ? layout_cursor
               : focus == KEYBOARD_VARIANT ? variant_cursor : test_cursor);
        fflush(stdout);
        int key = ui_key();
        if (key == 3 || key == UI_KEY_EOF)
            break;
        if (key == 27 || key == UI_KEY_LEFT) {
            if (focus == KEYBOARD_LAYOUT) {
                result = UI_BACK;
                break;
            }
            focus = focus == KEYBOARD_TEST && variants.catalog.count > 1
                        ? KEYBOARD_VARIANT : KEYBOARD_LAYOUT;
        } else if (key == '\r' || key == '\n' || key == '\t') {
            if (focus == KEYBOARD_TEST) {
                if (key == '\t')
                    focus = KEYBOARD_LAYOUT;
                else {
                    result = UI_NEXT;
                    break;
                }
                continue;
            }
            if (!layouts.count)
                continue;
            if (focus == KEYBOARD_LAYOUT && variants.catalog.count > 1) {
                focus = KEYBOARD_VARIANT;
                continue;
            }
            if (!variants.count)
                continue;
            const char *keymap = variants.catalog.options[variants.selected].value;
            if (!cenere_select_keyboard(plan, keymap, mode == DEMO_INSTALLATION) ||
                (strcmp(applied_keymap, keymap) && mode == REAL_INSTALLATION &&
                 !cenere_apply_keyboard(plan))) {
                ui_end();
                fprintf(stderr, "%s\n", cenere_error());
                break;
            }
            if (strcmp(applied_keymap, plan->keymap)) {
                explicit_bzero(test, sizeof(test));
                if (*applied_keymap) {
                    explicit_bzero(plan->password, sizeof(plan->password));
                    explicit_bzero(plan->existing_password, sizeof(plan->existing_password));
                }
                snprintf(applied_keymap, applied_size, "%s", plan->keymap);
            }
            focus = KEYBOARD_TEST;
        } else if (focus == KEYBOARD_TEST)
            ui_keyboard_edit(test, sizeof(test), key);
        else
            ui_keyboard_list_key(focus == KEYBOARD_LAYOUT ? &layouts : &variants, key, rows);
    }
done:
    explicit_bzero(test, sizeof(test));
    ui_catalog_free(&layouts.catalog);
    ui_catalog_free(&variants.catalog);
    return result;
}

static void ui_check_progress(const char *, const char *, const char *, uint64_t);
static enum ui_page checking_page = UI_REVIEW;

static bool ui_inspect_storage(struct installation_plan *plan, enum ui_page page)
{
    checking_page=page;
    cenere_set_progress_callback(ui_check_progress);
    bool ok=cenere_inspect(plan);
    cenere_set_progress_callback(NULL);
    checking_page=UI_REVIEW;
    return ok;
}

static enum ui_result ui_storage_problem(void)
{
    char message[512];snprintf(message,sizeof(message),"%s",cenere_error());
    for (;;) {
        ui_header("This storage choice cannot be used",UI_ENCRYPTION,REAL_INSTALLATION,false);
        size_t width=ui_panel_width(),row=6;
        const char *at=message;
        while (*at && row+3<ui_lines()) {
            size_t length=strlen(at),take=length<width?length:width;
            if (take<length) {
                size_t word=take;
                while (word && at[word]!=' ') word--;
                if (word) take=word;
            }
            char line[513];snprintf(line,sizeof(line),"%.*s",(int)take,at);
            ui_row(row++,line);at+=take;while (*at==' ') at++;
        }
        ui_center(ui_lines()-2,"Enter/Esc: change disk choices   Ctrl+C: cancel");
        fflush(stdout);
        int key=ui_key();
        if (key==3 || key==UI_KEY_EOF) return UI_CANCEL;
        if (key=='\n' || key=='\r' || key==27) return UI_BACK;
    }
}

static enum ui_result ui_destination(struct installation_plan *plan, enum execution_mode mode)
{
    char previous[PATH_MAX];
    strcpy(previous, plan->target);
    if (mode == DEMO_INSTALLATION) {
        enum ui_result result =
            ui_menu("Choose the destination disk", demo_disks, UI_OPTIONS(demo_disks), plan->target,
                    sizeof(plan->target), UI_DESTINATION, mode);
        if (result == UI_NEXT) {
            plan->existing_home = strcmp(plan->target, "/demo/target") == 0;
            if (strcmp(previous, plan->target) != 0)
                plan->preserve_home = plan->existing_home;
        }
        return result;
    }
    struct cenere_disk *disks = calloc(256, sizeof(*disks));
    struct ui_option *options = calloc(256, sizeof(*options));
    if (!disks || !options) {
        free(disks);
        free(options);
        return UI_CANCEL;
    }
    size_t count = cenere_disks(disks, 256);
    for (size_t i = 0; i < count; i++)
        options[i] = (struct ui_option){.label = disks[i].label, .value = disks[i].device};
    enum ui_result result = ui_menu("Choose the destination disk", options, count, plan->target,
                                    sizeof(plan->target), UI_DESTINATION, mode);
    free(options);
    free(disks);
    if (result == UI_NEXT) {
        if (strcmp(previous,plan->target))
            explicit_bzero(plan->existing_password,sizeof(plan->existing_password));
        if (strcmp(previous,plan->target)) plan->discard_encrypted=false;
        if (!ui_inspect_storage(plan,UI_DESTINATION)) {
            ui_end();
            fprintf(stderr, "%s\n", cenere_error());
            return UI_CANCEL;
        }
        if (strcmp(previous, plan->target) != 0) {
            plan->preserve_home = plan->existing_home;
            plan->alongside = plan->existing_os;
        }
    }
    return result;
}

static enum ui_result ui_preserve_home(struct installation_plan *plan, enum execution_mode mode)
{
    char choice[4];
    strcpy(choice, plan->preserve_home ? "yes" : "no");
    enum ui_result result =
        ui_menu("Keep the existing home files?", preserve_options, UI_OPTIONS(preserve_options),
                choice, sizeof(choice), UI_PRESERVE_HOME, mode);
    if (result == UI_NEXT)
        plan->preserve_home = strcmp(choice, "yes") == 0;
    return result;
}

static enum ui_result ui_encryption(struct installation_plan *plan, enum execution_mode mode)
{
    char choice[4];
    strcpy(choice, plan->encrypt ? "yes" : "no");
    enum ui_result result =
        ui_menu("Encrypt the system and home?", encryption_options, UI_OPTIONS(encryption_options),
                choice, sizeof(choice), UI_ENCRYPTION, mode);
    if (result == UI_NEXT) {
        plan->encrypt = strcmp(choice, "yes") == 0;
    }
    return result;
}

static void show_plan(FILE *, const struct installation_plan *, enum execution_mode);

static enum ui_result ui_review(const struct installation_plan *plan, enum execution_mode mode)
{
    char *summary=NULL; size_t length=0;
    FILE *stream=open_memstream(&summary,&length);
    if (!stream) return UI_CANCEL;
    show_plan(stream,plan,mode);
    fclose(stream);
    char keyword[32]="";
    size_t scroll=0;
    bool overflow=false;
    const char *problem=NULL;
    for (;;) {
        ui_header("Review and confirm",UI_REVIEW,mode,false);
        size_t rows=ui_lines()>14 ? ui_lines()-11 : 2, width=ui_panel_width();
        size_t line_number=0,row=6;
        const char *at=summary;
        while (*at) {
            size_t part=strcspn(at,"\n");
            if (part>width) part=width;
            if (line_number>=scroll && row<6+rows) {
                char line[256]; snprintf(line,sizeof(line),"%.*s",(int)part,at);
                ui_row(row++,line);
            }
            line_number++;
            at+=part;
            if (*at=='\n') at++;
        }
        size_t field=ui_lines()>5 ? ui_lines()-4 : 12;
        ui_row(field - 1, "Type INSTALL to begin:");
        size_t cursor = ui_input(field, keyword, false, "");
        if (problem) ui_row(field+1,problem);
        ui_center(ui_lines()>2?ui_lines()-2:1,"Up/Down: scroll   Esc: edit   Ctrl+C: cancel");
        printf("\033[%zu;%zuH\033[?25h",field,cursor); fflush(stdout);
        int key=ui_key();
        if (key=='\r' || key=='\n') {
            if (!overflow && !strcmp(keyword,"INSTALL")) { free(summary); return UI_NEXT; }
            problem="Type INSTALL exactly to begin.";
        } else if (key==27 || key==UI_KEY_LEFT) { free(summary); return UI_BACK; }
        else if (key==3 || key==UI_KEY_EOF) { free(summary); return UI_CANCEL; }
        else if (key==UI_KEY_UP || key==UI_KEY_PAGE_UP || key==UI_KEY_HOME)
            scroll=key==UI_KEY_HOME?0:scroll>(key==UI_KEY_UP?1:rows)?scroll-(key==UI_KEY_UP?1:rows):0;
        else if (key==UI_KEY_DOWN || key==UI_KEY_PAGE_DOWN || key==UI_KEY_END) {
            size_t last=line_number>rows?line_number-rows:0;
            size_t next=key==UI_KEY_END?last:scroll+(key==UI_KEY_DOWN?1:rows);
            scroll=next<last?next:last;
        } else if (key==127 || key==8) {
            if (*keyword) keyword[strlen(keyword)-1]=0;
            overflow=false; problem=NULL;
        } else if (key>=32 && key<=126) {
            if (strlen(keyword)+1<sizeof(keyword)) {
                size_t used=strlen(keyword);keyword[used]=(char)key;keyword[used+1]=0;problem=NULL;
            } else overflow=true;
        }
    }
}

static enum ui_page ui_next_page(enum ui_page page, enum execution_mode mode)
{
    page++;
    while (page < UI_PAGE_COUNT && !ui_page_visible(page, mode))
        page++;
    return page;
}

static enum ui_page ui_previous_page(enum ui_page page, enum execution_mode mode)
{
    page--;
    while (page > UI_KEYMAP && !ui_page_visible(page, mode))
        page--;
    return page;
}

static int confirm_installation(const struct installation_plan *, enum execution_mode);

static bool ui_wizard(struct installation_plan *plan, enum execution_mode mode)
{
    char confirmation[sizeof(plan->password)] = {0};
    const char *password_error = NULL;
    bool confirmed = false;
    char applied_keymap[64]="";
    const char *unlock_error=NULL;
    enum ui_page page = UI_KEYMAP;

    ui_active_plan = plan;
    if (!ui_begin()) {
        ui_active_plan = NULL;
        fprintf(stderr, "Cannot open the terminal wizard.\n");
        return false;
    }
    for (;;) {
        enum ui_result result = UI_CANCEL;
        switch (page) {
        case UI_KEYMAP:
            result = ui_keyboard_page(plan, mode, applied_keymap, sizeof(applied_keymap));
            break;
        case UI_UNLOCK: {
            static const struct ui_option choices[]={
                {"Unlock and inspect before choosing what to preserve","unlock"},
                {"Replace encrypted contents without preserving them","replace"}};
            char choice[16];strcpy(choice,plan->discard_encrypted?"replace":"unlock");
            result=ui_menu("Encrypted storage found",choices,UI_OPTIONS(choices),choice,sizeof(choice),page,mode);
            if (result==UI_NEXT) {
                plan->discard_encrypted=!strcmp(choice,"replace");
                if (plan->discard_encrypted) {
                    explicit_bzero(plan->existing_password,sizeof(plan->existing_password));
                    plan->preserve_home=false; plan->alongside=false;
                }
            }
            break;
        }
        case UI_UNLOCK_PASSWORD:
            result=ui_text("Existing disk password (unlock for inspection)",plan->existing_password,
                           sizeof(plan->existing_password),true,true,NULL,page,mode,unlock_error);
            if (result==UI_NEXT) {
                if (!ui_inspect_storage(plan,UI_UNLOCK_PASSWORD)) {
                    explicit_bzero(plan->existing_password,sizeof(plan->existing_password));
                    unlock_error=cenere_error();continue;
                }
                unlock_error=NULL;
                plan->preserve_home=plan->existing_home;
                plan->alongside=plan->existing_os;
                plan->encrypt=plan->existing_encryption;
            }
            break;
        case UI_DESTINATION:
            result = ui_destination(plan, mode);
            break;
        case UI_OTHER_SYSTEMS: {
            static const struct ui_option choices[] = {
                {"Keep existing systems and install alongside", "keep"},
                {"Replace existing systems", "replace"}};
            char choice[16];
            strcpy(choice, plan->alongside ? "keep" : "replace");
            result = ui_menu(plan->systems, choices, UI_OPTIONS(choices), choice, sizeof(choice),
                             page, mode);
            if (result == UI_NEXT)
                plan->alongside = !strcmp(choice, "keep");
            break;
        }
        case UI_PRESERVE_HOME:
            result = ui_preserve_home(plan, mode);
            break;
        case UI_ENCRYPTION:
            result = ui_encryption(plan, mode);
            if (result==UI_NEXT && mode==REAL_INSTALLATION) {
                checking_page=UI_ENCRYPTION;
                cenere_set_progress_callback(ui_check_progress);
                bool ready=cenere_check_storage(plan);
                cenere_set_progress_callback(NULL);
                checking_page=UI_REVIEW;
                if (!ready) result=ui_storage_problem();
            }
            break;
        case UI_LOCALE:
            result = ui_catalog_menu("Choose the language and locale", plan->locale,
                                     sizeof(plan->locale), page, mode);
            break;
        case UI_TIMEZONE:
            result = ui_catalog_menu("Choose the timezone", plan->timezone, sizeof(plan->timezone),
                                     page, mode);
            break;
        case UI_HOSTNAME:
            suggest_hostname(plan);
            result = ui_text("Computer name", plan->hostname, sizeof(plan->hostname), false, false,
                             valid_hostname, page, mode, NULL);
            break;
        case UI_USERNAME:
            result = ui_text("Local username", plan->username, sizeof(plan->username), false, false,
                             valid_username, page, mode, NULL);
            break;
        case UI_PASSWORD:
            result = ui_text("User password", plan->password, sizeof(plan->password), true, true,
                             NULL, page, mode, NULL);
            if (result == UI_NEXT) {
                explicit_bzero(confirmation, sizeof(confirmation));
                password_error = NULL;
            }
            break;
        case UI_PASSWORD_CONFIRM:
            result = ui_text("Confirm user password", confirmation, sizeof(confirmation), true,
                             false, NULL, page, mode, password_error);
            if (result == UI_NEXT && strcmp(confirmation, plan->password) != 0) {
                explicit_bzero(confirmation, sizeof(confirmation));
                password_error = "Passwords do not match. Try again or press Esc.";
                continue;
            }
            break;
        case UI_REVIEW:
            if (mode==REAL_INSTALLATION) {
                cenere_set_progress_callback(ui_check_progress);
                ui_check_progress("Checking the installation plan", "Please wait.", "", 0);
                bool ready = cenere_preflight(plan);
                cenere_set_progress_callback(NULL);
                if (!ready) {
                    ui_end(); fprintf(stderr,"%s\n",cenere_error()); result=UI_CANCEL; break;
                }
            }
            result=ui_review(plan,mode);
            confirmed=result==UI_NEXT;
            break;
        case UI_PAGE_COUNT:
            break;
        }
        if (confirmed || result == UI_CANCEL)
            break;
        if (result == UI_BACK) {
            if (page == UI_KEYMAP)
                break;
            page = ui_previous_page(page, mode);
        } else {
            page = ui_next_page(page, mode);
        }
    }
    explicit_bzero(confirmation, sizeof(confirmation));
    ui_end();
    ui_active_plan = NULL;
    if (!confirmed)
        fprintf(stderr, "Installation cancelled.\n");
    return confirmed;
}

static bool collect_choices(struct installation_plan *plan, enum execution_mode mode)
{
    fprintf(stderr,"\nKeyboard > Disk > Preferences > Account > Summary > Install\n");
    if (!ask_question("Keyboard layout (for example us or it)",plan->keymap,sizeof(plan->keymap),NULL,configuration_name)) return false;
    if (!cenere_select_keyboard(plan,plan->keymap,mode==DEMO_INSTALLATION)) {
        fprintf(stderr,"%s\n",cenere_error());return false;
    }
    if (mode==REAL_INSTALLATION && !cenere_apply_keyboard(plan)) {
        fprintf(stderr,"%s\n",cenere_error());return false;
    }
    char keyboard_test[128]="test";
    if (!ask_question("Test the keyboard (no password)",keyboard_test,sizeof(keyboard_test),NULL,NULL)) return false;
    fprintf(stderr,"\nDisk preparation\n");
    if (mode == REAL_INSTALLATION) {
        bool first = !*plan->snapshot;
        if (!*plan->target) {
            struct cenere_disk disks[32];
            size_t count = cenere_disks(disks, 32);
            if (!count) {
                fprintf(stderr, "No unused writable destination disks available.\n");
                return false;
            }
            for (size_t i = 0; i < count; i++)
                fprintf(stderr, "%s\n", disks[i].label);
            strcpy(plan->target, disks[0].device);
            if (!ask_question("Destination disk", plan->target, sizeof(plan->target), NULL, NULL))
                return false;
        }
        if (!cenere_inspect(plan)) {
            fprintf(stderr, "%s\n", cenere_error());
            return false;
        }
        if (plan->locked_storage) {
            char choice[16]="unlock";
            if (!ask_question("Encrypted storage",choice,sizeof(choice),"unlock/replace",NULL)) return false;
            plan->discard_encrypted=!strcmp(choice,"replace");
            if (!plan->discard_encrypted) {
                for (;;) {
                    if (!read_password("Existing disk password",plan->existing_password,sizeof(plan->existing_password)) ||
                        !strcmp(plan->existing_password,"cancel")) return false;
                    if (cenere_inspect(plan)) break;
                    fprintf(stderr,"%s\n",cenere_error());
                    explicit_bzero(plan->existing_password,sizeof(plan->existing_password));
                }
                plan->encrypt=plan->existing_encryption;
            } else {plan->preserve_home=false;plan->alongside=false;}
        }
        if (first && !plan->discard_encrypted) {
            plan->preserve_home = plan->existing_home;
            plan->alongside = plan->existing_os;
        }
        if (plan->existing_os && !plan->discard_encrypted) {
            fprintf(stderr, "Detected: %s\n", plan->systems);
            char choice[16];
            strcpy(choice, plan->alongside ? "keep" : "replace");
            if (!ask_question("Other systems on disk", choice, sizeof(choice), "keep/replace",
                              NULL))
                return false;
            plan->alongside = !strcmp(choice, "keep");
        }
    }
    if (plan->existing_home && !plan->discard_encrypted) {
        char choice[4];
        strcpy(choice, plan->preserve_home ? "yes" : "no");
        if (!ask_question("Preserve the existing home files", choice, sizeof(choice), "yes/no",
                          NULL))
            return false;
        plan->preserve_home = strcmp(choice, "yes") == 0;
    }
    char encryption[4];
    strcpy(encryption, plan->encrypt ? "yes" : "no");
    if (!ask_question("Encrypt both system and home", encryption, sizeof(encryption), "yes/no",
                      NULL))
        return false;
    plan->encrypt = strcmp(encryption, "yes") == 0;
    if (mode==REAL_INSTALLATION && !cenere_check_storage(plan)) {
        fprintf(stderr,"%s\n",cenere_error());
        return false;
    }
    suggest_hostname(plan);
    fprintf(stderr,"\n1 of 4: Preferences\n");
    if (!ask_question("System locale (for example en_US.UTF-8)", plan->locale, sizeof(plan->locale),
                      NULL, configuration_name) ||
        !ask_question("Timezone (for example Europe/Rome or UTC)", plan->timezone,
                      sizeof(plan->timezone), NULL, configuration_name) ||
        !ask_question("Hostname (lowercase letters, digits, hyphens)", plan->hostname,
                      sizeof(plan->hostname), NULL, valid_hostname) ||
        !ask_question("Username (lowercase letters, digits, underscores, hyphens)", plan->username,
                      sizeof(plan->username), NULL, valid_username))
        return false;
    return ask_secret(plan->password, sizeof(plan->password), "User password",
                      "Confirm user password");
}

static size_t plan_steps(const struct installation_plan *plan,
                         enum installation_step steps[STEP_COUNT])
{
    size_t count = 0;
    if (complete_installation(plan)) {
        if (plan->existing_home && plan->preserve_home && !plan->reuse_storage)
            steps[count++] = BACKUP_HOME;
        if (plan->encrypt)
            steps[count++] = SETUP_ENCRYPTION;
        steps[count++] = FORMAT_ROOT;
        if (!plan->reuse_storage)
            steps[count++] = FORMAT_HOME;
        steps[count++] = MOUNT_ROOT;
        if (plan->existing_home && plan->preserve_home && !plan->reuse_storage)
            steps[count++] = RESTORE_HOME;
    }
    steps[count++] = TRANSFER_ROOT;
    if (complete_installation(plan)) {
        steps[count++] = CONFIGURE_FSTAB;
        steps[count++] = CONFIGURE_LOCALIZATION;
        steps[count++] = CONFIGURE_NETWORK;
        steps[count++] = CONFIGURE_ACCOUNTS;
        steps[count++] = GENERATE_INITRAMFS;
        steps[count++] = CONFIGURE_BOOT;
    }
    return count;
}

static void describe_step(FILE *output, enum installation_step step,
                          const struct installation_plan *plan)
{
    switch (step) {
    case BACKUP_HOME:
        fprintf(output,
                "Keep the original home source until its copied contents are verified.\n");
        break;
    case SETUP_ENCRYPTION:
        fprintf(output, "Use one encrypted container for both the system and /home %s.\n",
                plan->reuse_storage ? "retaining the existing disk password"
                                    : "using the local user's password");
        break;
    case FORMAT_ROOT:
        fprintf(output, "Prepare a Btrfs root subvolume; root and home share free space.\n");
        break;
    case FORMAT_HOME:
        fprintf(output, "Create the home subvolume in the shared Btrfs filesystem%s.\n",
                plan->existing_home && !plan->preserve_home && !plan->alongside
                    ? "; existing home files will be replaced"
                    : "");
        break;
    case MOUNT_ROOT:
        fprintf(output, "Mount root, /home, and the required boot filesystem.\n");
        break;
    case RESTORE_HOME:
        fprintf(output, "Copy and verify preserved home files in the new home subvolume.\n");
        break;
    case TRANSFER_ROOT:
        fprintf(output, "Copy the prepared system files; replace contents outside /home.\n");
        break;
    case CONFIGURE_FSTAB:
        fprintf(output, "Configure /etc/fstab for the target filesystems.\n");
        break;
    case CONFIGURE_LOCALIZATION:
        fprintf(output, "Set keymap %s, locale %s, timezone %s.\n", plan->keymap, plan->locale,
                plan->timezone);
        break;
    case CONFIGURE_NETWORK:
        fprintf(output, "Set hostname %s and enable the payload's networking service.\n",
                plan->hostname);
        break;
    case CONFIGURE_ACCOUNTS:
        fprintf(output,
                "Configure user %s with the supplied password and distribution administrator policy; "
                "keep the root account locked%s.\n",
                plan->username, plan->preserve_home ? " and preserve home ownership" : "");
        break;
    case GENERATE_INITRAMFS:
        fprintf(output, "Generate the target initramfs.\n");
        break;
    case CONFIGURE_BOOT:
        fprintf(output, "Configure %s for %s boot.\n", plan->bootloader, plan->firmware);
        break;
    case STEP_COUNT:
        break;
    }
}

static void show_plan(FILE *output, const struct installation_plan *plan, enum execution_mode mode)
{
    fprintf(output, "\n%s installation review\nDestination: %s\n",
            mode == DEMO_INSTALLATION ? "DEMO" : "REAL",
            mode == DEMO_INSTALLATION ? ui_label(demo_disks, UI_OPTIONS(demo_disks), plan->target)
                                      : plan->target);
    if (complete_installation(plan)) {
        if (mode == REAL_INSTALLATION) {
            fprintf(output, "Disk size: %.1f GiB\n",
                    (double)plan->disk_bytes / (1024 * 1024 * 1024));
            if (plan->existing_os)
                fprintf(output, "Detected: %s\nOther systems: %s\n", plan->systems,
                        plan->alongside ? "preserve" : "replace");
            fprintf(output, "Storage: %s\n", plan->layout);
        }
        fprintf(output,
                "Keyboard: %s (console %s)\nLanguage and locale: %s\nTimezone: %s\n"
                "Computer name: %s\nLocal user: %s\nUser password: entered\n"
                "The reviewed root will be replaced; storage operations are listed above.\n",
                ui_keymap_group_label(plan->keymap_group), plan->keymap, plan->locale, plan->timezone, plan->hostname, plan->username);
        if (mode == REAL_INSTALLATION && plan->alongside && !plan->preserve_home)
            fprintf(output, "The new system will use a new home; existing systems keep their home data.\n");
        else if (plan->existing_home)
            fprintf(output, "/home will be %s.\n", plan->preserve_home ? "preserved" : "replaced");
        else
            fprintf(output, "Root and /home will use subvolumes sharing one Btrfs filesystem.\n");
        fprintf(output, "Encryption: %s.\n",
                plan->encrypt ? (plan->reuse_storage ? "existing shared container; its existing disk password is retained"
                                                        : "system and home; uses the local user's password") : "none");
        if (plan->existing_home && plan->preserve_home && !plan->reuse_storage)
            fprintf(output,"The source home partition is retained; its files are copied and verified.\n");
    } else {
        fprintf(output,
                "System files: %s\nContents outside /home will be replaced.\n"
                "/home will be preserved. Boot and account setup are not included.\n",
                plan->source);
    }
    if (mode == DEMO_INSTALLATION)
        fprintf(output, "This is a simulation; no system changes will be made.\n");
}

static int confirm_installation(const struct installation_plan *plan, enum execution_mode mode)
{
    char *answer = NULL;
    size_t capacity = 0;
    bool confirmed = false;

    if (mode == DEMO_INSTALLATION)
        fprintf(stderr,
                "DEMO MODE: confirmation starts a simulation; no files or devices are changed.\n");
    fprintf(stderr,
            "Target: %s\n"
            "%s\n"
            "Type INSTALL to begin, edit to revise, or cancel: ",
            mode == DEMO_INSTALLATION ? ui_label(demo_disks, UI_OPTIONS(demo_disks), plan->target)
                                      : plan->target,
            complete_installation(plan)
                ? (plan->existing_home && plan->preserve_home
                       ? "The root will be replaced; existing /home files will be preserved."
                       : "The root and /home will be replaced.")
                : "Contents outside /home will be replaced; /home will be preserved.");
    fflush(stderr);

    ssize_t length = getline(&answer, &capacity, stdin);
    if (length == -1) {
        fprintf(stderr, "No confirmation received.\n");
        free(answer);
        return false;
    }
    if (memchr(answer, '\0', (size_t)length) == NULL) {
        trim_line_ending(answer, (size_t)length);
        confirmed = strcmp(answer, "INSTALL") == 0;
    }
    bool edit=complete_installation(plan) && !strcmp(answer,"edit");
    free(answer);
    if (!confirmed && !edit) fprintf(stderr,"Installation cancelled.\n");
    return confirmed?1:edit?2:0;
}

static int run_rsync(const char *source, const char *target)
{
    char source_slash[PATH_MAX];
    char target_slash[PATH_MAX];
    char *arguments[] = {
        "rsync",         "--archive",  "--hard-links", "--acls",          "--xattrs",
        "--numeric-ids", "--checksum", "--delete",     "--exclude=/home", "--",
        source_slash,    target_slash, NULL,
    };
    pid_t child;
    int status;
    size_t source_length = strlen(source);
    size_t target_length = strlen(target);

    if (source_length + 2 > sizeof(source_slash) || target_length + 2 > sizeof(target_slash)) {
        fprintf(stderr, "Source or target path is too long.\n");
        return 1;
    }
    memcpy(source_slash, source, source_length);
    source_slash[source_length] = '/';
    source_slash[source_length + 1] = '\0';
    memcpy(target_slash, target, target_length);
    target_slash[target_length] = '/';
    target_slash[target_length + 1] = '\0';

    child = fork();
    if (child == -1) {
        perror("Cannot start rsync");
        return 1;
    }
    if (child == 0) {
        execv("/usr/bin/rsync", arguments);
        perror("Cannot execute /usr/bin/rsync");
        _exit(127);
    }

    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR)
            continue;
        perror("Cannot wait for rsync");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "rsync failed; the target may be partially updated.\n");
        return 1;
    }
    return 0;
}

static bool validate_real_plan(struct installation_plan *plan)
{
    char source[PATH_MAX];
    char target[PATH_MAX];
    struct stat metadata;

    if (complete_installation(plan)) {
        if (!cenere_preflight(plan)) {
            fprintf(stderr, "%s\n", cenere_error());
            return false;
        }
        return true;
    }
    if (realpath(plan->source, source) == NULL) {
        perror("Cannot resolve the prepared system root");
        return false;
    }
    if (realpath(plan->target, target) == NULL) {
        perror("Cannot resolve the target root");
        return false;
    }
    if (strcmp(target, "/") == 0) {
        fprintf(stderr, "Refusing to use the running system's root as the target.\n");
        return false;
    }
    if (path_is_within(source, target) || path_is_within(target, source)) {
        fprintf(stderr, "The source and target paths must not overlap.\n");
        return false;
    }
    if (stat(source, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        stat(target, &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
        fprintf(stderr, "The source and target roots must be accessible directories.\n");
        return false;
    }
    if (!is_installable_payload(source))
        return false;
    if (!mount_path_exists(target)) {
        fprintf(stderr, "Target is not an existing mountpoint: %s\n", target);
        return false;
    }
    if (!mount_path_is_read_write(target)) {
        fprintf(stderr, "Target mount is read-only: %s\n", target);
        return false;
    }
    if (has_unsafe_nested_mount(target)) {
        fprintf(stderr, "Refusing nested target mounts outside /home; mount /home separately "
                        "if needed.\n");
        return false;
    }
    if (access("/usr/bin/rsync", X_OK) != 0) {
        perror("Cannot execute /usr/bin/rsync");
        return false;
    }
    strcpy(plan->source, source);
    strcpy(plan->target, target);
    return true;
}

static void ui_progress_screen(const char *phase, const char *detail,
                               const char *log_path, uint64_t elapsed, bool checking)
{
    ui_header(checking ? "Preparing the installation" : "Installing the system",
              checking ? checking_page : UI_PAGE_COUNT, REAL_INSTALLATION, false);
    ui_row(6, phase);
    if (strcmp(phase, detail))
        ui_row(8, detail);
    char clock[80];
    snprintf(clock, sizeof(clock), "%c %s: %llu:%02llu:%02llu", "|/-\\"[elapsed%4],
             checking ? "Elapsed" : "Total elapsed",
             (unsigned long long)elapsed / 3600, (unsigned long long)elapsed / 60 % 60,
             (unsigned long long)elapsed % 60);
    ui_row(10, clock);
    if (*log_path) {
        ui_row(12, "Installation log:");
        ui_row(13, log_path);
    }
    fflush(stdout);
}

static void ui_check_progress(const char *phase, const char *detail,
                              const char *log_path, uint64_t elapsed)
{
    ui_progress_screen(phase, detail, log_path, elapsed, true);
}

static void ui_install_progress(const char *phase, const char *detail,
                                const char *log_path, uint64_t elapsed)
{
    ui_progress_screen(phase, detail, log_path, elapsed, false);
}

static int execute_plan(const struct installation_plan *plan, enum execution_mode mode)
{
    if (mode == REAL_INSTALLATION && complete_installation(plan))
        return cenere_install(plan);
    enum installation_step steps[STEP_COUNT];
    size_t count = plan_steps(plan, steps);

    /* The demo branch returns before any external utility can be executed. */
    if (mode == DEMO_INSTALLATION) {
        bool completed[STEP_COUNT] = {false};
        for (size_t i = 0; i < count; i++) {
            printf("[SIMULATED] ");
            describe_step(stdout, steps[i], plan);
            completed[steps[i]] = true;
        }
        printf("Demo state: root %s; /home %s; boot setup %s.\n",
               completed[TRANSFER_ROOT] ? "transferred" : "unchanged",
               plan->preserve_home ? "preserved" : "newly created",
               completed[CONFIGURE_BOOT] ? "simulated" : "not planned");
        printf("Simulation completed. No system was installed.\n");
        return 0;
    }
    /* Reject the whole plan before starting any real operation. */
    if (count != 1 || steps[0] != TRANSFER_ROOT)
        return 1;
    describe_step(stdout, TRANSFER_ROOT, plan);
    fflush(stdout);
    if (run_rsync(plan->source, plan->target) != 0)
        return 1;
    printf("Filesystem transfer completed. Boot and system identity are not configured yet.\n");
    return 0;
}

static int finish_installation(bool use_ui)
{
    char action[16] = "reboot";
    if (use_ui && ui_begin()) {
        static const struct ui_option actions[] = {
            {"Reboot", "reboot"}, {"Power off", "poweroff"}
        };
        enum ui_result choice = ui_menu("Installation completed - choose the next action",
            actions, UI_OPTIONS(actions), action, sizeof(action), UI_PAGE_COUNT, REAL_INSTALLATION);
        ui_end();
        if (choice != UI_NEXT)
            return 0;
    } else {
        char *answer = NULL;
        size_t capacity = 0;
        for (;;) {
            fprintf(stderr, "Next action (reboot/poweroff, Enter to stay in the live environment): ");
            fflush(stderr);
            ssize_t length = getline(&answer, &capacity, stdin);
            if (length < 0) { free(answer); return 0; }
            if (memchr(answer, '\0', (size_t)length))
                continue;
            trim_line_ending(answer, (size_t)length);
            if (!*answer || !strcmp(answer, "cancel")) { free(answer); return 0; }
            if (!strcmp(answer, "reboot") || !strcmp(answer, "poweroff")) {
                strcpy(action, answer);
                free(answer);
                break;
            }
            fprintf(stderr, "Choose reboot or poweroff.\n");
        }
    }
    int result = cenere_power(!strcmp(action, "reboot"));
    if (result)
        fprintf(stderr, "Could not %s (exit %d). The installation is complete; the live environment remains available.\n",
                action, result);
    return result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    enum execution_mode mode = REAL_INSTALLATION;
    bool paths_supplied = false;
    bool disk_supplied = false;
    struct installation_plan plan;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--demo") == 0) {
        mode = DEMO_INSTALLATION;
    } else if (argc == 3 && !strcmp(argv[1], "--install")) {
        disk_supplied = true;
    } else if (argc == 3 && strncmp(argv[1], "--", 2) != 0 && strncmp(argv[2], "--", 2) != 0) {
        paths_supplied = true;
    } else if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }
    if (mode == REAL_INSTALLATION && geteuid() != 0) {
        fprintf(stderr, "Run this installer as root. Use --demo for an unprivileged simulation.\n");
        return 1;
    }
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fprintf(stderr, "Cannot prepare input for the questionnaire.\n");
        return 1;
    }
    initialize_plan(&plan, mode);
    plan.copy_only = paths_supplied;
    int result = 1;
    const char *terminal_type = getenv("TERM");
    bool use_ui = !paths_supplied && !disk_supplied && isatty(STDIN_FILENO) &&
                  isatty(STDOUT_FILENO) && terminal_type != NULL &&
                  strcmp(terminal_type, "dumb") != 0;
    if (disk_supplied) {
        if (strlen(argv[2]) >= sizeof(plan.target)) {
            fprintf(stderr, "Disk path is too long.\n");
            goto cleanup;
        }
        strcpy(plan.target, argv[2]);
    }
    if (paths_supplied) {
        if (strlen(argv[1]) >= sizeof(plan.source) || strlen(argv[2]) >= sizeof(plan.target)) {
            fprintf(stderr, "Source or target path is too long.\n");
            goto cleanup;
        }
        strcpy(plan.source, argv[1]);
        strcpy(plan.target, argv[2]);
    } else if (use_ui) {
        if (!ui_wizard(&plan, mode))
            goto cleanup;
    } else {
        fprintf(stderr,
                "%s\nPress Enter for the displayed default. Type cancel at any question to stop.\n",
                mode == DEMO_INSTALLATION ? "DEMO MODE - fictional storage; all installation "
                                            "actions are simulated. Use a sample password."
                                          : "REAL INSTALLATION - guided offline system setup.");
        for (;;) {
            if (!collect_choices(&plan,mode)) goto cleanup;
            if (mode==REAL_INSTALLATION && !validate_real_plan(&plan)) goto cleanup;
            show_plan(stderr,&plan,mode);
            int confirmation=confirm_installation(&plan,mode);
            if (confirmation==2) continue;
            if (confirmation!=1) goto cleanup;
            break;
        }
    }
    if (paths_supplied) {
        if (!validate_real_plan(&plan)) goto cleanup;
        show_plan(stderr,&plan,mode);
        if (confirm_installation(&plan,mode)!=1) goto cleanup;
    }
    if (use_ui && mode == REAL_INSTALLATION && complete_installation(&plan))
        cenere_set_progress_callback(ui_install_progress);
    result = execute_plan(&plan, mode);
    cenere_set_progress_callback(NULL);
    if (use_ui)
        fputs("\033[?25h", stdout);

cleanup:
    explicit_bzero(plan.password, sizeof(plan.password));
    explicit_bzero(plan.existing_password, sizeof(plan.existing_password));
    if (!result && mode == REAL_INSTALLATION && complete_installation(&plan))
        result = finish_installation(use_ui);
    return result;
}
