/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "flashutils/flashutils.h"

#define NELEM(x)    (sizeof(x) / sizeof((x)[0]))

#define SDCARD_ROOT "/sdcard"

#define KEY_CHORD_MAX       (16)
#define KEY_CHORD_MAX_KEYS  (3)

enum {
    ACTION_BOOT = 0,
    ACTION_MASS_STORAGE,
    ACTION_RELOAD,
    ACTION_REBOOT,
    ACTION_POWEROFF
};

enum {
    TYPE_FILE_LIST = 0,
    TYPE_SUBMENU,
    TYPE_ACTION,
    TYPE_TUNABLE,
    TYPE_SEPARATOR
};

struct bootable {
    char *label;
    char *path;
};

struct bootmenu_item {
    int type;
    int action;
    char *path;
    char **labels;
    struct bootmenu_item *items;
};

struct key_chord {
    int num_keys;
    int keys[KEY_CHORD_MAX_KEYS];
};

struct tunable {
    char *label;
    char *name;
    char **values;
    int num_values;
    int value;
};

static int allow_display_toggle = 1;
static int poweroff = 0;

static char **bootmenu_labels = 0;
static struct bootmenu_item *bootmenu_items = 0;

static int num_key_chords = 0;
static int key_chord_timeout = 2;
static struct key_chord key_chord_table[KEY_CHORD_MAX];

static struct bootable *bootables = 0;
static int bootable_count = 0;
static int bootable_allocated = 0;
static char **paths;

static struct tunable *tunables = 0;
static int tunable_count = 0;
static int tunable_allocated = 0;
static int settings_modified = 0;

static struct bootmenu_item static_menu_items[] = {
    {
        .type = TYPE_SEPARATOR,
    }, {
        .type = TYPE_FILE_LIST,
        .action = ACTION_BOOT,
        .path = SDCARD_ROOT
    }, {
        .type = TYPE_ACTION,
        .action = ACTION_MASS_STORAGE
    }, {
        .type = TYPE_ACTION,
        .action = ACTION_RELOAD
    }, {
        .type = TYPE_ACTION,
        .action = ACTION_REBOOT
    }, {
        .type = TYPE_ACTION,
        .action = ACTION_POWEROFF
    }, {
        .type = 0,
    }
};

static const char *static_menu_labels[] = {
    "- - -   Misc   - - - -",
    "Boot kernel from SD card",
    "Mount mass storage",
    "Reload DroidBoot",
    "Reboot",
    "Power off",
    0
};

