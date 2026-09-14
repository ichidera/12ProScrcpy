# Developer documentation

This covers what a new contributor actually needs: how to get a working
build, how the input-injection architecture is put together, where the
device-specific calibration data lives, and the gotchas that have already
cost real time to work through — so they don't get rediscovered from
scratch.

If you only read one section, read [§4 Architecture](#4-architecture) —
it explains *why* this fork looks the way it does, which makes the rest of
the codebase make sense.

---

## 1. What this project is

`12ProScrcpy` is a fork of [QtScrcpy](https://github.com/barry-ran/QtScrcpy)
(itself a Qt GUI wrapper around [scrcpy](https://github.com/Genymobile/scrcpy)),
rebuilt around **root-elevated raw input injection** (`su` + `sendevent`
directly into kernel input nodes, and `input keyevent` as a framework
fallback) instead of scrcpy's normal control-socket protocol. See
[§4](#4-architecture) for why.

Target hardware calibration data (touch coordinate ranges, which physical
node each hardware button lives on) is specific to the device documented in
`docs/real-device-adb-sendevent.md` — a Qualcomm "Waipio"-class device. If
you're bringing this up on different hardware, that doc is your template
for re-deriving the equivalent values; see [§6](#6-porting-to-a-different-device).

---

## 2. Build environment

### Prerequisites

| Tool | Notes |
|---|---|
| Qt **6.8.0**, MSVC2022 64-bit kit | Base install typically only includes core modules — you also need the **Qt Multimedia** and **Qt SQL** modules explicitly (see below) |
| Visual Studio 2022/2026, "Desktop development with C++" | Full IDE or just Build Tools — either works |
| CMake 3.10+ | Bundled with Qt or VS, or install separately |
| Windows SDK | See the pinned-version note below — **do not assume the newest installed SDK works** |

### Getting the missing Qt modules

If you didn't install Qt through the full interactive installer (e.g. you're
not sure how your existing Qt install was set up, or don't want to create an
account), use [`aqtinstall`](https://github.com/miurahr/aqtinstall) instead —
pulls the same official packages, no login required:

```bat
pip install aqtinstall
aqt install-qt windows desktop 6.8.0 win64_msvc2022_64 -m qtmultimedia qtsql -O X:\Qt
```

(`-O` points at your existing Qt root so it slots in alongside whatever's
already there, rather than creating a second install.)

Missing `Qt6Multimedia.dll` or `Qt6Sql.dll` at runtime (`"...System Error:
The code execution cannot proceed because QtXSql.dll was not found"`) means
this step wasn't done, or `windeployqt` (see below) wasn't re-run after
installing the module.

### Windows SDK — pin a known-working version

Multiple installed SDK versions can coexist, and **the newest one is not
necessarily the working one**. On the reference dev machine, SDK
`10.0.28000.0` (a preview-channel version) has a broken `mt.exe` that
crashes with `STATUS_DLL_NOT_FOUND` — `10.0.26100.0` works correctly.

Check what's installed:
```bat
dir "C:\Program Files (x86)\Windows Kits\10\bin"
```

Sanity-check a candidate directly before trusting it:
```bat
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe" /?
```
(should print full usage; a broken one prints nothing and exits silently)

Once you know a good version, pin every dev shell to it explicitly:
```bat
call "X:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0
```

**This must be run in every new `cmd.exe` window** before configuring or
building — it doesn't persist across shells, and using the wrong shell type
(PowerShell run scripts that `call` a `.bat` don't propagate environment
variables back into the PowerShell process the way plain `cmd.exe` does) is
a common way to silently end up back on the default (possibly broken)
toolchain/compiler.

### `third_party/` binaries are not in git

`12ProScrcpyCore/src/third_party/` (prebuilt FFmpeg libs/headers/DLLs, `adb`
binaries, `scrcpy-server`) is deliberately excluded from the repository and
from `scripts/zip_project.py`'s export — these are large binary blobs
fetched separately, not source. If a clean checkout fails to compile with
`Cannot open include file: 'libavcodec/avcodec.h'`, this folder is empty or
missing. Get it from the upstream
[`QtScrcpyCore`](https://github.com/barry-ran/QtScrcpyCore) repo's
equivalent `src/third_party` path and copy it in at the same relative
location.

### Building

```bat
call "X:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0
cd /d <repo_root>
mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="X:\Qt\6.8.0\msvc2022_64"
cmake --build . --config Release
X:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe --release <repo_root>\output\x64\Release\12ProScrcpy.exe
```

Incremental rebuild after code changes (no need to redo the `cmake ..`
configure step, just re-init the compiler environment per shell):
```bat
call "X:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0
cd /d <repo_root>\build
cmake --build . --config Release
```

### `LNK1104: cannot open file '...\12ProScrcpy.exe'`

Not a code problem — the previous build's exe is still running (or held
open by Explorer/AV). Kill it and relink, no rebuild needed:
```bat
taskkill /f /im 12ProScrcpy.exe
cmake --build . --config Release
```

---

## 3. Repo layout

```
12ProScrcpy/
├── 12ProScrcpy/              # Qt GUI application
│   ├── ui/                   # Windows, dialogs, the game-controls editor/panel
│   ├── uibase/                # MagneticWidget (edge-docking toolbar base), etc.
│   ├── util/                 # Config persistence, platform mouse-tap helpers
│   └── 12ProScrcpyCore/       # Core device-communication library (see below)
│       ├── include/           # Public headers (AdbSendEventSession, etc.)
│       ├── src/
│       │   ├── adb/           # AdbSendEventSession - the root-shell sendevent channel
│       │   ├── device/
│       │   │   ├── controller/    # Controller - the real input-dispatch hub
│       │   │   │   ├── inputconvert/  # Mouse/keyboard/keymap -> Controller calls
│       │   │   │   └── receiver/      # Device -> PC (video/clipboard) direction
│       │   │   ├── decoder/       # FFmpeg-based video decode
│       │   │   └── server/        # scrcpy-server.jar process management (video/audio only now - see §4)
│       │   └── third_party/       # NOT in git - see §2
├── docs/
│   ├── real-device-adb-sendevent.md   # Calibration reference for the target device
│   └── raw-input-daemon-design.md     # Deferred future-work design doc
├── scripts/
│   └── zip_project.py         # Exports a git-aware zip snapshot of the repo
└── config/
```

---

## 4. Architecture

### The problem this fork exists to solve

Stock scrcpy/QtScrcpy sends touch/key input over a **TCP control socket** to
`scrcpy-server.jar`, which runs on-device as the unprivileged `shell` user
(launched via `adb shell app_process ...`) and injects input through
Android's `InputManager` API. On some devices/Android versions, that path
throws:

```
ERROR: Injecting input events requires the caller ... to have the
INJECT_EVENTS permission.
```

This happens regardless of root, because **the server process itself runs
unprivileged** — root on the shell doesn't retroactively grant that already-
running unprivileged process anything. The one device-side workaround
(Developer Options → "USB debugging (Security settings)") isn't present or
reliable on every device/OEM build.

### The fix: bypass the server's input path entirely

Instead of relying on `scrcpy-server`'s privileged-input path, this fork
drives input two other ways, chosen per input type based on what's actually
confirmed to work on real hardware:

1. **Raw `sendevent` into real kernel input nodes**, via a persistent
   `adb shell su` session (`AdbSendEventSession`). Used for touch (always)
   and for hardware buttons that have a confirmed real kernel node
   (Power, Volume — see `docs/real-device-adb-sendevent.md`).
2. **Root-elevated `input keyevent`** (framework-level, not raw evdev), for
   keys with **no** confirmed real hardware node — AppSwitch, Copy, Cut, and
   (importantly) Home/Back/Menu on this specific device, because
   `uinput-goodix`'s `KEY_HOME`/`KEY_BACK`/`KEY_MENU` capabilities register
   at the kernel level (confirmed via `getevent`) but don't actually
   translate into real navigation actions — it's a gesture-wake virtual
   device, not a general nav-key source. Root bypasses the same
   `INJECT_EVENTS` check that blocks the unprivileged server, since `input`
   run as root goes through the normal framework path with elevated
   privilege rather than the shell UID's restricted one.

`scrcpy-server.jar` is still used, but now **only for what it's actually
needed for**: video/audio streaming and device→PC signaling (clipboard,
rotation events via `dumpsys`). It is no longer in the touch/key injection
path at all.

### Key files and what each owns

- **`AdbSendEventSession`** (`adbsendeventsession.h/.cpp`) — owns one
  persistent `adb shell su` process for the whole session. Escalates to
  root and sets SELinux permissive once at startup (`setenforce 0` — see
  `docs/real-device-adb-sendevent.md` §2 for why this is required even as
  root). Exposes:
  - `touchDown/touchMove/touchUp` — Type B multitouch primitives
  - `pressHome/pressBack/pressMenu/pressPower/pressVolumeUp/pressVolumeDown`
    — raw sendevent against each button's specific confirmed hardware node
    (`HardwareKeyProfile`)
  - `pressKeyEvent(int androidKeycode)` — the `input keyevent` fallback

- **`Controller`** (`controller.h/.cpp`) — the real dispatch hub every input
  path funnels through:
  - `sendRealTouch()` — maps mirrored-frame coordinates to the touch
    panel's native coordinate space (`mapFrameToRawTouch()`), handling
    portrait vs. landscape (including the `ROTATION_90`/`ROTATION_270`
    disambiguation — see the inline comments in `mapFrameToRawTouch()` for
    the point-reflection derivation and its confidence level). MOVE events
    are throttled/coalesced to ~60Hz (`ensureTouchMoveThrottle()` /
    `flushPendingTouchMoves()`) — necessary because each raw `sendevent`
    spawns a new process on-device, and a live drag can generate move
    samples faster than the phone can fork+exec+exit them, which without
    throttling causes a backlog that keeps visibly "sliding" after the
    finger lifts.
  - `sendRealScroll()` — mouse wheel, converted into a short synthetic
    swipe on the *same* real touch channel (there's no root-shell
    equivalent of a raw "scroll" event) rather than any framework command,
    so it feels like a real finger flick.
  - `sendRealKeyEvent()` / `postKeyCodeClick()` — the `input keyevent`
    fallback path.
  - `postGoHome/postGoMenu/postGoBack/postPower/postVolumeUp/postVolumeDown`
    — pick raw-sendevent vs. framework-fallback per button, per the
    reasoning in the previous section.

- **`InputConvertNormal`** / **`InputConvertGame`**
  (`inputconvert/inputconvertnormal.*`, `inputconvert/inputconvertgame.*`)
  — translate Qt mouse/keyboard/keymap events into `Controller` calls.
  Both route touch through `sendRealTouch()`; keyboard passthrough and
  keymap "android key" bindings route through `sendRealKeyEvent()`.

- **`GameControlsPanel`** / **`GameControlsEditor`**
  (`ui/gamecontrolspanel.*`, `ui/gamecontrolseditor.*`) — the in-app touch
  keymap system (define on-screen regions that map to taps/swipes/drags,
  independent of the scrcpy control-socket keymap system upstream ships).

### Known trade-offs (read before "fixing" these)

- **Keyboard modifiers are not conveyed through `input keyevent`.** It
  takes a bare keycode with no metastate flag — `Shift+A`, `Ctrl+C`, etc.
  won't carry the modifier through this fallback path. This was a
  deliberate trade against the alternative (the socket path, which hits
  `INJECT_EVENTS` and doesn't work at all). Fixing this properly means real
  evdev keyboard injection, which is a materially bigger effort — see
  `docs/raw-input-daemon-design.md`.
- **`input keyevent` fires an atomic press+release** — there's no way to
  inject a true separate down/up through it. Where this matters
  (`InputConvertGame::sendKeyEvent`, `InputConvertNormal::keyEvent`), the
  fallback fires once on physical key-*down* and no-ops on key-*up*. Qt's
  own auto-repeat (`isAutoRepeat()`) re-fires `KeyPress` while a key is
  held, so a held key still produces repeated presses server-side without
  explicit hold-state tracking — but true "hold" semantics (as opposed to
  "repeated discrete presses") are not preserved.
- **`ROTATION_90`'s touch formula is a derived hypothesis, not
  independently verified** the way `ROTATION_270` was (see the comment
  block in `Controller::mapFrameToRawTouch()`). If touch is off specifically
  in that orientation, recalibrate from fresh on-device tap samples the
  same way `docs/real-device-adb-sendevent.md` §6 was produced, rather than
  assuming the point-reflection math is exactly right.
- **The per-event `sendevent` process-spawn cost is a real, structural
  latency floor**, not fully eliminated by the MOVE throttle (which
  mitigates the *symptom* — backlog — not the underlying per-event
  overhead). See `docs/raw-input-daemon-design.md` for the planned
  persistent-daemon architecture that would actually remove it.

---

## 5. Coding conventions

- Match existing style in the file you're editing; a `clang-format` config
  and helper script (`12ProScrcpy/clang-format-all.sh`) are present — run it
  before submitting a PR that touches formatting-sensitive code.
- The build treats warnings as errors (`/WX`). A change that compiles with
  warnings locally in a lenient config will still fail CI/a clean build —
  check compiler output for warnings, not just the final success/fail
  status.
- Commit messages in this repo follow a light `type: summary` convention
  (`fix: ...`, `docs: ...`) — not strictly enforced, but preferred for
  scanability in `git log`.
- When adding a new input path (a new button, a new gesture), decide
  explicitly whether it needs raw `sendevent` (only if you've *confirmed*
  a real hardware node backs it — see `docs/real-device-adb-sendevent.md`
  for the `getevent`-based verification method) or the `input keyevent`
  fallback (safe default for anything without a confirmed node). Don't
  assume a `KEY_*` capability showing up in a device's capability dump
  means raw injection will actually work — `uinput-goodix`'s Home/Back/Menu
  capabilities are the concrete counter-example (§4).

---

## 6. Porting to a different device

The touch/button calibration in this codebase (`TouchProfile`,
`HardwareKeyProfile` defaults in `adbsendeventsession.h`) is specific to the
one physical device this fork was developed against. To bring it up on
different hardware:

1. Enumerate input devices: `adb shell su -c "getevent -pl"` (or inspect
   `/proc/bus/input/devices`) to find the touchscreen's node and its
   `ABS_MT_POSITION_X/Y` max values, and which node (if any) really backs
   each hardware button.
2. Confirm SELinux enforcement status and whether `setenforce 0` is needed
   (it was on the reference device; may not be on all Android
   builds/kernels).
3. For each candidate hardware-button node, **verify with `getevent`
   during a live `sendevent` call** that the event both (a) registers at
   the kernel level and (b) actually produces the expected on-screen
   action — don't assume a matching `KEY_*` capability is sufficient (§4).
4. Update `TouchProfile`/`HardwareKeyProfile` defaults accordingly, and
   write up the findings as a new `docs/real-device-<device>.md`, following
   the existing doc's structure, so the next port has a template and this
   one stays intact as reference for its own device.
