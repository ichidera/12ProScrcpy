# 12ProScrcpy

A [QtScrcpy](https://github.com/barry-ran/QtScrcpy) fork rebuilt around
**root-elevated raw input injection** for rooted Android devices where
scrcpy's normal control-socket input path hits an `INJECT_EVENTS`
permission wall — touch, buttons, scroll, and keyboard are driven via a
persistent `adb shell su` session using `sendevent` directly against real
kernel input nodes (with a root-elevated `input keyevent` fallback for keys
without a confirmed hardware node), instead of relying on
`scrcpy-server`'s unprivileged input-injection path.

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
│       ├── src/adb/                 # AdbSendEventSession - the root-shell sendevent channel
│       ├── src/device/controller/   # Controller - the real input-dispatch hub
│       └── src/third_party/         # NOT tracked in git - see docs/DEVELOPMENT.md §2
├── docs/
│   ├── DEVELOPMENT.md               # Full build guide + architecture
│   ├── real-device-adb-sendevent.md # Hardware calibration reference
│   └── raw-input-daemon-design.md   # Deferred future-work design doc
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
