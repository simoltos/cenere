#ifndef CENERE_INSTALL_H
#define CENERE_INSTALL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct installation_plan {
    char source[PATH_MAX];
    char target[PATH_MAX];
    bool copy_only, existing_home, preserve_home, encrypt;
    bool locked_storage, reuse_storage, existing_encryption;
    bool discard_encrypted;
    char keymap[64], locale[64], timezone[96], keymap_group[32];
    char xkb_layout[64], xkb_model[64], xkb_variant[64], xkb_options[128];
    char image_id[64], live_user[32];
    size_t keymap_variant_count;
    char hostname[64], username[32], password[256], existing_password[256];
    char firmware[8], bootloader[16];
    bool existing_os, alongside;
    char systems[1024], layout[32768], snapshot[65536];
    uint64_t disk_bytes;
};

struct cenere_disk {
    char device[PATH_MAX], label[256];
};

struct cenere_keyboard {
    char keymap[64], layout[64], model[64], variant[64], options[128];
    char name[160], variant_name[160];
};

size_t cenere_keyboards(struct cenere_keyboard *maps, size_t limit, bool demo);
bool cenere_select_keyboard(struct installation_plan *plan, const char *keymap, bool demo);
bool cenere_apply_keyboard(struct installation_plan *plan);

size_t cenere_disks(struct cenere_disk *disks, size_t limit);
bool cenere_inspect(struct installation_plan *plan);
bool cenere_preflight(struct installation_plan *plan);
typedef void (*cenere_progress_callback)(const char *phase, const char *detail,
                                         const char *log_path, uint64_t elapsed);
void cenere_set_progress_callback(cenere_progress_callback callback);
int cenere_install(const struct installation_plan *plan);
int cenere_power(bool reboot);
const char *cenere_error(void);

#endif
