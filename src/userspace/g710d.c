#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <poll.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define VENDOR_ID  0x046d
#define PRODUCT_ID 0xc24d

int current_profile = 1;
char sysfs_led_path[512] = "";
volatile int keep_running = 1;
struct libevdev_uinput *macro_uidev = NULL;

void signal_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

int find_sysfs_path() {
    DIR *d;
    struct dirent *dir;
    d = opendir("/sys/bus/hid/devices");
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, "046D:C24D")) {
                snprintf(sysfs_led_path, sizeof(sysfs_led_path), 
                         "/sys/bus/hid/devices/%s/logitech-g710/led_macro", dir->d_name);
                if (access(sysfs_led_path, F_OK) == 0) {
                    closedir(d);
                    return 0;
                }
            }
        }
        closedir(d);
    }
    return -1;
}

void set_profile_led(int profile, int mr_on) {
    if (sysfs_led_path[0] == '\0') return;
    int fd = open(sysfs_led_path, O_WRONLY);
    if (fd >= 0) {
        char val[4];
        int mask = (1 << (profile - 1));
        if (mr_on) mask |= 8;
        snprintf(val, sizeof(val), "%d", mask);
        write(fd, val, strlen(val));
        close(fd);
    }
}

void send_key(struct libevdev_uinput *uidev, int code, int value) {
    libevdev_uinput_write_event(uidev, EV_KEY, code, value);
    libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
}

#define MAX_MACROS 100
#define MAX_KEYS_PER_MACRO 32

typedef struct {
    int profile;
    int g_key;
    int keys[MAX_KEYS_PER_MACRO];
    int key_count;
} Macro;

Macro macros[MAX_MACROS];
int macro_count = 0;

int parse_profile_or_gkey(const char *token, char prefix, int min, int max, int *out) {
    char *endptr;
    long value;

    if (!token || token[0] != prefix) return -1;
    errno = 0;
    value = strtol(token + 1, &endptr, 10);
    if (errno != 0 || endptr == token + 1 || *endptr != '\0') return -1;
    if (value < min || value > max) return -1;
    *out = (int)value;
    return 0;
}

void load_config() {
    FILE *f = fopen("/etc/g710d.conf", "r");
    if (!f) {
        printf("No config file found at /etc/g710d.conf.\n");
        return;
    }

    char line[1024];
    macro_count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char p_str[16], g_str[16];
        if (sscanf(line, "%s %s", p_str, g_str) != 2) continue;
        if (macro_count >= MAX_MACROS) break;

        Macro *m = &macros[macro_count];
        int profile = 0;
        int gkey = 0;
        if (parse_profile_or_gkey(p_str, 'P', 1, 3, &profile) < 0 ||
            parse_profile_or_gkey(g_str, 'G', 1, 6, &gkey) < 0) {
            continue;
        }
        m->profile = profile;
        m->g_key = KEY_F17 + gkey - 1;
        m->key_count = 0;

        char *line_copy = strdup(line);
        if (!line_copy) continue;
        strtok(line_copy, " \t\n\r"); // P
        strtok(NULL, " \t\n\r");       // G
        char *token;
        while ((token = strtok(NULL, " \t\n\r")) != NULL) {
            if (strcmp(token, "RELEASE") == 0) {
                if (m->key_count < MAX_KEYS_PER_MACRO) m->keys[m->key_count++] = -2;
                continue;
            }
            if (strcmp(token, "HOLD") == 0) {
                if (m->key_count < MAX_KEYS_PER_MACRO) m->keys[m->key_count++] = -3;
                continue;
            }
            int code = libevdev_event_code_from_name(EV_KEY, token);
            if (code != -1 && m->key_count < MAX_KEYS_PER_MACRO) {
                m->keys[m->key_count++] = code;
            }
        }
        free(line_copy);
        if (m->key_count > 0) macro_count++;
    }
    fclose(f);
    printf("Total macros loaded: %d\n", macro_count);
}

