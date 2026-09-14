# Future work: persistent on-device raw-input daemon

**Status:** Deferred — parked here until the current branch's outstanding
work (panel positioning, remaining input audits, etc.) is done. Not started.

**Baseline reference:** `uinput_daemon.c` (BlueStacks 5 prototype, attached
separately) — persistent process, newline-delimited text protocol, one open
device fd for the life of the process, no per-event process spawn. The design
below adapts that same shape to the real phone's actual hardware input nodes
instead of a self-created virtual uinput device. Transport differs: BlueStacks
used a Unix-domain socket + `adb forward`; this design uses a plain TCP socket
over USB-RNDIS, which removes the ADB server process hop from the hot path
entirely (see "Transport" section below).

---

## Why (current bottleneck)

The present architecture (`AdbSendEventSession`) drives input by writing
`sendevent ...` command lines into a persistent interactive `adb shell su`
session. Each line still causes the device to **fork+exec a brand-new
`sendevent` process** — open the node, one ioctl/write, exit. The shell
executes these strictly serially, so a fast stream of events (e.g. a live
touch drag) can arrive faster than the phone can fork+exec+exit each one,
building a backlog that visibly keeps draining after input has already
stopped. (This is what the ~60Hz client-side MOVE-coalescing throttle in
`Controller` currently works around — a mitigation, not a fix for the root
cause.)

A persistent daemon removes the per-event process-spawn cost entirely:
one process, one already-open fd per device node, a `write()` call per
event instead of a full process lifecycle.

---

## What's simpler here than the BlueStacks version

BlueStacks had no real touchscreen hardware to target, so the daemon had to
**create** a virtual device via `/dev/uinput` (`UI_DEV_SETUP` /
`UI_ABS_SETUP` / `UI_DEV_CREATE`, register EV_KEY/EV_ABS bits, etc.) before
it could emit anything.

On the real phone, the driver nodes already exist and are already fully
configured by the kernel/driver:

- Touch: `/dev/input/event6` (`fts`)
- Home/Back/Menu: `/dev/input/event1` (`uinput-goodix`)
- Power: `/dev/input/event2` (`pmic_pwrkey`)
- Volume up: `/dev/input/event0` (`gpio-keys`)
- Volume down: `/dev/input/event3` (`pmic_resin`)

So there's no `UI_DEV_*` setup block at all — just `open(path, O_WRONLY)`
against each node once at daemon startup, and `write(fd, &ie, sizeof(ie))`
per event, exactly like BlueStacks's `emit()` but against a real node
instead of a synthetic one. No `UI_DEV_DESTROY` teardown either — just
`close()`.

---

## What carries over unchanged

- **SELinux.** Root alone was confirmed insufficient for `sendevent` to
  register against these real nodes — `setenforce 0` was required first
  (see `docs/real-device-adb-sendevent.md` §2). The daemon should run
  `system("setenforce 0")` (or equivalent) at its own startup rather than
  assuming the caller already did it in a separate shell.
- Daemonizing pattern (double-fork, detach, log to a file) — reusable as-is.
- The `select()`-based single-current-client accept loop — reusable as-is
  (still true here: there's only one logical "remote control session" at a
  time).

---

## What's different / needs extending

### Multiple fds, not one

BlueStacks's daemon owns exactly one fd (its own virtual device — touch
only). The real phone spreads touch and hardware buttons across five
separate nodes. The daemon needs to open all five at startup and route each
command to the correct fd:

| Command family | Target node |
|---|---|
| `DOWN` / `MOVE` / `UP` / `TAP` / `SWIPE` | touch fd (event6) |
| `HOME` / `BACK` / `MENU` | event1 |
| `POWER` | event2 |
| `VOLUP` | event0 |
| `VOLDOWN` | event3 |

### Slot-aware touch protocol

BlueStacks's daemon assumes a single virtual finger (hardcoded slot 0). The
real app supports up to `MULTI_TOUCH_MAX_NUM = 9` concurrent contacts for
game keymaps (steer wheel + fire button held simultaneously, etc.), so the
protocol needs an explicit slot parameter:

```
DOWN slot trackid x y
MOVE slot x y
UP slot
```

