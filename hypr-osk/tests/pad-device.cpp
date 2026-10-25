#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/input.h>
#include <sys/ioctl.h>

static input_id device;
static std::array<unsigned char, (EV_MAX + 8) / 8> events;
static std::array<unsigned char, (KEY_MAX + 8) / 8> keys;
static unsigned long failRequest;

static int deviceIoctl(int, unsigned long request, ...)
{
    if (request == failRequest)
        return -1;
    va_list args;
    va_start(args, request);
    void *out = va_arg(args, void *);
    va_end(args);
    if (request == EVIOCGID)
        std::memcpy(out, &device, sizeof device);
    else if (request == EVIOCGBIT(0, events.size()))
        std::memcpy(out, events.data(), events.size());
    else if (request == EVIOCGBIT(EV_KEY, keys.size()))
        std::memcpy(out, keys.data(), keys.size());
    else
        return -1;
    return 0;
}

#define ioctl deviceIoctl
#include "../src/pad-device.hpp"
#undef ioctl

static void reset(unsigned product = 0x1304)
{
    device = {};
    device.vendor = 0x28de;
    device.product = product;
    events.fill(0);
    keys.fill(0);
    failRequest = 0;
    events[EV_KEY / 8] |= 1u << (EV_KEY % 8);
}

static void expect(const char *name, bool expected)
{
    if (padPhantomDevice(0) != expected) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main()
{
    for (unsigned product : {0x1302, 0x1304}) {
        reset(product);
        // Combined nodes observed live: keyboard keys, mouse buttons and REL.
        events[EV_REL / 8] |= 1u << (EV_REL % 8);
        keys[KEY_ENTER / 8] |= 1u << (KEY_ENTER % 8);
        keys[BTN_LEFT / 8] |= 1u << (BTN_LEFT % 8);
        expect("combined keyboard/mouse without a Mouse/Keyboard name", true);

        for (unsigned key : {KEY_ENTER, BTN_LEFT, KEY_VOLUMEUP, KEY_POWER}) {
            reset(product);
            keys[key / 8] |= 1u << (key % 8);
            expect("separate keyboard/mouse/consumer/system node", true);
        }
        reset(product);
        events[EV_ABS / 8] |= 1u << (EV_ABS % 8);
        keys[BTN_GAMEPAD / 8] |= 1u << (BTN_GAMEPAD % 8);
        expect("real gamepad stays available", false);
        keys.fill(0);
        expect("absolute-axis device stays available", false);
        reset(product);
        keys[BTN_GAMEPAD / 8] |= 1u << (BTN_GAMEPAD % 8);
        expect("button-only gamepad stays available", false);
    }
    reset();
    device.vendor = 0x046d;
    expect("unrelated keyboard stays available", false);
    reset();
    device.product = 0x1102;
    expect("unmanaged Valve product stays available", false);
    reset();
    events.fill(0);
    expect("non-key device stays available", false);
    for (unsigned long request : {EVIOCGID, EVIOCGBIT(0, events.size()), EVIOCGBIT(EV_KEY, keys.size())}) {
        reset();
        failRequest = request;
        expect("failed device query stays available", false);
    }
    std::puts("pad device classification passed");
}