int is_modifier(int code) {
    return (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
            code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
            code == KEY_LEFTALT || code == KEY_RIGHTALT ||
            code == KEY_LEFTMETA || code == KEY_RIGHTMETA);
}

void run_macro(struct libevdev_uinput *uidev, int g_key) {
    if (!uidev) return;

    for (int i = 0; i < macro_count; i++) {
        if (macros[i].profile == current_profile && macros[i].g_key == g_key) {
            printf("Executing macro for G%d (Profile %d)\n", g_key - KEY_F17 + 1, current_profile);
            
            int held_keys[MAX_KEYS_PER_MACRO];
            int held_is_manual[MAX_KEYS_PER_MACRO];
            int held_count = 0;
            int force_hold = 0;

            for (int j = 0; j < macros[i].key_count; j++) {
                int code = macros[i].keys[j];

                if (code == -3) { // HOLD prefix
                    force_hold = 1;
                    continue;
                }

                if (code == -2) { // Explicit RELEASE
                    for (int k = held_count - 1; k >= 0; k--) {
                        send_key(uidev, held_keys[k], 0);
                        usleep(5000);
                    }
                    held_count = 0;
                } else if (is_modifier(code) || force_hold) {
                    send_key(uidev, code, 1);
                    if (held_count < MAX_KEYS_PER_MACRO) {
                        held_keys[held_count] = code;
                        held_is_manual[held_count] = force_hold;
                        held_count++;
                    } else {
                        fprintf(stderr, "Too many held keys in macro, forcing release.\n");
                        send_key(uidev, code, 0);
                    }
                    force_hold = 0;
                    usleep(5000);
                } else {
                    send_key(uidev, code, 1);
                    usleep(10000);
                    send_key(uidev, code, 0);
                    usleep(10000);
                    
                    // Smart Auto-release (only for automatic modifiers)
                    if (held_count > 0 && j + 1 < macros[i].key_count && 
                        !is_modifier(macros[i].keys[j+1]) && 
                        macros[i].keys[j+1] != -2 && macros[i].keys[j+1] != -3) {
                        
                        int new_count = 0;
                        for (int k = 0; k < held_count; k++) {
                            if (!held_is_manual[k]) { // Release automatic modifiers
                                send_key(uidev, held_keys[k], 0);
                            } else { // Keep manual HOLDs
                                held_keys[new_count] = held_keys[k];
                                held_is_manual[new_count] = 1;
                                new_count++;
                            }
                        }
                        held_count = new_count;
                        usleep(5000);
                    }
                }
            }

            for (int j = held_count - 1; j >= 0; j--) {
                send_key(uidev, held_keys[j], 0);
                usleep(5000);
            }
            return;
        }
    }
    printf("No macro defined for G%d in Profile %d\n", g_key - KEY_F17 + 1, current_profile);
}

#define MAX_DEVICES 8
struct device_info {
    int fd;
    struct libevdev *dev;
};
struct device_info devices[MAX_DEVICES];
int device_count = 0;