static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int
select_file(char *msg, char* path, char *out, size_t out_len) {
    char *headers[] = { "CWM-based DroidBoot", msg,
                                   path,
                                   "",
                                   NULL };
    DIR* d;
    struct dirent* de;
    d = opendir(path);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path, strerror(errno));
        return 0;
    }

    int d_size = 0;
    int d_alloc = 10;
    char** dirs = malloc(d_alloc * sizeof(char*));
    int z_size = 1;
    int z_alloc = 10;
    char** zips = malloc(z_alloc * sizeof(char*));
    zips[0] = strdup("../");

    while ((de = readdir(d)) != NULL) {
        int name_len = strlen(de->d_name);

        if (de->d_type == DT_DIR) {
            // skip "." and ".." entries
            if (name_len == 1 && de->d_name[0] == '.') continue;
            if (name_len == 2 && de->d_name[0] == '.' &&
                de->d_name[1] == '.') continue;

            if (d_size >= d_alloc) {
                d_alloc *= 2;
                dirs = realloc(dirs, d_alloc * sizeof(char*));
            }
            dirs[d_size] = malloc(name_len + 2);
            strcpy(dirs[d_size], de->d_name);
            dirs[d_size][name_len] = '/';
            dirs[d_size][name_len+1] = '\0';
            ++d_size;
        } else if (de->d_type == DT_REG &&
                   name_len >= 4 &&
                   strncasecmp(de->d_name + (name_len-4), ".img", 4) == 0) {
            if (z_size >= z_alloc) {
                z_alloc *= 2;
                zips = realloc(zips, z_alloc * sizeof(char*));
            }
            zips[z_size++] = strdup(de->d_name);
        }
    }
    closedir(d);

    qsort(dirs, d_size, sizeof(char*), compare_string);
    qsort(zips, z_size, sizeof(char*), compare_string);

    // append dirs to the zips list
    if (d_size + z_size + 1 > z_alloc) {
        z_alloc = d_size + z_size + 1;
        zips = realloc(zips, z_alloc * sizeof(char*));
    }
    memcpy(zips + z_size, dirs, d_size * sizeof(char*));
    free(dirs);
    z_size += d_size;
    zips[z_size] = NULL;

    int result = 0;
    int chosen_item = 0;
        chosen_item = get_menu_selection(headers, zips, 0, chosen_item);

    char* item = 0;
    int item_len = 0;
    if (chosen_item >= 0) {
        item = zips[chosen_item];
        item_len = strlen(item);
    }
    if (chosen_item < 0) {
        result = -1;
    } else if (chosen_item == 0) {          // item 0 is always "../"
        // go up but continue browsing (if the caller is sdcard_directory)
        strlcpy(out, path, out_len);
        char *slash = strrchr(out, '/');
        if (slash)
            *slash = '\0';
        result = 1;
    } else if (item[item_len-1] == '/') {
        // recurse down into a subdirectory
        strlcpy(out, path, out_len);
        strlcat(out, "/", out_len);
        strlcat(out, item, out_len);
        out[strlen(out)-1] = '\0';  // truncate the trailing '/'
        result = 1;
    } else {
        // selected a zip file:  attempt to install it, and return
        // the status to the caller.
        strlcpy(out, path, out_len);
        strlcat(out, "/", out_len);
        strlcat(out, item, out_len);
        ui_print("\n-- Selected %s ...\n", item);
        result = 0;
    }

    int i;
    for (i = 0; i < z_size; ++i) free(zips[i]);
    free(zips);

    return result;
}

static void
build_cmdline(char *cmdline, size_t space) {
    int i, quotation;
    struct tunable *t = tunables;
    cmdline[0] = 0;
    strlcat(cmdline, "--command-line=", space);
    for (i = 0; i < tunable_count; ++i, ++t) {
        if (!t->num_values) {
            if (t->value) {
                strlcat(cmdline, t->name, space);
                strlcat(cmdline, " ", space);
            }
            continue;
        }
        if (!t->values[t->value][0])
            continue;
        quotation = (strpbrk(t->name, " \t=") != 0);
        if (quotation)
            strlcat(cmdline, "\"", space);
        strlcat(cmdline, t->name, space);
        if (quotation)
            strlcat(cmdline, "\"", space);
        strlcat(cmdline, "=", space);
        quotation = (strpbrk(t->values[t->value], " \t=") != 0);
        if (quotation)
            strlcat(cmdline, "\"", space);
        strlcat(cmdline, t->values[t->value], space);
        if (quotation)
            strlcat(cmdline, "\"", space);
        strlcat(cmdline, " ", space);
    }
}

extern int
run_exec_process ( char **argv);

static void
do_kexec(const char *path, const char *cmdline) {
    const char *kexec_load[] = {"/sbin/kexec", "-l", path, cmdline, NULL};
    run_exec_process((char **)kexec_load);
}

static void
kexec(const char *path) {
    char cmdline[1024];
    if (!path)
        return;
    build_cmdline(cmdline, 1024);
    ui_print("Booting '%s', cmdline='%s'\n", path, cmdline);
    ensure_path_mounted(path);
    do_kexec(path, cmdline);
    ensure_path_unmounted(path);
}

extern void
show_mount_usb_storage_menu(void);

static int
execute_action(int action, const char *path) {
    switch (action) {
    case ACTION_BOOT: {
        kexec(path);
        ui_print("Kexec failed\n");
        break; }
    case ACTION_MASS_STORAGE:
        ensure_path_unmounted(SDCARD_ROOT);
        show_mount_usb_storage_menu();
        break;
    case ACTION_RELOAD:
        poweroff = -1;
        return 1;
    case ACTION_REBOOT:
        poweroff = 0;
        return 1;
    case ACTION_POWEROFF:
        poweroff = 1;
        return 1;
    default:
        ui_print("Unknown action\n");
        break;
    }
    return 0;
}

