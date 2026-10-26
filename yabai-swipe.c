// yabai-swipe: replace the native trackpad swipe between Spaces with
// `yabai -m space --focus <space>`, which is instant via yabai's scripting addition.
//
// Gesture interception adapted from InstantSpaceSwitcher (MIT, (c) 2026 jurplel):
// https://github.com/jurplel/InstantSpaceSwitcher/blob/f7616e4/Sources/ISS/ISS.c

#include <ApplicationServices/ApplicationServices.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// Private CoreGraphics API; yabai uses the same call via SkyLight (SLSCopyManagedDisplaySpaces).
extern int CGSMainConnectionID(void);
extern CFArrayRef CGSCopyManagedDisplaySpaces(int cid);

// Private CGEvent fields and values (see ISS.c).
static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;

enum { kCGSEventGesture = 29, kCGSEventDockControl = 30 };
enum { kIOHIDEventTypeDockSwipe = 23, kMotionHorizontal = 1 };
enum { kPhaseBegan = 1, kPhaseChanged = 2, kPhaseEnded = 4, kPhaseCancelled = 8 };

static CFMachPortRef tap;
static bool tracking, fired;   // a swipe we own is in progress / its switch has been issued
static bool macos27;           // Dock needs the real ended event to close its gesture state
static bool invert;            // YABAI_SWIPE_INVERT=1 flips direction
static bool focused_mode;      // YABAI_SWIPE_DISPLAY=focused: use yabai's focused display
static char socket_path[256];  // same path yabai listens on: /tmp/yabai_$USER.socket
static char self_path[PATH_MAX];

static bool yabai_running(void) {
    return access(socket_path, F_OK) == 0;
}

// AXIsProcessTrusted() is cached for the life of a process: a grant or revocation made while
// it runs is never seen (observed on macOS 27.2). A fresh copy of this binary reads the
// current state. Takes a few milliseconds; only called while waiting or after a tap disable.
static bool trusted_now(void) {
    char *argv[] = { self_path, "--check-permission", NULL };
    pid_t pid;
    int status;
    if (posix_spawn(&pid, self_path, NULL, NULL, argv, environ) != 0) return false;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int64_t dict_int(CFDictionaryRef dict, CFStringRef key) {
    int64_t value = 0;
    CFNumberRef num = dict ? CFDictionaryGetValue(dict, key) : NULL;
    if (num) CFNumberGetValue(num, kCFNumberSInt64Type, &value);
    return value;
}

// yabai's space index for the neighbour of the visible Space on the display under the
// cursor. yabai numbers Spaces 1..n in CGSCopyManagedDisplaySpaces order across displays
// (space_manager_mission_control_index). Returns -1 at the first/last Space, 0 if unknown.
static int cursor_target_index(bool next) {
    CGEventRef probe = CGEventCreate(NULL);
    if (!probe) return 0;
    CGPoint cursor = CGEventGetLocation(probe);
    CFRelease(probe);

    CGDirectDisplayID did;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(cursor, 1, &did, &count) != kCGErrorSuccess || count == 0) return 0;
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(did);
    if (!uuid) return 0;
    CFStringRef uuid_str = CFUUIDCreateString(NULL, uuid);
    CFRelease(uuid);

    int result = 0, offset = 0;
    CFArrayRef displays = CGSCopyManagedDisplaySpaces(CGSMainConnectionID());
    CFIndex display_count = displays ? CFArrayGetCount(displays) : 0;
    for (CFIndex i = 0; i < display_count; ++i) {
        CFDictionaryRef display = CFArrayGetValueAtIndex(displays, i);
        CFArrayRef spaces = CFDictionaryGetValue(display, CFSTR("Spaces"));
        CFStringRef id = CFDictionaryGetValue(display, CFSTR("Display Identifier"));
        int n = spaces ? (int)CFArrayGetCount(spaces) : 0;

        // A single entry ("Main") means displays don't have separate Spaces.
        bool match = display_count == 1 ||
                     (id && CFStringCompare(id, uuid_str, kCFCompareCaseInsensitive) == kCFCompareEqualTo);
        if (!match) { offset += n; continue; }

        int64_t current = dict_int(CFDictionaryGetValue(display, CFSTR("Current Space")), CFSTR("id64"));
        for (int j = 0; j < n; ++j) {
            if (dict_int(CFArrayGetValueAtIndex(spaces, j), CFSTR("id64")) != current) continue;
            int target = next ? j + 1 : j - 1;
            result = (target < 0 || target >= n) ? -1 : offset + target + 1;
            break;
        }
        break;
    }

    if (displays) CFRelease(displays);
    CFRelease(uuid_str);
    return result;
}

// Focus the neighbouring Space. Returns false if no switch was attempted (yabai down or
// spawn failed), so the next gesture event of the same swipe can retry.
static bool switch_space(double motion) {
    if (!yabai_running()) return false;
    bool next = (motion > 0) != invert;

    char index[16];
    const char *selector = next ? "next" : "prev";
    if (!focused_mode) {
        int target = cursor_target_index(next);
        if (target < 0) return true;  // edge of the display: swallow the swipe, like yabai does
        if (target > 0) {
            snprintf(index, sizeof(index), "%d", target);
            selector = index;
        }
    }

    // Reap earlier yabai clients, then spawn without waiting: blocking the tap callback
    // stalls input. At most one finished client lingers until the next swipe.
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    char *argv[] = { "yabai", "-m", "space", "--focus", (char *)selector, NULL };
    pid_t pid;
    return posix_spawnp(&pid, "yabai", NULL, NULL, argv, environ) == 0;
}

