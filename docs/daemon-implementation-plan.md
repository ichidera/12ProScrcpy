# Implementation plan: persistent raw-input daemon

**Status: DRAFT — ready to start.** Both go/no-go experiments referenced in
`docs/raw-input-daemon-design.md` have now run and passed:

- **Rotation-compensation experiment:** refuted (no automatic framework
  compensation for a display-unassociated `INPUT_PROP_DIRECT` synthetic
  device) — but this resolved an open design question rather than blocking
  anything: the daemon owns the rotation transform itself.
- **Swipe-smoothness experiment:** confirmed. A persistent open `fd` with
  wall-clock-paced direct `write()` calls (zero process-spawn per event)
  produced swipes that feel like real continuous gestures, including
  natural fling/deceleration — validating the core premise that the
  current per-event `sendevent` process-spawn is in fact the bottleneck,
  not some deeper issue.

This doc turns that into an actual, phased build plan. Supersedes the
"deferred, not started" status on `docs/raw-input-daemon-design.md` — that
doc's architecture reasoning stays valid and is the reference for *why*;
this doc is the *how* and *in what order*.

---

## 0. Branch & scope

- New branch, off current `master` once the present branch's outstanding
  work is done (per earlier decision to hold this out).
- **Scope for v1:** touch only (tap/swipe/multitouch). Hardware buttons
  (Power/Volume/Home/Back/Menu) and the `input keyevent` fallback path
  stay on `AdbSendEventSession` exactly as they are today — they're
  low-frequency, discrete, already working, and not latency-sensitive the
  way continuous touch is. Migrating them is a cheap follow-up once the
  daemon is proven in production, not a v1 requirement.
- **Explicit non-goal for v1:** Tier 2 transport (direct LAN TCP). Ship
  Tier 1 (daemon + `adb forward`) first; only revisit Tier 2 if Tier 1
  still isn't enough once it's actually running in the app, not based on
  speculation.

---

## 1. Daemon binary (on-device, C, arm64)

### 1.1 Skeleton (reuse, don't rewrite)

Base structure ports directly from the BlueStacks `uinput_daemon.c`
pattern, already re-validated twice now (rotation test, swipe test):
daemonize (double-fork, detach), open a listening socket, accept one
client connection at a time, read newline-delimited text commands, `write()`
directly to an already-open device fd per event.

### 1.2 What's different from the BlueStacks version / the test harnesses

- **Target the real panel node** (`/dev/input/event6`, per
  `docs/real-device-adb-sendevent.md`), not a virtual uinput device — the
  test harnesses used uinput because it needed no SELinux permissive step
  and was faster to iterate on; the shipped daemon should inject into the
  real node the same way `AdbSendEventSession` already does, since that's
  the verified, production-confirmed path.
- **`setenforce 0` at daemon startup** (`system("setenforce 0")` or
  equivalent), not assumed pre-done by the caller — same requirement as
  `AdbSendEventSession` today, now owned by the daemon itself so it's
  self-sufficient.
- **Multi-fd, not one.** Open all five relevant nodes at startup (touch +
  four button nodes) even though v1 only actively uses the touch one —
  makes the button/key migration (future work, §6) a protocol-only change
  later, no daemon restructuring needed.
- **Slot-aware protocol**, not single-finger — the daemon owns
  `ABS_MT_SLOT`/`ABS_MT_TRACKING_ID` allocation per `pointer_id` from the
  wire protocol (see §3), not the PC.
