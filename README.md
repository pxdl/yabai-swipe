# yabai-swipe

Instant trackpad swipes between Spaces. The native horizontal Dock swipe is intercepted and replaced with `yabai -m space --focus next|prev`, which yabai's scripting addition performs without the slide animation.

Gesture interception is adapted from [InstantSpaceSwitcher](https://github.com/jurplel/InstantSpaceSwitcher) (MIT). Unlike ISS it does not synthesize gestures or register hotkeys; keyboard switching stays in skhd.

## Requirements

- yabai with the scripting addition loaded (`sudo yabai --load-sa` exits 0).
- System Settings → Trackpad → More Gestures → "Swipe between full-screen applications" enabled (the gesture being intercepted).
- Accessibility permission for `~/.local/bin/yabai-swipe`.

## Install

```sh
make install     # builds, copies to ~/.local/bin, loads the LaunchAgent
```

First launch prompts for Accessibility. Grant it, then `make restart`. Every rebuild changes the ad-hoc signature, so remove and re-add the entry in Accessibility after `make install`.

Log: `/tmp/yabai-swipe.log`. Remove with `make uninstall`.

## Behaviour

- If yabai isn't running (no `/tmp/yabai_$USER.socket`), swipes fall through to the native animation.
- At the first/last Space nothing happens (yabai has no wraparound).
- Wrong direction? Set `YABAI_SWIPE_INVERT=1` in the plist's `EnvironmentVariables` and `make install`.
- Vertical swipes (Mission Control, App Exposé) are untouched.
- Multiple displays: next/prev apply to yabai's focused display, not the one under the cursor.
