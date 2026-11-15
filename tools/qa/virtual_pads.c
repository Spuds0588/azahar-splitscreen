// virtual_pads - QA-only virtual Xbox 360 pads, for testing pad-driven peek.
//
// There is no way to press a physical controller from a script, so this makes
// the same device the couch uses: a uinput gamepad with the exact USB identity
// of the attached pad (bus 0003, vendor 045e, product 028e, version 0114). SDL
// therefore takes the identical code path it takes for a real one - it maps it
// through the same game controller entry, so the shoulder is `leftshoulder`,
// not a guessed button number.
//
// It needs root because /dev/uinput is root-only; it is started once per test
// and driven through a FIFO, so nothing else on the machine changes hands:
//
//   sudo ./virtual_pads /tmp/pads.ctl 2
//   echo "0 ld" > /tmp/pads.ctl     # pad 0 left shoulder down
//   echo "0 lu" > /tmp/pads.ctl     # pad 0 left shoulder up
//   echo "0 rx -32768" > /tmp/pads.ctl   # right stick fully left (first byte is signed)
//   echo "0 ry 32767" > /tmp/pads.ctl    # right stick fully down
//   echo "0 ad" > /tmp/pads.ctl     # pad 0 A button down
//   echo "0 au" > /tmp/pads.ctl     # pad 0 A button up
//   echo "0 remove" > /tmp/pads.ctl # unplug pad 0
//   echo quit > /tmp/pads.ctl       # tear the pads down
//
// It chmods the event nodes it creates to 0666, because a uinput device is not
// tied to a seat, so logind's uaccess ACL that makes the real pad readable does
// not cover it. Nothing else is altered.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PADS 4

static int fds[MAX_PADS];
static int live[MAX_PADS];
static char nodes[MAX_PADS][64];

/// Collect /dev/input/<prefix><n> numbers, so a node created by this program can
/// be told apart from the ones that were already there.
static int list_input(const char* prefix, int* numbers, int max) {
    DIR* dir = opendir("/dev/input");
    if (dir == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && count < max) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        numbers[count++] = atoi(entry->d_name + strlen(prefix));
    }
    closedir(dir);
    return count;
}

static int contains(const int* numbers, int count, int value) {
    for (int i = 0; i < count; ++i) {
        if (numbers[i] == value) {
            return 1;
        }
    }
    return 0;
}

/// The node that appeared since `before` was taken (taken before UI_DEV_CREATE,
/// so the new device is the difference).
static int find_new_node(const char* prefix, const int* before, int n_before) {
    int after[128];
    usleep(200 * 1000);
    const int n_after = list_input(prefix, after, 128);
    for (int i = 0; i < n_after; ++i) {
        if (!contains(before, n_before, after[i])) {
            return after[i];
        }
    }
    return -1;
}

static void emit(int fd, int type, int code, int value) {
    struct input_event event;
    memset(&event, 0, sizeof event);
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(fd, &event, sizeof event) < 0) {
        perror("write");
    }
}

