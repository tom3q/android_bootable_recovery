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

static const char *SDCARD_ROOT = "/sdcard";
static int allow_display_toggle = 1;
static int poweroff = 0;

static char**
prepend_title(char** headers) {
    char* title[] = { EXPAND(RECOVERY_VERSION),
                      "",
                      NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

extern int
get_menu_selection(char** headers, char** items, int menu_only,
                   int initial_selection);

static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int
sdcard_directory(const char* path) {
    ensure_path_mounted(SDCARD_ROOT);

    const char* MENU_HEADERS[] = { "Choose a package to install:",
                                   path,
                                   "",
                                   NULL };
    DIR* d;
    struct dirent* de;
    d = opendir(path);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path, strerror(errno));
        ensure_path_unmounted(SDCARD_ROOT);
        return 0;
    }

    char** headers = prepend_title(MENU_HEADERS);

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
                   strncasecmp(de->d_name + (name_len-4), ".zip", 4) == 0) {
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

    int result;
    int chosen_item = 0;
    do {
        chosen_item = get_menu_selection(headers, zips, 1, chosen_item);

        char* item = zips[chosen_item];
        int item_len = strlen(item);
        if (chosen_item == 0) {          // item 0 is always "../"
            // go up but continue browsing (if the caller is sdcard_directory)
            result = -1;
            break;
        } else if (item[item_len-1] == '/') {
            // recurse down into a subdirectory
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);
            new_path[strlen(new_path)-1] = '\0';  // truncate the trailing '/'
            result = sdcard_directory(new_path);
            if (result >= 0) break;
        } else {
            // selected a zip file:  attempt to install it, and return
            // the status to the caller.
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);

            ui_print("\n-- Install %s ...\n", path);
            set_sdcard_update_bootloader_message();
            char* copy = copy_sideloaded_package(new_path);
            ensure_path_unmounted(SDCARD_ROOT);
            if (copy) {
                result = install_package(copy);
                free(copy);
            } else {
                result = INSTALL_ERROR;
            }
            break;
        }
    } while (true);

    int i;
    for (i = 0; i < z_size; ++i) free(zips[i]);
    free(zips);
    free(headers);

    ensure_path_unmounted(SDCARD_ROOT);
    return result;
}


static void
prompt_and_wait() {
    char** headers = prepend_title((const char**)MENU_HEADERS);

    for (;;) {
        ui_reset_progress();

        allow_display_toggle = 1;
        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0, 0);
        allow_display_toggle = 0;

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        switch (chosen_item) {
            case ITEM_REBOOT:
                poweroff=0;
                return;

            case ITEM_WIPE_DATA:
//                 wipe_data(ui_text_visible());
                if (!ui_text_visible()) return;
                break;

            case ITEM_WIPE_CACHE:
//                 if (confirm_selection("Confirm wipe?", "Yes - Wipe Cache"))
//                 {
//                     ui_print("\n-- Wiping cache...\n");
//                     erase_volume("/cache");
//                     ui_print("Cache wipe complete.\n");
                    if (!ui_text_visible()) return;
//                 }
                break;

            case ITEM_APPLY_SDCARD:
//                 if (confirm_selection("Confirm install?", "Yes - Install /sdcard/update.zip"))
//                 {
//                     ui_print("\n-- Install from sdcard...\n");
//                     int status = install_package(SDCARD_PACKAGE_FILE);
//                     if (status != INSTALL_SUCCESS) {
//                         ui_set_background(BACKGROUND_ICON_ERROR);
//                         ui_print("Installation aborted.\n");
//                     } else if (!ui_text_visible()) {
                        return;  // reboot if logs aren't visible
//                     } else {
//                         ui_print("\nInstall from sdcard complete.\n");
//                     }
//                 }
                break;
            case ITEM_INSTALL_ZIP:
                show_install_update_menu();
                break;
            case ITEM_NANDROID:
                show_nandroid_menu();
                break;
            case ITEM_PARTITION:
                show_partition_menu();
                break;
            case ITEM_ADVANCED:
                show_advanced_menu();
                break;
            case ITEM_POWEROFF:
                poweroff=1;
                return;
        }
    }
}

int
droidboot_main(int argc, char **argv) {
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    printf("Starting DroidBoot on %s", ctime(&start));

    ui_init();
    ui_print(EXPAND(RECOVERY_VERSION)"\n");
    load_volume_table();
    process_volumes();

    LOGI("Checking for extendedcommand...\n");
    // we are starting up in user initiated recovery here
    // let's set up some default options
    ui_set_show_text(1);
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);

    prompt_and_wait();

    // Otherwise, get ready to boot the main system...
    if(!poweroff)
        ui_print("Rebooting...\n");
    else
        ui_print("Shutting down...\n");
    sync();
    reboot((!poweroff) ? RB_AUTOBOOT : RB_POWER_OFF);
    return EXIT_SUCCESS;
}