static void
prompt_and_wait() {
    char *headers[] = {
        "CWM-based DroidBoot",
        "",
        0
    };
    char **labels = bootmenu_labels;
    const struct bootmenu_item *items = bootmenu_items;

    for (;;) {
        const struct bootmenu_item *item;

        ui_reset_progress();

        allow_display_toggle = 1;
        int chosen_item = get_menu_selection(headers, labels, 0, 0);
        allow_display_toggle = 0;

        printf("Chosen item: %d\n", chosen_item);

        if (chosen_item < 0) {
            labels = bootmenu_labels;
            items = bootmenu_items;
            continue;
        }

        item = &items[chosen_item];
        switch (item->type) {
        case TYPE_FILE_LIST: {
            char path[PATH_MAX];
            int ret;
            strlcpy(path, item->path, PATH_MAX);
            ensure_path_mounted(item->path);
            do {
                ret = select_file("Select image to boot:",
                                                        path, path, PATH_MAX);
                if (strlen(path) < strlen(item->path))
                    strlcpy(path, item->path, PATH_MAX);
            } while (ret > 0);
            if (!ret)
                execute_action(item->action, path);
            ensure_path_unmounted(item->path);
            break; }
        case TYPE_SUBMENU:
            labels = item->labels;
            items = item->items;
            break;
        case TYPE_ACTION:
            if (execute_action(item->action, item->path))
                return;
            break;
        case TYPE_TUNABLE: {
            struct tunable *t = (struct tunable *)item->path;
            t->value = item->action;
            labels = bootmenu_labels;
            items = bootmenu_items;
            settings_modified = 1;
            break;}
        default:
            /* Separator */
            break;
        }
    }
}

static char *parse_line(char *line, char **save_ptr) {
    char *ptr;
    int quotation = 0;
    int chars = 0;

    if (!line)
        line = *save_ptr;

    if (!line)
        return 0;

    for (ptr = line; *ptr; ++ptr) {
        switch (*ptr) {
        case '\x22':
            ++chars;
            if (!quotation)
                line = ptr + 1;
            quotation ^= 1;
            *ptr = 0;
            break;
        case ' ':
        case '\t':
        case '\n':
            if (!chars)
                line = ptr + 1;
            if (quotation || !chars)
                continue;
            *ptr = 0;
            *save_ptr = ptr + 1;
            return line;
        case '#':
            if (quotation)
                continue;
            *save_ptr = 0;
            return 0;
        default:
            ++chars;
        }
    }

    if (!chars)
        line = 0;
    *save_ptr = 0;
    return line;
}

static int
check_keychords(void) {
    int i;
    for (i = 0; i < num_key_chords; ++i) {
        int k;
        struct key_chord *c = &key_chord_table[i];
        if (!c->num_keys)
            continue;
        for (k = 0; k < c->num_keys; ++k)
            if (!ui_key_pressed(c->keys[k]))
                break;
        if (k == c->num_keys)
            return i;
    }
    return -1;
}

static void
add_key_chord(unsigned long index, unsigned long code) {
    struct key_chord *c = &key_chord_table[index];
    if (c->num_keys >= KEY_CHORD_MAX_KEYS)
        return;
    c->keys[c->num_keys++] = code;
}

static void
parse_key_chords(const char *path, int accept_zero) {
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    printf("Parsing %s\n", path);
    while(fgets(buf, 1024, f)) {
        char *tmp = buf, *save;
        unsigned long code;
        if (num_key_chords >= KEY_CHORD_MAX)
            break;
        tmp = parse_line(buf, &save);
        if (!tmp)
            continue;
        do {
            code = strtoul(tmp, 0, 0);
            printf("Adding key %lu to chord %d\n", code, num_key_chords);
            add_key_chord(num_key_chords, code);
        } while ((tmp = parse_line(0, &save)));
        ++num_key_chords;
    }
    fclose(f);
}

static void
add_bootable(const char *label, const char *path) {
    struct bootable *b;

    printf("Adding bootable (label = '%s', path = '%s')\n", label, path);

    if (bootable_allocated == bootable_count) {
        bootable_allocated *= 2;
        bootables = realloc(bootables, bootable_allocated*sizeof(*bootables));
    }

    b = &bootables[bootable_count];
    b->label = strdup(label);
    b->path = strdup(path);

    ++bootable_count;
}