int add_device(const char *path) {
    if (device_count >= MAX_DEVICES) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) { close(fd); return -1; }

    if (libevdev_get_id_vendor(dev) == VENDOR_ID &&
        libevdev_get_id_product(dev) == PRODUCT_ID) {
        int has_macro_keys = 1;
        const int macro_keys[] = {KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22};
        for (size_t i = 0; i < sizeof(macro_keys) / sizeof(macro_keys[0]); i++) {
            if (!libevdev_has_event_code(dev, EV_KEY, macro_keys[i])) {
                has_macro_keys = 0;
                break;
            }
        }
        if (!has_macro_keys) {
            libevdev_free(dev);
            close(fd);
            return -1;
        }

        const char *phys = libevdev_get_phys(dev);
        int has_typing_keys = libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
                              libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
                              libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);
        int grab_safe = !has_typing_keys || (phys && strstr(phys, "/input1"));

        printf("Adding G710+ macro-capable device: %s (%s) [Phys=%s] [%s]\n",
               path,
               libevdev_get_name(dev),
               phys ? phys : "N/A",
               grab_safe ? "grab=yes" : "grab=no");

        if (grab_safe) {
            rc = libevdev_grab(dev, LIBEVDEV_GRAB);
            if (rc < 0) {
                fprintf(stderr, "Warning: Failed to grab macro interface (%s)\n", strerror(-rc));
            } else {
                printf("Successfully grabbed macro interface for exclusive access.\n");
            }
        }

        if (macro_uidev == NULL) {
            struct libevdev_uinput *uidev;
            struct libevdev *vdev = libevdev_new();
            if (!vdev) {
                libevdev_free(dev);
                close(fd);
                return -1;
            }
            libevdev_set_name(vdev, "G710+ Virtual Macro Keyboard");
            libevdev_enable_event_type(vdev, EV_KEY);
            for (int i = 0; i <= KEY_MAX; i++) libevdev_enable_event_code(vdev, EV_KEY, i, NULL);
            rc = libevdev_uinput_create_from_device(vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
            libevdev_free(vdev);
            if (rc < 0) {
                fprintf(stderr, "Failed to create uinput device: %s\n", strerror(-rc));
                libevdev_free(dev);
                close(fd);
                return -1;
            }
            macro_uidev = uidev;
        }

        devices[device_count].fd = fd;
        devices[device_count].dev = dev;
        device_count++;
        return 0;
    }
    libevdev_free(dev);
    close(fd);
    return -1;
}

void find_all_g710_devices() {
    char path[256];
    for (int i = 0; i < 64; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        add_device(path);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    load_config();
    find_all_g710_devices();
    if (device_count == 0) {
        fprintf(stderr, "Could not find any G710+ keyboard devices.\n");
        return 1;
    }
    if (find_sysfs_path() < 0) {
        fprintf(stderr, "Could not find sysfs path for LEDs.\n");
    } else {
        printf("Found sysfs LED path: %s\n", sysfs_led_path);
        set_profile_led(current_profile, 0);
    }
    struct pollfd fds[MAX_DEVICES];
    for (int i = 0; i < device_count; i++) {
        fds[i].fd = devices[i].fd;
        fds[i].events = POLLIN;
    }
    printf("Daemon started. Listening on %d devices... (Ctrl+C to quit)\n", device_count);
    while (keep_running) {
        int ret = poll(fds, device_count, 100);
        if (ret > 0) {
            for (int i = 0; i < device_count; i++) {
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    fprintf(stderr, "Input device error on fd=%d, revents=0x%x\n", fds[i].fd, fds[i].revents);
                    continue;
                }
                if (fds[i].revents & POLLIN) {
                    struct input_event ev;
                    int status = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                    while (status == LIBEVDEV_READ_STATUS_SUCCESS || status == LIBEVDEV_READ_STATUS_SYNC) {
                        if (status == LIBEVDEV_READ_STATUS_SYNC) {
                            status = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                            continue;
                        }
                        if (ev.type == EV_KEY && ev.value == 1) {
                            if (ev.code >= KEY_F13 && ev.code <= KEY_F15) {
                                current_profile = ev.code - KEY_F13 + 1;
                                printf("Switched to Profile %d\n", current_profile);
                                set_profile_led(current_profile, 0);
                            } else if (ev.code == KEY_F16) {
                                printf("MR pressed: Reloading configuration...\n");
                                set_profile_led(current_profile, 1);
                                load_config();
                                usleep(200000);
                                set_profile_led(current_profile, 0);
                            } else if (ev.code >= KEY_F17 && ev.code <= KEY_F22) {
                                run_macro(macro_uidev, ev.code);
                            }
                        }
                        status = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                    }
                }
            }
        }
    }
    printf("\nCleaning up and exiting...\n");
    if (macro_uidev) {
        libevdev_uinput_destroy(macro_uidev);
    }
    for (int i = 0; i < device_count; i++) {
        libevdev_free(devices[i].dev);
        close(devices[i].fd);
    }
    return 0;
}