- **Rotation transform owned here**, polling `dumpsys window`'s
  `mCurrentRotation` itself (same source `Controller`'s poll uses today) —
  per the resolved design question in §"ROTATION_90 / ROTATION_270
  handling" of `docs/raw-input-daemon-design.md`. Reuse the exact
  point-reflection formula already verified in
  `Controller::mapFrameToRawTouch()` — this is a port, not a re-derivation.

### 1.3 Concrete build steps

1. Strip `uinput_swipe_test.c` down to its `emit`/`syn`/`touch_*` primitives
   and command-parsing loop as the starting skeleton (it's already a
   working persistent-fd + text-protocol daemon in miniature).
2. Replace `setup_device()`'s uinput registration with plain `open()`
   against the five real nodes (no `UI_DEV_SETUP`/`UI_ABS_SETUP`/
   `UI_DEV_CREATE` needed at all — see `docs/raw-input-daemon-design.md`
   "What's simpler on real hardware").
3. Add the rotation-poll (`popen("dumpsys window")`-style or a persistent
   sub-shell, ported from `Controller::pollDeviceRotation()`'s command)
   and the coordinate transform.
4. Replace the interactive `stdin` command loop with a Unix domain socket
   `accept()`/read loop (BlueStacks daemon's structure).
5. Wire the daemon into the app's session lifecycle: pushed once via
   `adb push` to `/data/local/tmp/` at connection start (alongside the
   existing `adb.exe`/`scrcpy-server` push, same `third_party/`-adjacent
   bundling), launched via `adb shell su -c`, torn down on disconnect.

---

## 2. PC side

### 2.1 New class: `RawInputDaemonSession`

Parallel to `AdbSendEventSession`, not a replacement of it (v1 keeps both
alive — see §2.2). Owns:

- Pushing/launching the daemon binary
- `adb forward`-ing a local TCP port to the daemon's Unix socket
- A persistent `QTcpSocket` (or equivalent) connection to the forwarded port
- Writing wire-protocol lines for `DOWN`/`MOVE`/`UP`

### 2.2 Fallback behavior — non-negotiable for v1

If the daemon fails to start for *any* reason (push fails, exec fails,
wrong ABI on unexpected hardware, `adb forward` fails), `Controller` must
transparently fall back to the existing `AdbSendEventSession` touch path.
**Never a hard failure, never a regression against what works today.**
Concretely: `Controller::ensureRealTouchSession()` tries
`RawInputDaemonSession` first, and on any startup failure, falls back to
constructing `AdbSendEventSession` exactly as it does now. This should be
close to a no-op change to `Controller`'s public surface —
`sendRealTouch()`'s callers don't need to know which backend is live.

### 2.3 What gets simpler in `Controller`

- `mapFrameToRawTouch()`'s rotation branch: **deleted** for the
  daemon-backed path (daemon does it now) — but stays intact, unchanged,
  as-is for the `AdbSendEventSession` fallback path, since that one still
  needs to pre-compute raw panel coordinates itself. Two code paths
  temporarily, not a rewrite of the fallback.
- `ensureTouchMoveThrottle()`/`flushPendingTouchMoves()`: **evaluate
  after real-world testing**, don't remove reflexively. The daemon removes
  the process-spawn cost that motivated the throttle, but keeping a
  lightweight coalescing pass may still be worthwhile as backpressure
  protection against a saturated `adb forward` link — decide with real
  measurements, not assumption, once the daemon is actually wired in.

---

## 3. Wire protocol (v1)

Plain newline-delimited text, matching what both test harnesses already
validated (no reason to add binary framing complexity the swipe test
didn't need):

```
DOWN pointer_id x y
MOVE pointer_id x y
UP pointer_id
```

- Coordinates are **frame-space** (mirrored-window pixels), not raw panel
  space — the daemon does frame→panel + rotation transform, matching what
  `Controller::mapFrameToRawTouch()` does today, just relocated.
- `pointer_id` is an opaque small integer from the PC side (existing
  `Controller` slot numbering scheme carries over unchanged); daemon maps
  it to a real `ABS_MT_SLOT`/`ABS_MT_TRACKING_ID` pair internally.
- The PC needs to tell the daemon the current mirrored-frame size once
  per connection (and again on resize) so it can do frame→panel scaling —
  add a `FRAME width height` command, sent on connect and on every
  `Controller::resizeDisplay()`.

---

## 4. Testing plan

Phased, each gate before moving to the next:

1. **Daemon standalone, driven manually** (same style as the swipe test's
   interactive loop, but talking to the real node + through a forwarded
   socket instead of stdin) — confirm real-panel injection through this
   new path works at all, separate from wiring it into the app.
2. **`RawInputDaemonSession` behind a feature flag / debug toggle** in the
   app, `AdbSendEventSession` still the default — lets touch be tested
   through the new path without it being live for normal use yet.
3. **Side-by-side comparison**, same tests as the swipe experiment
   (scrolling, flicks, drags) but through the actual app UI this time, not
   the raw test harness — confirm the improvement holds up under real
   mirrored-frame coordinate mapping, not just raw panel coordinates typed
   by hand.
4. **Fallback path exercised deliberately** — simulate daemon-push failure
   (e.g. rename the binary temporarily) and confirm `Controller` falls
   back to `AdbSendEventSession` cleanly, not silently broken.
5. **Flip the default**, `AdbSendEventSession` becomes the explicit
   fallback-only path.

---

## 5. Rollback plan

Since `AdbSendEventSession` isn't being removed in v1 (§2.2), rollback is
just flipping which backend `ensureRealTouchSession()` tries first — no
data migration, no destructive change, low-risk to ship incrementally.

---

## 6. Explicitly deferred to v2 (not part of this plan)

- Migrating Power/Volume/Home/Back/Menu/AppSwitch/Copy/Cut onto the daemon
  (protocol already has room for it via the multi-fd design in §1.2, but
  no urgency — those paths work fine today).
- Tier 2 transport (direct LAN TCP, bypassing `adb forward`).
- Keyboard modifier support (Shift/Ctrl/Alt) — separate, harder problem,
  real evdev keyboard injection, out of scope here same as it was in the
  original design doc.

---

## Open questions before starting

- Exact daemon push/launch lifecycle: once per app launch, or once per
  device connection (re-push on reconnect)? Leaning toward per-connection,
  matching how `AdbSendEventSession` already re-establishes its session.
- `adb forward` port allocation: fixed port vs. dynamically chosen
  (`adb forward tcp:0 ...` and read back the assigned port) — dynamic is
  safer against port collisions if the app is ever run twice / multiple
  devices connected simultaneously.