# qtscrcpy_raw_input_daemon

On-device counterpart of `RawInputDaemonSession` (PC side). Implements
`docs/daemon-implementation-plan.md` section 1 and the protocol/behaviour
documented in `docs/raw-input-daemon-design.md`.

## What it is

A small, persistent, daemonized C process that runs **on the phone**, opens
the real touch/button input nodes once, and drives them directly via
`write()` on an already-open fd instead of the current approach (a fresh
`sendevent` process fork+exec per event via a persistent `adb shell su`
session — see `AdbSendEventSession`). This removes the per-event
process-spawn cost that the go/no-go swipe-smoothness experiment confirmed
was the actual bottleneck.

v1 scope only drives the touch node (`/dev/input/event6`) via the wire
protocol below. The four button nodes are opened at startup too but not yet
wired to any command — see plan §6 "explicitly deferred to v2".

## Building

This is **not** part of the normal CMake/Qt build — it cross-compiles to
aarch64 Android, a completely different target than the PC application.
Build it once (or whenever `raw_input_daemon.c` changes) with the Android
NDK:

```sh
./build_raw_input_daemon.sh --ndk /path/to/android-ndk --api 26
```

This produces `../third_party/raw_input_daemon/qtscrcpy_raw_input_daemon`, a
static aarch64 binary. **That file is not checked in as a prebuilt binary in
this change** — it must be built locally with the NDK (no NDK toolchain is
available in this environment to produce it here). `RawInputDaemonSession`
degrades gracefully to the existing `AdbSendEventSession` path (per plan
§2.2) if the binary is missing, so the app still works normally without it;
building and dropping in the binary is what turns the daemon path on.

## Wire protocol (plan §3)

Plain newline-delimited text over the accepted Unix-domain client
connection:

```
FRAME width height
DOWN slot trackid x y
MOVE slot x y
UP slot
PING
QUIT
```

Coordinates in `DOWN`/`MOVE` are **frame-space** (mirrored-window pixels),
matching what `Controller::mapFrameToRawTouch()` computed on the PC before
this daemon existed — the daemon now does frame→panel scaling and the
rotation transform itself (see the design doc's "Decision" section), so the
wire protocol needs no rotation awareness on either side.

## Transport (Tier 1 only — plan's explicit non-goal for v1 is Tier 2)

The daemon listens on a Unix domain socket bound in the **abstract
namespace** (name `qtscrcpy_raw_input_daemon`, no filesystem path) — this is
exactly what `adb forward tcp:<port> localabstract:qtscrcpy_raw_input_daemon`
expects on the PC side, and avoids any writable-socket-path/SELinux/cleanup
concerns a filesystem-path socket would raise.

## Logging

Appends to `/data/local/tmp/qtscrcpy_raw_input_daemon.log` for post-hoc
debugging (`adb shell cat /data/local/tmp/qtscrcpy_raw_input_daemon.log`).
Not rotated — it's small and this is a dev/debug aid, not a production log
pipeline.

## Known open items (carried over from the design/plan docs, not resolved by this change)

- Real device validation of this exact binary (SELinux, ABI, kernel
  `struct input_event` layout) still needs to happen against the actual
  hardware per the plan's phased testing section (§4) — this repo change
  provides the code, not a from-hardware confirmation run.
- Hardware button migration (§6, v2).
- Tier 2 direct-TCP transport (§6, v2).
