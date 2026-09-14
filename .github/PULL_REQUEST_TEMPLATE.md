## What this changes

<!-- Summary of the change and why. -->

## Testing

- [ ] Clean build succeeds (`cmake --build . --config Release`), no new warnings
- [ ] Tested on real hardware (state device model + whether it matches
      `docs/real-device-adb-sendevent.md`'s reference device or is different)

If this touches input injection (touch, buttons, keyboard, scroll, keymap):

- [ ] Confirmed which path this uses - raw `sendevent` or `input keyevent`
      fallback (see `docs/DEVELOPMENT.md` §4) - and why
- [ ] If claiming a new hardware node works, confirmed with `getevent` that
      the event both registers *and* produces the expected on-screen action
      (not just that a matching `KEY_*` capability exists - see the
      Home/Back/Menu counter-example in `docs/DEVELOPMENT.md` §4)

## Related issue(s)

<!-- Closes #... -->
