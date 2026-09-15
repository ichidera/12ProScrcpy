# 12ProScrcpy

A [QtScrcpy](https://github.com/barry-ran/QtScrcpy) fork rebuilt around
**root-elevated raw input injection** for rooted Android devices where
scrcpy's normal control-socket input path hits an `INJECT_EVENTS`
permission wall.

Two input backends exist, both bypassing `scrcpy-server`'s unprivileged
input path entirely:

- **`AdbSendEventSession`** (default, always available) — a persistent
  `adb shell su` session issuing `sendevent` directly against real kernel
  input nodes, with a root-elevated `input keyevent` fallback for keys
  without a confirmed hardware node.
- **`RawInputDaemonSession`** (opt-in, faster) — a small persistent daemon
  pushed to and run on the device itself, talked to over a raw TCP socket
  on the phone's USB-RNDIS interface. Removes the per-touch-event process-spawn
  cost `AdbSendEventSession` has (every `sendevent` call forks a new
  process on-device), which is the difference between touch that feels
  "close enough" and touch that feels like a real finger. See
  [`docs/daemon-implementation-plan.md`](docs/daemon-implementation-plan.md).
  **Requires USB tethering enabled on the phone** (Settings → Network &
  Internet → Hotspot & tethering → USB tethering) — without it the daemon
  can't resolve an IP to connect to and touch is silently dropped rather
  than falling back (a deliberate choice, not a bug — see the dev doc).

`scrcpy-server.jar` is still used for what it's actually needed for —
video/audio mirroring and device→PC signaling — it's just no longer in the
touch/key path.

> **New here?** Read [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — it
> explains *why* the architecture looks this way, which makes the rest of
> the codebase make sense. Also includes the full build walkthrough below,
> plus everything else a contributor needs.

---

## Why this exists

Stock scrcpy/QtScrcpy sends input over a TCP control socket to
`scrcpy-server.jar`, which runs on-device as the unprivileged `shell` user
and injects input through Android's `InputManager`. On some devices/Android
versions this throws:

```
ERROR: Injecting input events requires the caller ... to have the
INJECT_EVENTS permission.
```

...regardless of root, because the server process itself is unprivileged —
root on the shell doesn't retroactively grant an already-running
unprivileged process anything. This fork bypasses that path entirely by
driving input directly, at the root/kernel level, instead.

See [`docs/DEVELOPMENT.md` §4](docs/DEVELOPMENT.md#4-architecture) for the
full explanation, and
[`docs/real-device-adb-sendevent.md`](docs/real-device-adb-sendevent.md)
for the on-device calibration data (touch coordinate ranges, which kernel
node backs each hardware button) this fork's defaults are tuned against.

### Reference target device

| | |
|---|---|
| Platform | Qualcomm "Waipio"-class SoC (Snapdragon 8 Gen 1-class) |
| Touchscreen | FocalTech `fts`, Type B multitouch |
| Root | Magisk, unlocked bootloader |

*(Different devices may work but will very likely need different
calibration data — see
[`docs/DEVELOPMENT.md` §6](docs/DEVELOPMENT.md#6-porting-to-a-different-device)
for how to re-derive it.)*

## Repo structure

```
12ProScrcpy/
├── 12ProScrcpy/                    # Qt GUI application
│   └── 12ProScrcpyCore/             # Core device-communication library
│       ├── src/adb/                 # AdbSendEventSession + RawInputDaemonSession (PC-side clients)
│       ├── src/rawinputdaemon/      # raw_input_daemon.c - on-device daemon binary source
│       ├── src/device/controller/   # Controller - the real input-dispatch hub
│       └── src/third_party/         # NOT tracked in git - see docs/DEVELOPMENT.md §2
├── docs/
│   ├── DEVELOPMENT.md               # Full build guide + architecture
│   ├── real-device-adb-sendevent.md # Hardware calibration reference
│   ├── raw-input-daemon-design.md   # Original daemon design doc + rotation-compensation experiment result
│   ├── daemon-implementation-plan.md# Daemon build plan, phased
│   └── JOURNEY.md                   # War stories from getting this all working
├── scripts/                         # Dev helper scripts (project zip export, etc.)
└── config/
```

## Prerequisites

- A rooted Android device (see the reference target above; other devices
  need their own calibration — see `docs/DEVELOPMENT.md` §6)
- USB debugging enabled and the device authorized for ADB
- Qt **6.8.0**, MSVC2022 64-bit kit, **plus the Qt Multimedia and Qt SQL
  modules** (not included in a minimal Qt install by default)
- Visual Studio 2022/2026 with the C++ desktop workload
- CMake 3.10+

## Getting started

Full details, including how to fetch missing Qt modules without the
interactive installer and how to avoid a broken-Windows-SDK pitfall, are in
[`docs/DEVELOPMENT.md` §2](docs/DEVELOPMENT.md#2-build-environment). Short
version:

```bat
call "<path-to-VS>\VC\Auxiliary\Build\vcvars64.bat" <known-good-SDK-version>
cd <repo_root>
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<path-to-Qt>\6.8.0\msvc2022_64"
cmake --build . --config Release
<path-to-Qt>\6.8.0\msvc2022_64\bin\windeployqt.exe --release <repo_root>\output\x64\Release\12ProScrcpy.exe
```

`third_party/` (FFmpeg, `adb`, `scrcpy-server`) is not in this repository —
see `docs/DEVELOPMENT.md` §2 for where to get it; without it, the build
fails with a missing `libavcodec/avcodec.h` error.

### Optional: building the raw-input daemon

The daemon backend (faster touch, see above) needs a separate arm64 build
pushed to the device, alongside the PC-side app build:

```powershell
cd 12ProScrcpy\12ProScrcpyCore\src\rawinputdaemon
aarch64-linux-android30-clang -static raw_input_daemon.c -o qtscrcpy_raw_input_daemon
copy qtscrcpy_raw_input_daemon <repo_root>\output\x64\Release\qtscrcpy_raw_input_daemon
```

**The binary name and location matter** — `RawInputDaemonSession` looks
for exactly `qtscrcpy_raw_input_daemon` next to `12ProScrcpy.exe` (same
directory `adb.exe`/`scrcpy-server` already live in) and pushes/launches it
itself; it does not use whatever's already running on the device under a
different name. You do not need to manually `adb push`/`adb shell` it
yourself — that's only useful for isolated testing of the daemon binary on
its own, outside the app.

**Also requires USB tethering enabled on the phone** (see above) — this is
separate from USB *debugging*, easy to have one on without the other, and
the single most common reason the daemon silently fails to connect.

## Usage

Connect your rooted device over USB (or configured wireless ADB), launch
`12ProScrcpy.exe`, and select the device. The in-app game-controls editor
(touch-mapped on-screen buttons/gestures) is accessed from the toolbar's
control icon.

## Troubleshooting

- **`INJECT_EVENTS` error in logs:** this is the exact problem this fork
  exists to route around — if you're seeing it, something is still on the
  old control-socket path. See `docs/DEVELOPMENT.md` §4.
- **Touch registers but nothing happens (esp. Home/Back/Menu):** some
  hardware buttons' kernel-level capability doesn't mean the OS will
  actually act on a raw injected event for it — see the Home/Back/Menu
  case study in `docs/DEVELOPMENT.md` §4.
- **Device not detected:** confirm ADB authorization and USB/wireless
  debugging is enabled.
- **Build fails on a fresh checkout:** almost always either the missing Qt
  modules, a bad Windows SDK pin, or missing `third_party/` — see
  `docs/DEVELOPMENT.md` §2, which documents all three with the exact fixes.
- **Touch silently does nothing, log shows "raw input daemon not
  available... dropping touch event":** the daemon backend failed to start
  and is deliberately not falling back (see the daemon note above). Check,
  in order: USB tethering is on; the daemon binary is correctly named and
  placed (see "Optional: building the raw-input daemon" above); and the
  app was actually *reconnected* after fixing either of those — the daemon
  is only attempted once per connection, so fixing the cause after a failed
  attempt doesn't retroactively retry it within the same session.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). This project follows the
[Contributor Covenant](CODE_OF_CONDUCT.md).

## Disclaimer

This project targets rooted devices with an unlocked bootloader. Rooting
and unlocking your bootloader can void your warranty and carries risk of
bricking your device — proceed at your own risk, and only on hardware you
understand the implications for. `AdbSendEventSession` sets SELinux to
permissive mode for the session (`setenforce 0`) as a documented, deliberate
trade-off required for raw input injection to function — see
`docs/real-device-adb-sendevent.md` §2 for what that does and doesn't
affect.

## Licence

Since it is based on QtScrcpy, which is based on scrcpy, it uses the same
license as scrcpy:

    Copyright (C) 2025 Rankun

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.