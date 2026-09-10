# 12ProScrcpy

A [QtScrcpy](https://github.com/barry-ran/QtScrcpy) build tailored for **rooted Xiaomi 12 Pro (codename `zeus`)** devices, with fixes and tweaks for screen mirroring on Magisk-rooted, unlocked-bootloader setups where the stock `scrcpy-server` push/exec flow runs into permission and SELinux friction.

> This repo bundles a vendored copy of `QtScrcpy` and `Scrcpy` alongside supporting scripts and docs, rather than relying on stock upstream builds, because the target device needed root-aware handling that upstream doesn't ship with by default.

---

## Why this exists

Stock `scrcpy`/`QtScrcpy` assumes a fairly "clean" ADB environment. On a rooted Xiaomi 12 Pro running MIUI/HyperOS with Magisk, a few things commonly break or need adjusting:

- Pushing/executing `scrcpy-server` can be blocked or interfered with depending on SELinux enforcing mode and MIUI's own restrictions.
- Magisk root needs to be accounted for (root shell vs. adb shell permissions, `su` handling).
- Device-specific quirks around unlocked bootloader / `orange` boot state and MIUI's ADB behavior.

This project's goal is to smooth over those issues so mirroring "just works" on this specific device, rather than being a general-purpose scrcpy replacement.

### Target device

| | |
|---|---|
| Model | Xiaomi 12 Pro (`2201122G`) |
| Codename | `zeus` (region: `zeus_eea`) |
| Chipset | Qualcomm SM8450 (Snapdragon 8 Gen 1) |
| Android / MIUI base | Android 13 (SDK 33) |
| Root | Magisk, unlocked bootloader (`orange` state) |

*(Other devices may work but are untested — this repo is being developed and validated against the device above specifically.)*

## Root-specific tweaks

- Handling around `scrcpy-server` push/exec that avoids the permission issues a stock push can hit on a Magisk-rooted, SELinux-restricted setup.
- Adjustments for running server-side commands with root context where a plain `adb shell` invocation isn't sufficient.

*(This section will get more specific as the tweaks are finalized — see `docs/` for details as they're written up.)*

## Repo structure

```
12ProScrcpy/
├── QtScrcpy/     # QtScrcpy source/build, with device-specific patches
├── Scrcpy/       # Vendored scrcpy (server/client) used by QtScrcpy
├── scripts/      # Setup/build/launch helper scripts (work in progress)
├── docs/         # Notes, device specs, troubleshooting write-ups
└── .gitignore
```

> **Status:** `scripts/` is still being fleshed out (device setup, build automation, and/or launch presets are all on the table). This section of the README will be filled in once the scripts stabilize.

## Prerequisites

- A rooted Xiaomi 12 Pro (`zeus`) with Magisk and an unlocked bootloader
- USB debugging enabled, and the device authorized for ADB
- ADB installed and on your `PATH`
- Qt (version TBD) and a working C++ toolchain, if building `QtScrcpy` from source

## Getting started

```powershell
git clone <this-repo-url>
cd 12ProScrcpy
# build instructions TBD — see QtScrcpy/ for upstream build docs in the meantime
```

*(Full build/setup steps will be documented here once the build process is finalized.)*

## Usage

*(Fill in once the launch flow is locked down — e.g. any wrapper script, quality/bitrate presets, or connection mode you standardize on.)*

## Troubleshooting

- **`scrcpy-server` fails to push/start:** check SELinux mode and Magisk root grant for your shell/ADB session.
- **Device not detected:** confirm ADB authorization and that USB/wireless debugging is enabled.

*(Expand this section as real issues come up during development.)*

## Disclaimer

This project targets a rooted device with an unlocked bootloader. Rooting and unlocking your bootloader can void your warranty and carries risk of bricking your device — proceed at your own risk, and only on hardware you understand the implications for.

## Licence
Since it is based on Qtscrcpy which is scrcpy, it uses the same license as scrcpy

    Copyright (C) 2025 Rankun
    
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at
    
        http://www.apache.org/licenses/LICENSE-2.0
    
    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.