static void
parse_bootables(const char *path) {
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    printf("Parsing %s\n", path);
    while(fgets(buf, 1024, f)) {
        char *label, *path, *save;
        label = parse_line(buf, &save);
        if (!label)
            continue;
        path = parse_line(0, &save);
        if (!path)
            continue;
        add_bootable(label, path);
    }
    fclose(f);
}

static void
add_tunable(const char *label, const char *name,
                                            char **values, int num_values) {
    struct tunable *t;
    int i;

    printf("Adding tunable (label = '%s', name = '%s', values = { ", label, name);
    for (i = 0; i < num_values; ++i) {
        printf("'%s'", values[i]);
        if (i < num_values - 1)
            printf(", ");
    }
    printf(" })\n");

    if (tunable_allocated == tunable_count) {
        tunable_allocated *= 2;
        tunables = realloc(tunables, tunable_allocated*sizeof(*tunables));
    }

    t = &tunables[tunable_count];
    t->label = strdup(label);
    t->name = strdup(name);
    t->values = values;
    t->num_values = num_values;
    t->value = 0;

    ++tunable_count;
}

static void
parse_tunables(const char *path) {
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    printf("Parsing %s\n", path);
    while(fgets(buf, 1024, f)) {
        char *label, *name, *tmp, *save;
        char **values;
        unsigned int num_values = 0;
        unsigned int allocated_values = 2;
        label = parse_line(buf, &save);
        if (!label)
            continue;
        name = parse_line(0, &save);
        if (!name)
            continue;
        values = malloc(allocated_values*sizeof(char *));
        while ((tmp = parse_line(0, &save))) {
            if (num_values == allocated_values) {
                allocated_values *= 2;
                values = realloc(values, allocated_values*sizeof(char *));
            }
            values[num_values++] = strdup(tmp);
        }
        add_tunable(label, name, values, num_values);
    }
    fclose(f);
}

static void
set_tunable(const char *name, const char *value) {
    int i, j;
    for (i = 0; i < tunable_count; ++i) {
        if (strcasecmp(name, tunables[i].name))
            continue;

        for (j = 0; j < tunables[i].num_values; ++j) {
            if (strcasecmp(value, tunables[i].values[j]))
                continue;

            tunables[i].value = j;
            return;
        }

        if (!tunables[i].num_values) {
            tunables[i].value = strtoul(value, 0, 0);
            return;
        }
    }
}

static void
parse_settings(const char *path) {
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    printf("Parsing %s\n", path);
    while(fgets(buf, 1024, f)) {
        char *name, *value, *save;
        name = parse_line(buf, &save);
        if (!name)
            continue;
        value = parse_line(0, &save);
        if (!value)
            continue;
        set_tunable(name, value);
    }
    fclose(f);
}

static void
save_settings(const char *path) {
    char buf[1024];
    strlcpy(buf, path, 1024);
    strlcat(buf, ".old", 1024);
    rename(path, buf);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    int i;
    struct tunable *t = tunables;
    for (i = 0; i < tunable_count; ++i, ++t) {
        if (t->num_values)
            fprintf(f, "\"%s\"\t\"%s\"\n", t->name, t->values[t->value]);
        else
            fprintf(f, "\"%s\"\t%d\n", t->name, t->value);
    }
    fclose(f);
}

static void
build_tunable_menu(struct tunable *tunable,
                            struct bootmenu_item **items, char ***labels) {
    int i, num_values = tunable->num_values;
    if (!num_values)
        num_values = 2;
    *items = malloc(num_values*sizeof(**items));
    *labels = malloc((num_values + 1)*sizeof(**labels));
    for (i = 0; i < tunable->num_values; ++i) {
        (*items)[i].type = TYPE_TUNABLE;
        (*items)[i].action = i;
        (*items)[i].path = (void *)tunable;
        if (tunable->values[i][0] == 0)
            (*labels)[i] = "(none)";
        else
            (*labels)[i] = tunable->values[i];
    }
    if (!tunable->num_values) {
        (*items)[0].type = TYPE_TUNABLE;
        (*items)[0].action = 0;
        (*items)[0].path = (void *)tunable;
        (*labels)[0] = "(off)";
        (*items)[1].type = TYPE_TUNABLE;
        (*items)[1].action = 1;
        (*items)[1].path = (void *)tunable;
        (*labels)[1] = "(on)";
    }
    (*labels)[num_values] = 0;
}

