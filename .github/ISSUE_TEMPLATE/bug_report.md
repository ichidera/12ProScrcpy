---
name: Bug report
about: Something isn't working
title: ''
labels: bug
assignees: ''
---

## What happened

<!-- What's broken, what did you expect instead. -->

## Device info

- Device model:
- Is this the same reference hardware documented in `docs/real-device-adb-sendevent.md`? (yes / no, different device)
- Android version:
- Root method (Magisk, etc.):
- Output of `adb shell su -c "getenforce"`:

## Which input path is affected? (delete what doesn't apply)

- [ ] Touch (raw sendevent)
- [ ] Power / Volume (raw sendevent)
- [ ] Home / Back / Menu / AppSwitch / Copy / Cut (`input keyevent` fallback)
- [ ] Keyboard passthrough
- [ ] Scroll wheel
- [ ] Game controls editor / keymap panel
- [ ] Build/compile issue (not runtime behavior)
- [ ] Other / not sure

## Steps to reproduce

1.
2.
3.

## For touch/coordinate issues: sample data

<!--
If touch lands in the wrong place, a few (intended tap) -> (actual tap)
coordinate pairs are the most useful thing you can provide - see
docs/real-device-adb-sendevent.md for how past calibration was done.
-->

## Build output / logs

<!-- Paste the relevant compiler error or runtime log here. -->
