# yabai-swipe

Instant trackpad swipes between macOS Spaces.

macOS animates every Space switch made with the trackpad. yabai's scripting addition can switch Spaces instantly, but only for its own commands. yabai-swipe intercepts the native horizontal Space swipe and runs `yabai -m space --focus <space>` instead, so swipes switch instantly too.

It is a single C file that runs as a LaunchAgent, with no UI and no hotkeys of its own. Keyboard shortcuts stay with skhd or whatever you use.

## How it works

1. A `CGEventTap` watches Dock swipe gesture events from the trackpad.
2. When a horizontal swipe begins, the tap swallows it, so the Dock never animates it.
3. On the first movement, yabai-swipe works out the neighbouring Space on the display under the cursor and spawns `yabai -m space --focus <index>`.
4. On macOS 27 and later, the swipe's end event goes through with its motion zeroed, because the Dock needs it to reset its gesture state.

Space indexes match yabai's numbering: 1..n across all displays, in the order `CGSCopyManagedDisplaySpaces` returns them (see `space_manager_mission_control_index` in yabai).

## Requirements

- macOS on Apple Silicon. Tested on macOS 27.2.
- [yabai](https://github.com/asmvik/yabai) with the scripting addition loaded: `sudo yabai --load-sa` exits 0. Without it, `space --focus` animates or fails.
- System Settings → Trackpad → More Gestures → **Swipe between full-screen applications** enabled. yabai-swipe intercepts that gesture, with three or four fingers as you prefer.
- Xcode Command Line Tools (`clang`, `codesign`). `make check` also needs `jq`.
- No other tool intercepting the same swipe, such as InstantSpaceSwitcher or Quicker with their swipe override enabled.

## Install

```sh
make install
```

This builds `build/yabai-swipe`, signs it, copies it to `~/.local/bin/yabai-swipe`, writes `~/Library/LaunchAgents/com.pxdl.yabai-swipe.plist` and loads it.

The first launch shows the Accessibility prompt. Enable `yabai-swipe` under System Settings → Privacy & Security → Accessibility, and it starts by itself within a few seconds, without a manual restart. The log at `/tmp/yabai-swipe.log` should end with `yabai-swipe: running (...)`.

### Keeping the permission across rebuilds

By default the binary is ad-hoc signed, and macOS ties the Accessibility grant to that exact build. After every rebuild you have to remove the entry with `−` and add it again with `+` (toggling it off and on is not enough).

A self-signed code-signing certificate makes the grant survive rebuilds, because macOS then ties it to the certificate. Create one once, as the [yabai wiki](https://github.com/asmvik/yabai/wiki/Installing-yabai-(from-HEAD)) describes: Keychain Access → Certificate Assistant → Create a Certificate, Identity Type **Self-Signed Root**, Certificate Type **Code Signing**. Then:

```sh
make install SIGN="name of the certificate"
```

Re-add the entry in Accessibility one last time; later builds signed with the same certificate keep it.

## Configuration

Settings are environment variables in `com.pxdl.yabai-swipe.plist`. Edit them there, then run `make install` again.

| Variable | Values | Default | Effect |
|---|---|---|---|
| `YABAI_SWIPE_DISPLAY` | `cursor`, `focused` | `cursor` | `cursor`: move through the Spaces of the display under the mouse cursor, like the native swipe, by focusing the neighbour's index. `focused`: run yabai's `space --focus next/prev`, which acts on the display of the focused window and, at a display's last Space, continues onto the next display. Identical on a single display. |
| `YABAI_SWIPE_INVERT` | `0`, `1` | `0` | Set to `1` if swipes go the wrong way. |

## Behaviour

- If yabai isn't running (no `/tmp/yabai_$USER.socket`), the swipe goes through to the normal animated switch.
- At the first or last Space of a display, the swipe does nothing (in `focused` mode, only at the first or last Space overall). There is no wraparound.
- Vertical swipes (Mission Control, App Exposé) and three-finger page swipes are untouched.
- Switching Spaces on a display that isn't focused also moves focus to that display (yabai behaviour).
- If macOS disables the event tap (after a slow callback or across sleep/wake), yabai-swipe turns it back on, and a check every 5 seconds catches the case where the disable notice never arrives. If Accessibility was revoked, it exits instead, because re-enabling a tap without the permission can hold input. launchd then restarts it, and it waits for the permission again.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Accessibility prompt again after `make install` | The ad-hoc signature changed (see [Keeping the permission across rebuilds](#keeping-the-permission-across-rebuilds)). Remove `yabai-swipe` from Accessibility with `−` and re-add `~/.local/bin/yabai-swipe` with `+`. |
| Swipes still animate | Check `sudo yabai --load-sa` exits 0 and that `yabai -m space --focus 2` is instant from a terminal. After a macOS update the scripting addition is usually what breaks. |
| Wrong Space is chosen | Run `make check` with the cursor on the affected display. It compares the index yabai-swipe computes (`yabai-swipe --target next\|prev`: `-1` at an edge, `0` if the display can't be resolved) with yabai's own view. |
| Nothing happens at all | Check `launchctl print gui/$(id -u)/com.pxdl.yabai-swipe` and `/tmp/yabai-swipe.log`. |

## Make targets

| Target | Does |
|---|---|
| `make` | Build and sign `build/yabai-swipe` (`SIGN=<certificate>` to use a certificate instead of ad-hoc) |
| `make install` | Build, install the binary and LaunchAgent, (re)load it |
| `make check` | Cross-check the Space index against yabai for the display under the cursor |
| `make restart` | Restart the running agent |
| `make uninstall` | Unload the agent and delete the binary and plist |
| `make clean` | Delete `build/` |

## Credits

Gesture interception is adapted from [InstantSpaceSwitcher](https://github.com/jurplel/InstantSpaceSwitcher) by jurplel (MIT). yabai-swipe leaves out that project's synthesized gestures and hotkeys; yabai does the switching.

## License

MIT, see [LICENSE](LICENSE).