// Re-enable the tap after macOS disabled it (slow callback, sleep/wake). If Accessibility
// was revoked, re-arming an active tap can hold input, so exit instead; launchd restarts
// us into the permission wait in main().
static void rearm_tap(void) {
    if (CGEventTapIsEnabled(tap)) return;
    if (!trusted_now()) {
        fprintf(stderr, "yabai-swipe: accessibility permission revoked, exiting\n");
        exit(EXIT_FAILURE);
    }
    CGEventTapEnable(tap, true);
}

static void watchdog(CFRunLoopTimerRef timer, void *info) {
    (void)timer; (void)info;
    rearm_tap();
}

static CGEventRef callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *info) {
    (void)proxy; (void)info;

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        rearm_tap();
        return event;
    }

    int64_t kind = CGEventGetIntegerValueField(event, kCGSEventTypeField);
    if (kind != kCGSEventDockControl) {
        // Companion gesture events of a swipe we own
        return (kind == kCGSEventGesture && tracking) ? NULL : event;
    }

    // Only real trackpad input (HID kernel source) and only horizontal Dock swipes.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0) return event;
    if (CGEventGetIntegerValueField(event, kCGEventGestureHIDType) != kIOHIDEventTypeDockSwipe) return event;
    if (CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion) != kMotionHorizontal) return event;

    switch (CGEventGetIntegerValueField(event, kCGEventGesturePhase)) {
    case kPhaseBegan:
        if (!yabai_running()) return event;  // native swipe
        tracking = true;
        fired = false;
        return NULL;

    case kPhaseChanged: {
        if (!tracking) return event;
        double progress = CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
        if (!fired && progress != 0.0) fired = switch_space(progress);
        return NULL;
    }

    case kPhaseEnded: {
        if (!tracking) return event;
        double velocity = CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
        if (!fired && velocity != 0.0) switch_space(velocity);
        tracking = fired = false;
        if (!macos27) return NULL;
        // Zero the motion so the Dock closes its gesture state without switching again.
        CGEventSetDoubleValueField(event, kCGEventGestureSwipeProgress, 0.0);
        CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityX, 0.0);
        CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityY, 0.0);
        return event;
    }

    case kPhaseCancelled:
        if (!tracking) return event;
        tracking = fired = false;
        return NULL;

    default:
        return tracking ? NULL : event;
    }
}

// Shows the system prompt if permission is missing. Only meaningful as the first check in a
// process; see trusted_now().
static bool request_accessibility(void) {
    const void *keys[] = { kAXTrustedCheckOptionPrompt };
    const void *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    bool trusted = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    return trusted;
}

int main(int argc, char **argv) {
    // Internal: exit status tells a parent instance whether Accessibility is granted (trusted_now).
    if (argc == 2 && strcmp(argv[1], "--check-permission") == 0) {
        return AXIsProcessTrusted() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    // Debug: print the index a swipe would focus on the display under the cursor.
    if (argc == 3 && strcmp(argv[1], "--target") == 0 &&
        (strcmp(argv[2], "next") == 0 || strcmp(argv[2], "prev") == 0)) {
        printf("%d\n", cursor_target_index(strcmp(argv[2], "next") == 0));
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: yabai-swipe [--target next|prev]\n");
        return EXIT_FAILURE;
    }

    uint32_t self_size = sizeof(self_path);
    if (_NSGetExecutablePath(self_path, &self_size) != 0) {
        fprintf(stderr, "yabai-swipe: executable path too long\n");
        return EXIT_FAILURE;
    }

    const char *user = getenv("USER");
    snprintf(socket_path, sizeof(socket_path), "/tmp/yabai_%s.socket", user ? user : "");

    const char *inv = getenv("YABAI_SWIPE_INVERT");
    invert = inv && strcmp(inv, "1") == 0;

    const char *display = getenv("YABAI_SWIPE_DISPLAY");
    focused_mode = display && strcmp(display, "focused") == 0;
    if (display && !focused_mode && strcmp(display, "cursor") != 0) {
        fprintf(stderr, "yabai-swipe: unknown YABAI_SWIPE_DISPLAY=%s, using cursor\n", display);
    }

    char version[32] = {0};
    size_t size = sizeof(version);
    macos27 = sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0 && atoi(version) >= 27;

    // Prompt once. A grant made while this process runs is never visible to it, so wait on a
    // fresh process and then exit; KeepAlive starts a new instance that has the permission.
    if (!request_accessibility()) {
        fprintf(stderr, "yabai-swipe: waiting for accessibility permission\n");
        while (!trusted_now()) sleep(1);
        fprintf(stderr, "yabai-swipe: accessibility granted, restarting\n");
        return EXIT_FAILURE;
    }

    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                           CGEventMaskBit(kCGSEventGesture) | CGEventMaskBit(kCGSEventDockControl),
                           callback, NULL);
    if (!tap) {
        fprintf(stderr, "yabai-swipe: could not create event tap\n");
        return EXIT_FAILURE;  // launchd retries after its throttle interval
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source);  // the run loop retains it
    CGEventTapEnable(tap, true);

    // Backstop for a tap disabled without the disabled event being delivered.
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 5, 5, 0, 0, watchdog, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRelease(timer);

    fprintf(stderr, "yabai-swipe: running (macOS %s, display=%s, invert=%d)\n",
            version, focused_mode ? "focused" : "cursor", invert);
    CFRunLoopRun();
    return EXIT_SUCCESS;
}
