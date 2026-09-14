# Contributing to 12ProScrcpy

Thanks for taking a look at this project. It's a fairly specialized fork —
please read this before opening a PR, it'll save both of us time.

## Before you start

- **Read `docs/DEVELOPMENT.md` first.** It explains why this fork's input
  architecture looks the way it does (root-elevated raw `sendevent` /
  `input keyevent` instead of scrcpy's normal control socket), which is
  essential context before touching anything under `12ProScrcpyCore/src/device/controller/`
  or `src/adb/`.
- **This project targets specific rooted hardware.** Calibration data
  (touch coordinate ranges, which kernel node backs each hardware button)
  in `docs/real-device-adb-sendevent.md` is device-specific. If you're
  testing on different hardware, say so in your PR — a fix that's correct
  for one device's node layout may not be for another's, and "works on my
  device" needs the device stated.
- **Get a build working first.** Follow `docs/DEVELOPMENT.md` §2 end to
  end, including the Qt module installation and Windows SDK version
  pinning — both are common, previously-hit stumbling blocks with fixes
  already documented there. If you hit something not covered, that's
  itself useful feedback — see "Reporting issues" below.

## Reporting issues

Use the issue templates under `.github/ISSUE_TEMPLATE/`. For anything
input-related (touch/button not working, wrong coordinates, lag), please
include:

- Device model and confirmation of whether it's the same hardware
  documented in `docs/real-device-adb-sendevent.md`, or different hardware
- Output of `adb shell su -c "getenforce"` (SELinux mode)
- Whether the issue is with raw `sendevent`-driven input (touch, Power,
  Volume) or the `input keyevent` fallback path (Home/Back/Menu, AppSwitch,
  Copy/Cut, keyboard passthrough) — see `docs/DEVELOPMENT.md` §4 if you're
  not sure which path a given control uses
- For touch/coordinate issues specifically: a few `(intended tap) →
  (actual tap)` coordinate pairs, the same way past rotation calibration
  was done (see `docs/real-device-adb-sendevent.md`)

## Making changes

### Input-handling changes specifically

If you're adding a new button/gesture or changing how an existing one is
injected:

1. Decide whether it needs raw `sendevent` or the `input keyevent`
   fallback. **Don't assume** a matching `KEY_*` capability in a device's
   capability dump means raw injection will work — verify with `getevent`
   during a live test that the event both registers *and* produces the
   expected on-screen action. `docs/DEVELOPMENT.md` §4 has a concrete
   counter-example (Home/Back/Menu on the reference device) where this
   assumption was wrong.
2. If it's genuinely a new confirmed hardware node, add the finding to
   `docs/real-device-adb-sendevent.md` (or a new device-specific doc if
   you're on different hardware — see `docs/DEVELOPMENT.md` §6).
3. Prefer routing through `Controller`'s existing entry points
   (`sendRealTouch`, `sendRealKeyEvent`, `sendRealScroll`) rather than
   calling `AdbSendEventSession` directly from UI/input-conversion code —
   keeps the dispatch logic (rotation handling, throttling, fallback
   choice) in one place.

### Code style

- Match the surrounding file's style. A `clang-format` config exists;
  run `12ProScrcpy/clang-format-all.sh` before submitting if your change
  touches formatting-sensitive areas.
- The build uses `/WX` (warnings-as-errors) — a change that compiles with
  warnings locally in a lenient config will still fail a clean build.
  Check your compiler output, not just whether it linked.
- Commit messages: a light `type: summary` convention (`fix: ...`,
  `docs: ...`) is used throughout this repo's history — not strictly
  enforced, but appreciated for `git log` scanability.

### Before opening a PR

- Confirm a clean build (`cmake --build . --config Release` from a fresh
  or updated `build/` directory) succeeds with no new warnings.
- If your change touches input injection, state what you actually tested
  it against (device, orientation, whether it's the raw or fallback path)
  in the PR description — this is a project where "compiles" and "works on
  real hardware" are genuinely different bars, and reviewers can't verify
  hardware behavior from the diff alone.
- Use the PR template under `.github/PULL_REQUEST_TEMPLATE.md`.

## Code of Conduct

This project follows the Contributor Covenant — see `CODE_OF_CONDUCT.md`.
