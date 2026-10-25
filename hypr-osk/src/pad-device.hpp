#pragma once

#include <linux/input.h>
#include <sys/ioctl.h>

/* The Puck can expose one combined keyboard/mouse node per channel, all
 * named "Valve Software Steam Controller Puck". Match identity and input
 * capabilities instead of assuming separate "Puck Keyboard/Mouse" names.
 * Leave real gamepad/absolute-axis nodes available to games and evdev. */
static inline bool padPhantomDevice(int fd)
{
    input_id id{};
    if (ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != 0x28de ||
        (id.product != 0x1302 && id.product != 0x1304))
        return false;

    unsigned char events[(EV_MAX + 8) / 8]{};
    unsigned char keys[(KEY_MAX + 8) / 8]{};
    if (ioctl(fd, EVIOCGBIT(0, sizeof events), events) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
        return false;
    auto has = [](const unsigned char *bits, unsigned bit) {
        return (bits[bit / 8] & (1u << (bit % 8))) != 0;
    };
    return has(events, EV_KEY) && !has(events, EV_ABS) && !has(keys, BTN_GAMEPAD);
}