(`TAP`/`SWIPE` can stay slot-0-only convenience wrappers on top of the same
primitives, same as BlueStacks's version.)

### Real axis ranges + BTN_TOUCH

- `AXIS_MAX 32767` (BlueStacks placeholder) → real confirmed values:
  `xMax = 14399`, `yMax = 31999`.
- `BTN_TOUCH` must be emitted alongside slot/tracking-id (the `fts` driver
  requires it explicitly — confirmed in `docs/real-device-adb-sendevent.md`
  §6, unlike BlueStacks's simpler Type-A panel which didn't need it).

### ROTATION_90 / ROTATION_270 handling

**Resolved by experiment (see below): rotation compensation is NOT free,
even via a synthetic device — the daemon must own it.**

Coordinate pre-rotation for landscape frames currently lives in
`Controller::mapFrameToRawTouch()` on the PC side (C++). A speculative
idea was tested before committing to that split further: register a
*virtual* `uinput` touchscreen (same `INPUT_PROP_DIRECT` property real
touch drivers use) instead of injecting into the real panel's node, on the
theory that Android's `TouchInputMapper` might apply its normal automatic
rotation compensation to any `INPUT_PROP_DIRECT` device, synthetic or not,
making the whole ROTATION_90/270 disambiguation problem evaporate.

**Tested and refuted.** Sending the identical raw coordinate through a
registered `INPUT_PROP_DIRECT` virtual device at each of the three
rotations produced the same uncompensated, glass-fixed behavior as the real
panel already exhibits — confirmed by the ROTATION_90 vs. ROTATION_270
results being exact mirror images of each other (matching the "180°-apart
chirality" relationship already documented for the real panel), not an
identity mapping. A display-unassociated synthetic direct-touchscreen does
not get automatic compensation without an explicit display-port
association via a custom `.idc` file — a materially more invasive,
harder-to-maintain path (writable system-protected location, fragile
across reboots) than just keeping the rotation transform in our own code,
for no real payoff over what's already verified and shipped. Not worth
pursuing further.

**Decision:** move the rotation transform from the PC (`Controller`) into
the daemon. This was previously left PC-side reasoning that the PC already
has rotation-poll context for other reasons (UI layout) so the daemon
wouldn't need to duplicate it — but the daemon can just as easily poll
`dumpsys window` itself, and doing so lets the wire protocol be pure
frame-space coordinates (`DOWN pointer_id x y`, etc.), with zero rotation
awareness needed on the PC side of the wire at all. Simpler protocol,
one less thing the two sides need to agree on.

---

## Transport: direct TCP over USB-RNDIS

The daemon binds a plain TCP socket. The PC connects directly to the phone's
USB-RNDIS interface IP (typically `192.168.42.129` or whatever the RNDIS
adapter assigns) — no `adb forward`, no ADB server process in the path at
all. Every packet goes USB wire → kernel TCP stack → daemon, with nothing
else touching it.

This is the single biggest remaining structural latency win and is exactly
what root + unlocked bootloader access enables: `adb forward` would have
added a full extra process hop (the PC-side ADB server) on every packet in
steady state. USB-RNDIS removes that hop entirely. The RNDIS interface is
available whenever USB debugging is active on a rooted device, requiring no
additional setup beyond what the app already depends on.

`adb` is still used for the one-time push (`adb push`) and launch
(`adb shell su -c`) of the daemon binary at session start. It is not in
the hot path after that.

---

## Open questions / risks before starting

1. **`struct input_event` ABI.** The `time` field's width (`struct timeval`
   with `long` members) depends on the target kernel's time_t size (32-bit
   vs 64-bit `time_t`, relevant post-Y2038 kernel changes). Needs to be
   verified against this specific device's actual kernel headers, not
   assumed from the BlueStacks/x86 build — a struct layout mismatch would
   silently write garbage/misaligned events instead of failing loudly.
2. **Cross-compile toolchain.** Needs an Android NDK `aarch64-linux-android<API>-clang
   -static` build (same invocation style as the BlueStacks binary), targeting
   this phone's actual arch/API level rather than reusing the BlueStacks x86
   static binary.
3. **Where the prebuilt binary lives.** Likely bundled alongside the existing
   `adb.exe` / `scrcpy-server` in `12ProScrcpyCore/src/third_party/`, pushed
   to `/data/local/tmp/` once per session the same way `adb push` already
   happens for those.
4. **Failure/fallback behavior.** If the daemon fails to start (push fails,
   exec fails, wrong ABI, SELinux still enforcing for some other reason),
   `AdbSendEventSession`'s existing per-event `sendevent` path should remain
   as an automatic fallback rather than a hard failure - don't regress
   the currently-working path while this is being brought up.

---

## Non-goals for this doc

- Not adding Wi-Fi / LAN transport variants — USB-RNDIS is the transport;
  other network paths are out of scope for this doc.
- Not touching keyboard-modifier (Shift/Ctrl/Alt) injection here — that's a
  separate, harder problem (real evdev keyboard node vs. `input keyevent`'s
  bare-keycode limitation) noted elsewhere, out of scope for this doc.