static void
build_menu(void) {
    int i, j = 0;
    int menu_items = bootable_count + 1 + tunable_count + NELEM(static_menu_items);
    paths = malloc(bootable_count*sizeof(*paths));
    bootmenu_items = malloc(menu_items*sizeof(*bootmenu_items));
    bootmenu_labels = malloc(menu_items*sizeof(*bootmenu_labels));
    for (i = 0; i < bootable_count; ++i, ++j) {
        paths[i] = bootables[i].path;
        bootmenu_items[j].type = TYPE_ACTION;
        bootmenu_items[j].action = ACTION_BOOT;
        bootmenu_items[j].path = bootables[i].path;
        bootmenu_labels[j] = bootables[i].label;
    }
    bootmenu_items[j].type = TYPE_SEPARATOR;
    bootmenu_labels[j] = "- - - Tunables - - - -";
    ++j;
    for (i = 0; i < tunable_count; ++i, ++j) {
        bootmenu_items[j].type = TYPE_SUBMENU;
        build_tunable_menu(&tunables[i],
                        &bootmenu_items[j].items, &bootmenu_items[j].labels);
        bootmenu_labels[j] = tunables[i].label;
    }
    memcpy(&bootmenu_items[j], static_menu_items,
                        NELEM(static_menu_items)*sizeof(*static_menu_items));
    memcpy(&bootmenu_labels[j], static_menu_labels,
                        NELEM(static_menu_labels)*sizeof(*static_menu_labels));
}

int
droidboot_main(int argc, char **argv) {
    time_t start = time(NULL);
    int ret;

    // If these fail, there's not really anywhere to complain...
    printf("Starting DroidBoot on %s\n", ctime(&start));

    ui_init();
    ui_print("DroidBoot...\n");

    // we are starting up in user initiated recovery here
    // let's set up some default options
    ui_set_show_text(1);
    ui_set_background(BACKGROUND_ICON_NONE);

    // allocate initial memory for parsed configuration
    bootable_allocated = 2;
    bootables = malloc(bootable_allocated*sizeof(*bootables));
    tunable_allocated = 2;
    tunables = malloc(tunable_allocated*sizeof(*tunables));

    // Initialize default configuration
    parse_key_chords("/etc/chords", 1);
    parse_bootables("/etc/bootables");
    parse_tunables("/etc/tunables");

    load_volume_table();
    process_volumes();

    // Initialize local configuration
    ensure_path_mounted("/boot");
    parse_key_chords("/boot/chords", 0);
    parse_bootables("/boot/bootables");
    parse_tunables("/boot/tunables");

    // Initialize configurable settings
    parse_settings("/boot/settings");

    build_menu();

    sleep(key_chord_timeout);

    // Check boot hotkeys
    ret = check_keychords();

    switch (ret) {
    case -1:
        if (!paths[0]) {
            ui_print("No default boot image defined, entering menu...\n");
            break;
        }
        ui_print("Booting default...\n");
        execute_action(ACTION_BOOT, paths[0]);
        ui_print("Boot failed, entering menu.\n");
        break;
    case 0:
        ui_print("Entering boot menu...\n");
        break;
    default:
        ui_print("Booting position %d...\n", ret);
        execute_action(ACTION_BOOT, paths[ret]);
        ui_print("Boot failed, entering menu...\n");
    }

    prompt_and_wait();

    // Otherwise, get ready to boot the main system...
    if (!poweroff)
        ui_print("Rebooting...\n");
    else if (poweroff > 0)
        ui_print("Shutting down...\n");
    if (settings_modified)
        save_settings("/boot/settings");
    sync();
    if (poweroff >= 0)
        reboot((!poweroff) ? RB_AUTOBOOT : RB_POWER_OFF);
    return EXIT_SUCCESS;
}
