// yabai-swipe: replace the native trackpad swipe between Spaces with
// `yabai -m space --focus <space>`, which is instant via yabai's scripting addition.
//
// Gesture interception adapted from InstantSpaceSwitcher (MIT, (c) 2026 jurplel):
// https://github.com/jurplel/InstantSpaceSwitcher/blob/f7616e4/Sources/ISS/ISS.c

#include <ApplicationServices/ApplicationServices.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern char **environ;
extern int CGSMainConnectionID(void);
extern CFArrayRef CGSCopyManagedDisplaySpaces(int cid);

// Private CGEvent fields (see ISS.c).
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
static bool tracking, fired;
static bool macos27;   // Dock needs the real ended event to close its gesture state
static bool invert;        // YABAI_SWIPE_INVERT=1 flips direction
static bool focused_mode;  // YABAI_SWIPE_DISPLAY=focused: switch on yabai's focused display
static char socket_path[256];

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

// Returns false if yabai isn't running, so the native swipe still works.
static bool yabai_focus(double value) {
    if (access(socket_path, F_OK) != 0) return false;
    bool next = (value > 0) != invert;

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

    char *argv[] = { "yabai", "-m", "space", "--focus", (char *)selector, NULL };
    pid_t pid;
    // Async on purpose: blocking the tap callback stalls all input. SIGCHLD is ignored, so no zombies.
    return posix_spawnp(&pid, "yabai", NULL, NULL, argv, environ) == 0;
}

static CGEventRef callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *ctx) {
    (void)proxy; (void)ctx;

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
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
        if (access(socket_path, F_OK) != 0) return event;  // yabai down: native swipe
        tracking = true;
        fired = false;
        return NULL;

    case kPhaseChanged: {
        if (!tracking) return event;
        double progress = CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
        if (!fired && progress != 0.0) fired = yabai_focus(progress);
        return NULL;
    }

    case kPhaseEnded: {
        if (!tracking) return event;
        double velocity = CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
        if (!fired && velocity != 0.0) yabai_focus(velocity);
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

int main(int argc, char **argv) {
    // Debug: print the index a swipe would focus on the display under the cursor.
    if (argc == 3 && strcmp(argv[1], "--target") == 0) {
        printf("%d\n", cursor_target_index(strcmp(argv[2], "next") == 0));
        return 0;
    }

    signal(SIGCHLD, SIG_IGN);

    const char *user = getenv("USER");
    snprintf(socket_path, sizeof(socket_path), "/tmp/yabai_%s.socket", user ? user : "");

    const char *inv = getenv("YABAI_SWIPE_INVERT");
    invert = inv && strcmp(inv, "1") == 0;

    const char *mode = getenv("YABAI_SWIPE_DISPLAY");
    focused_mode = mode && strcmp(mode, "focused") == 0;

    char version[32] = {0};
    size_t size = sizeof(version);
    macos27 = sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0 && atoi(version) >= 27;

    // Prompts for Accessibility on first run.
    const void *keys[] = { kAXTrustedCheckOptionPrompt };
    const void *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool trusted = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    if (!trusted) {
        fprintf(stderr, "yabai-swipe: accessibility permission missing; grant it, then `make restart`\n");
        return 0;  // clean exit: KeepAlive only restarts crashes, so no prompt loop
    }

    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                           (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl), callback, NULL);
    if (!tap) {
        fprintf(stderr, "yabai-swipe: could not create event tap\n");
        return 1;
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    fprintf(stderr, "yabai-swipe: running (macOS %s, display=%s, invert=%d)\n",
            version, focused_mode ? "focused" : "cursor", invert);
    CFRunLoopRun();
    return 0;
}
