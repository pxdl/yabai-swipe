# yabai-swipe

Instant trackpad swipes between macOS Spaces.

macOS animates every Space switch made with the trackpad. yabai's scripting addition can switch Spaces instantly, but only for its own commands. yabai-swipe connects the two: it intercepts the native horizontal Space swipe and runs `yabai -m space --focus <space>` instead.

It is a single C file that runs as a LaunchAgent. It has no UI and registers no hotkeys, so keyboard shortcuts stay with skhd (or whatever you use).

## How it works

1. A `CGEventTap` watches Dock swipe gesture events from the trackpad.
2. A horizontal swipe is swallowed when it begins, so the Dock never animates it.
3. On the first movement, yabai-swipe works out the neighbouring Space on the display under the cursor and spawns `yabai -m space --focus <index>`.
4. On macOS 27+ the swipe's end event is passed through with its motion zeroed, because the Dock needs it to reset its gesture state.

Space indexes match yabai's numbering: 1..n across all displays, in the order `CGSCopyManagedDisplaySpaces` returns them (see `space_manager_mission_control_index` in yabai).

## Requirements

- macOS on Apple Silicon. Tested on macOS 27.2.
- [yabai](https://github.com/asmvik/yabai) with the scripting addition loaded: `sudo yabai --load-sa` exits 0. Without it, `space --focus` animates or fails.
- System Settings → Trackpad → More Gestures → **Swipe between full-screen applications** enabled. That's the gesture being intercepted; whether it uses three or four fingers is up to you.
- Xcode Command Line Tools (`clang`, `codesign`).

## Install

```sh
make install
```

This builds `build/yabai-swipe`, ad-hoc signs it, copies it to `~/.local/bin/yabai-swipe`, writes `~/Library/LaunchAgents/com.pxdl.yabai-swipe.plist` and loads it.

The first launch asks for Accessibility permission and then exits. Enable `yabai-swipe` under System Settings → Privacy & Security → Accessibility, then:

```sh
make restart
```

The log at `/tmp/yabai-swipe.log` should end with `yabai-swipe: running (...)`.

## Configuration

Settings are environment variables in `com.pxdl.yabai-swipe.plist`. Edit them there, then run `make install` again.

| Variable | Values | Default | Effect |
|---|---|---|---|
| `YABAI_SWIPE_DISPLAY` | `cursor`, `focused` | `cursor` | Which display's Spaces the swipe moves through: the one under the mouse cursor, or yabai's focused display (`space --focus next/prev`). |
| `YABAI_SWIPE_INVERT` | `0`, `1` | `0` | Set to `1` if swipes go the wrong way. |

## Behaviour

- If yabai isn't running (no `/tmp/yabai_$USER.socket`), the swipe goes through to the normal animated switch.
- At the first or last Space of a display, the swipe does nothing. There is no wraparound.
- Vertical swipes (Mission Control, App Exposé) and three-finger page swipes are untouched.
- Switching Spaces on a display that isn't focused also moves focus to that display (yabai behaviour).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Accessibility prompt again after `make install` | Every build changes the ad-hoc signature. Remove `yabai-swipe` from Accessibility with `−`, re-add `~/.local/bin/yabai-swipe` with `+`, then `make restart`. Toggling the existing entry off and on is not enough. |
| Swipes still animate | Check `sudo yabai --load-sa` exits 0 and that `yabai -m space --focus 2` is instant from a terminal. After a macOS update the scripting addition is usually what breaks. |
| Wrong Space is chosen | `~/.local/bin/yabai-swipe --target next` (or `prev`) prints the index a swipe would focus for the display under the cursor: `-1` at an edge, `0` if the display can't be resolved. Compare with `yabai -m query --spaces`. |
| Nothing happens at all | `launchctl print gui/$(id -u)/com.pxdl.yabai-swipe` and `/tmp/yabai-swipe.log`. |

## Make targets

| Target | Does |
|---|---|
| `make` | Build and sign `build/yabai-swipe` |
| `make install` | Build, install the binary and LaunchAgent, (re)load it |
| `make restart` | Restart the running agent |
| `make uninstall` | Unload the agent and delete the binary and plist |
| `make clean` | Delete `build/` |

## Credits

Gesture interception is adapted from [InstantSpaceSwitcher](https://github.com/jurplel/InstantSpaceSwitcher) by jurplel (MIT). Unlike that project, yabai-swipe doesn't synthesize gestures or provide hotkeys; the switch itself is done by yabai.

## License

MIT, see [LICENSE](LICENSE).