static void emit_key(int fd, int code, int value) {
    emit(fd, EV_KEY, code, value);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void emit_abs(int fd, int code, int value) {
    emit(fd, EV_ABS, code, value);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int make_pad(int index) {
    int event_before[128];
    int js_before[128];
    const int n_event_before = list_input("event", event_before, 128);
    const int n_js_before = list_input("js", js_before, 128);

    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "virtual_pads: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    const int buttons[] = {BTN_A,      BTN_B,      BTN_X,       BTN_Y,
                           BTN_TL,     BTN_TR,     BTN_SELECT,  BTN_START,
                           BTN_MODE,   BTN_THUMBL, BTN_THUMBR};
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        ioctl(fd, UI_SET_KEYBIT, buttons[i]);
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    // Sticks span the real pad's range so injected values need no rescaling;
    // triggers and the hat keep theirs.
    struct {
        int code, min, max;
    } axes[] = {{ABS_X, -32768, 32767}, {ABS_Y, -32768, 32767}, {ABS_RX, -32768, 32767},
                {ABS_RY, -32768, 32767}, {ABS_Z, 0, 255},       {ABS_RZ, 0, 255},
                {ABS_HAT0X, -1, 1},      {ABS_HAT0Y, -1, 1}};
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; ++i) {
        ioctl(fd, UI_SET_ABSBIT, axes[i].code);
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof abs_setup);
        abs_setup.code = axes[i].code;
        abs_setup.absinfo.minimum = axes[i].min;
        abs_setup.absinfo.maximum = axes[i].max;
        ioctl(fd, UI_ABS_SETUP, &abs_setup);
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e;
    setup.id.product = 0x028e;
    setup.id.version = 0x0114;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Microsoft X-Box 360 pad");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "virtual_pads: cannot create pad: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    const int event = find_new_node("event", event_before, n_event_before);
    const int js = find_new_node("js", js_before, n_js_before);
    if (event >= 0) {
        snprintf(nodes[index], sizeof nodes[index], "/dev/input/event%d", event);
        chmod(nodes[index], 0666);
    } else {
        nodes[index][0] = '\0';
    }
    if (js >= 0) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/js%d", js);
        chmod(path, 0666);
    }
    printf("virtual_pads: pad %d on %s\n", index, nodes[index]);
    fflush(stdout);
    return fd;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: virtual_pads <fifo> [pad count]\n");
        return 2;
    }
    const int wanted = argc > 2 ? atoi(argv[2]) : 2;
    memset(fds, -1, sizeof fds);
    for (int i = 0; i < wanted && i < MAX_PADS; ++i) {
        fds[i] = make_pad(i);
        live[i] = fds[i] >= 0;
    }

    const int ctl = open(argv[1], O_RDWR);
    if (ctl < 0) {
        fprintf(stderr, "virtual_pads: cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    printf("virtual_pads: ready\n");
    fflush(stdout);

    char buffer[512];
    size_t used = 0;
    char chunk[256];
    ssize_t got;
    while ((got = read(ctl, chunk, sizeof chunk)) > 0) {
        if (used + (size_t)got >= sizeof buffer) {
            used = 0;
        }
        memcpy(buffer + used, chunk, (size_t)got);
        used += (size_t)got;
        char* start = buffer;
        char* newline;
        while ((newline = memchr(start, '\n', used - (size_t)(start - buffer))) != NULL) {
            *newline = '\0';
            int pad = 0;
            int number = 0;
            char action[16] = {0};
            if (sscanf(start, "%d %15s %d", &pad, action, &number) >= 2 && pad >= 0 &&
                pad < MAX_PADS) {
                if (live[pad] && fds[pad] >= 0) {
                    if (strcmp(action, "ld") == 0) {
                        emit_key(fds[pad], BTN_TL, 1);
                    } else if (strcmp(action, "lu") == 0) {
                        emit_key(fds[pad], BTN_TL, 0);
                    } else if (strcmp(action, "rd") == 0) {
                        emit_key(fds[pad], BTN_TR, 1);
                    } else if (strcmp(action, "ru") == 0) {
                        emit_key(fds[pad], BTN_TR, 0);
                    } else if (strcmp(action, "rx") == 0) {
                        emit_abs(fds[pad], ABS_RX, number);
                    } else if (strcmp(action, "ry") == 0) {
                        emit_abs(fds[pad], ABS_RY, number);
                    } else if (strcmp(action, "ad") == 0) {
                        emit_key(fds[pad], BTN_A, 1);
                    } else if (strcmp(action, "au") == 0) {
                        emit_key(fds[pad], BTN_A, 0);
                    } else if (strcmp(action, "remove") == 0) {
                        emit_key(fds[pad], BTN_TL, 0);
                        ioctl(fds[pad], UI_DEV_DESTROY);
                        close(fds[pad]);
                        fds[pad] = -1;
                        live[pad] = 0;
                        printf("virtual_pads: pad %d removed\n", pad);
                        fflush(stdout);
                    }
                }
            } else if (strcmp(start, "quit") == 0) {
                for (int i = 0; i < MAX_PADS; ++i) {
                    if (fds[i] >= 0) {
                        ioctl(fds[i], UI_DEV_DESTROY);
                        close(fds[i]);
                    }
                }
                printf("virtual_pads: gone\n");
                fflush(stdout);
                return 0;
            }
            start = newline + 1;
        }
        used -= (size_t)(start - buffer);
        memmove(buffer, start, used);
    }
    return 0;
}
