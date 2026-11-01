/* Steam's hidraw reader cannot be EVIOCGRABbed. Drain its controller
 * reports normally, but replace desktop-owned input with a released state.
 * Only the Steam client executable and Valve 2026 controller are affected;
 * inherited LD_PRELOAD in games and helpers is inert. No polling thread. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Games inherit the preload in older Steam Runtime containers. Use the
 * original glibc symbol versions rather than requiring the host's 2.34+. */
#if defined(__x86_64__)
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver pthread_once,pthread_once@GLIBC_2.2.5");
#elif defined(__i386__)
__asm__(".symver dlsym,dlsym@GLIBC_2.0");
__asm__(".symver pthread_once,pthread_once@GLIBC_2.0");
#endif

static ssize_t (*next_read)(int, void *, size_t);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int is_steam;
static char owner_path[4096];

static void init(void)
{
    next_read = dlsym(RTLD_NEXT, "read");
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0)
        return;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    is_steam = base && !strcmp(base + 1, "steam");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || snprintf(owner_path, sizeof owner_path, "%s/hypr-osk-pad-owner", runtime) >= (int)sizeof owner_path)
        is_steam = 0;
}

static int desktop_owns_pad(void)
{
    /* Reopen on every hardware report: plugin reload replaces the file.
     * Missing/malformed state or a dead compositor fails open for Steam. */
    int fd = open(owner_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return 0;
    char state[64];
    ssize_t n = next_read(fd, state, sizeof state - 1);
    close(fd);
    if (n <= 0)
        return 0;
    state[n] = 0;
    unsigned long pid = 0;
    const char *p = state;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p++ - '0');
        if (pid > INT_MAX)
            return 0;
    }
    return pid > 1 && !strcmp(p, " 1\n") && kill((pid_t)pid, 0) == 0;
}

static int controller_id(unsigned vendor, unsigned product)
{
    return vendor == 0x28de && (product == 0x1302 || product == 0x1304);
}

ssize_t read(int fd, void *buf, size_t count)
{
    pthread_once(&once, init);
    if (!next_read) {
        errno = ENOSYS;
        return -1;
    }
    ssize_t n = next_read(fd, buf, count);
    int saved_errno = errno;
    if (n > 0 && is_steam) {
        struct hidraw_devinfo raw = {0};
        struct input_id ev = {0};
        /* Check the device before opening the state file: Steam's ordinary
         * disk/socket reads and other HID peripherals pass straight through. */
        if (ioctl(fd, HIDIOCGRAWINFO, &raw) == 0 && controller_id((unsigned short)raw.vendor, (unsigned short)raw.product)) {
            unsigned char *report = buf;
            if (n >= 46 && report[0] == 0x42 && desktop_owns_pad()) {
                /* Keep report ID/header and telemetry. Release buttons,
                 * touches, triggers, sticks and both pad coordinates. */
                memset(report + 2, 0, 28);
            }
        } else if (ioctl(fd, EVIOCGID, &ev) == 0 && controller_id(ev.vendor, ev.product) &&
                   n % sizeof(struct input_event) == 0 && desktop_owns_pad()) {
            struct input_event *events = buf;
            for (size_t i = 0; i < (size_t)n / sizeof *events; ++i) {
                if (events[i].type == EV_KEY)
                    events[i].value = 0;
                else if (events[i].type == EV_ABS) {
                    struct input_absinfo axis;
                    if (ioctl(fd, EVIOCGABS(events[i].code), &axis) == 0)
                        events[i].value = events[i].code == ABS_Z || events[i].code == ABS_RZ ?
                            axis.minimum : axis.minimum + (axis.maximum - axis.minimum) / 2;
                }
            }
        }
    }
    errno = saved_errno;
    return n;
}
