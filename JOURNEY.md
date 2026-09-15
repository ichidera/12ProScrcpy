# The journey

Not documentation in the usual sense — a chronicle of how this project
actually got built, including the wrong turns. If you're the kind of
person who reads changelogs for fun, this is for you. If you're trying to
fix a specific bug, you want `docs/DEVELOPMENT.md` instead.

---

## Chapter 1: "Why won't it even compile"

Before any of the interesting input-injection work, there was just...
getting a Windows build working at all. In order, each one looking solved
until the next one appeared:

- MinGW picked up instead of MSVC, because `vcvars64.bat` wasn't sourced
  in the shell CMake was run from. This alone happened *four separate
  times* across the project's history — it never stopped being the first
  thing to check when a build mysteriously used the wrong compiler.
- Windows SDK `10.0.28000.0` — the newest installed, therefore assumed
  best — had a broken `mt.exe` that didn't error, just silently produced
  nothing (`STATUS_DLL_NOT_FOUND`, no output at all). Diagnosed by running
  it standalone and comparing against an older SDK version, which worked
  fine. Newest ≠ working became a standing rule after this.
- Qt 6.8 needed the Multimedia and SQL modules explicitly — neither
  ships in a minimal install, and each missing one surfaces as a
  *runtime* `.dll not found` dialog, not a build error, so it looked like
  two completely unrelated problems the first time around.
- `git submodule update --init --recursive` wasn't enough, because the
  actual submodule was nested one level deeper than expected
  (`QtScrcpy/QtScrcpyCore`, not the repo root) — invisible until reading
  `.gitmodules` directly.
- `third_party/` (FFmpeg, `adb`, `scrcpy-server`) was never in git at all
  — a separate binary-blob folder that has to be fetched from the
  upstream `QtScrcpyCore` repo by hand. First appeared as a wall of
  `libavcodec/avcodec.h not found` errors that looked like a totally
  broken build, but was just a missing folder.

None of these were exotic. All of them were the kind of thing that costs
real time exactly once and then becomes a thirty-second checklist forever
after — which is the entire reason `docs/DEVELOPMENT.md` §2 exists.

## Chapter 2: the great renaming, and the bug that kept coming back

Partway through, the project got renamed — `QtScrcpy` → `12ProScrcpy`,
`QtScrcpyCore` → `12ProScrcpyCore`. Straightforward in theory. In practice
it left two land mines:

1. A handful of source files had the old path **hardcoded** in relative
   includes (`"../QtScrcpyCore/include/QtScrcpyCore.h"`), which broke the
   moment the folder was renamed.
2. The top-level `CMakeLists.txt` tried to link a target named
   `12ProScrcpyCore` — but the *actual* CMake target name inside the core
   library's own `CMakeLists.txt` was still `QtScrcpyCore` (just a string
   that happened not to get renamed). CMake doesn't error when you link a
   name that isn't a real target — it just silently fails to propagate
   include directories, so the symptom was a confusing "can't find
   QtScrcpyCore.h" error that looked identical to bug #1 but had a
   completely different cause.

Both were fixed. Then, across several later sessions, **both came back —
repeatedly** — because the fix had been applied to a working tree but
never actually `git commit`ed, and something (a fresh export, a checkout,
a zip round-trip) kept reverting to the last committed state, which still
had the bug. The actual lesson wasn't "fix the bug again" three or four
times — it was "commit the fix the moment it's confirmed working,"
which is now rule one in the personal quick-reference.

## Chapter 3: the `INJECT_EVENTS` wall

The actual reason this fork exists. `scrcpy-server.jar` runs on-device as
the unprivileged `shell` user, and on this hardware, Android refused to
let it inject input:

```
ERROR: Injecting input events requires the caller ... to have the
INJECT_EVENTS permission.
```

The confusing part: root access on the *shell* didn't help, because the
server process itself was never elevated — it's a completely separate
process from the interactive `adb shell su` session. Root on one doesn't
retroactively grant anything to the other. That distinction — "which
process actually needs the privilege" rather than "is the phone rooted"
— turned out to be the whole ballgame, and is the one idea everything
else in this project is downstream of.

The fix: stop asking `scrcpy-server` to inject anything. Write raw
`sendevent` calls directly into the kernel's input device nodes, from
inside an actually-root shell.

## Chapter 4: the hardware calibration hunt

Getting raw `sendevent` working meant first figuring out, for this
specific phone, which `/dev/input/eventN` node was the touchscreen, what
coordinate range it used, and — this was the surprising part — that
**SELinux enforcement was a second, independent gate on top of root.**
`uid=0` alone did nothing for hardware-key nodes; `setenforce 0` was
required too. Two separate locks, not one.

Then came the genuinely counterintuitive discovery: the touchscreen's
`uinput-goodix` virtual device advertised `KEY_HOME`, `KEY_BACK`, and
`KEY_MENU` as supported capabilities. Sending them registered cleanly at
the kernel level — confirmed with `getevent` showing clean DOWN/UP
pairs — and did *nothing at all* on screen. Turned out that device exists
for a gesture-wake feature, not general navigation, and just happens to
share capability bits with real nav keys. The fix ended up being the
`input keyevent` framework command instead — a completely different
mechanism, chosen per-button based on what was actually *verified* to
work, not what a capability list suggested should work. That distinction
— verify the action, not just the registration — became a standing rule,
written directly into `CONTRIBUTING.md`.

## Chapter 5: rotation, guessed then tested

Landscape touch worked in one rotation and not the other. The fix for the
untested direction was derived by reasoning, not measurement — the two
landscape rotations are mirror-image chiralities 180° apart, so the
untested formula should be the geometric point-reflection of the verified
one. Shipped on that reasoning; flagged honestly in the code comments as
"a hypothesis, not independently confirmed the way the first one was."

Later, a different idea surfaced: what if registering a *virtual*
`INPUT_PROP_DIRECT` touchscreen let Android's own rotation compensation
handle it automatically, removing the hand-derived math entirely? A
genuinely reasonable hypothesis — that property is exactly the mechanism
real touch drivers rely on to avoid ever thinking about rotation
themselves.

Built a real test for it (`uinput_rotation_test.c`), registered the
virtual device, sent identical raw coordinates at all three rotations,
and read the results off Android's built-in Pointer Location overlay.
**Refuted, cleanly** — the virtual device showed the exact same
uncompensated, glass-fixed behavior as the real panel, confirmed by the
two landscape rotations being exact mirror images of each other (the same
signature the original hand-derived hypothesis predicted). A
display-unassociated synthetic device doesn't get free rotation
compensation. Genuinely useful negative result — it meant the rotation
transform wasn't a real-hardware quirk to someday engineer around, it was
a fundamental property of the injection method, and belonged wherever the
injection itself happened.

## Chapter 6: is it actually the process-spawn cost, though?

The working theory for why touch felt "close but not quite real" was:
every `sendevent` call forks a brand-new process on the phone, and a fast
drag generates move samples faster than fork+exec+exit can keep up,
building a backlog that visibly kept "sliding" after the finger lifted.
Plausible. Untested.

Rather than commit real engineering time to a full persistent daemon on
that assumption alone, built a much smaller test first
(`uinput_swipe_test.c`): one process, one already-open file descriptor,
direct `write()` calls, wall-clock-paced so the test wasn't accidentally
just measuring a lucky sleep value. Fed it real swipe/scroll/flick
gestures and compared by feel against an actual finger on the same
screen.

It felt right — genuine fling behavior, natural deceleration, nothing
stepped or laggy. That result is what turned "maybe build a daemon
someday" into an actual phased implementation plan
(`docs/daemon-implementation-plan.md`) the same day.

## Chapter 7: building the daemon, or, the header forgot what the .cpp knew

The daemon's PC-side client (`RawInputDaemonSession`) evolved fast during
its own development — starting from a `QTcpSocket` + `adb forward` +
text-protocol sketch, ending up as raw BSD/Winsock sockets, a binary
8-byte packet protocol, and a direct connection over the phone's
USB-RNDIS interface instead of `adb forward` at all. Good evolution. The
header didn't keep up — it still declared the *original* design's members
and methods, while the `.cpp` had moved on to an entirely different set.
The compiler eventually said so, in the most roundabout way possible:
`ssize_t` undeclared and a truncation warning, which were real but minor
— the actual problem was a header and implementation that had quietly
drifted into describing two different classes that happened to share a
name.

Then, once that was sorted and the daemon binary itself got built and
pushed to the device by hand for a manual test — it worked, standing
alone. And touch through the actual app still didn't, dropping every
event with "raw input daemon not available."

Turned out to be two things stacked on top of each other: a manually-run
test binary with the wrong filename sitting on the device, entirely
unrelated to what the app itself was trying to launch — and, once that
was fixed, a "tried once per connection, never retried" latch meaning the
app's first (failed, because the binary wasn't in place yet) attempt was
the only one it would ever make without a full reconnect. Fixing the
binary after the fact didn't help until the connection itself was torn
down and rebuilt.

And after *all* of that — new header, new binary, correct name, correct
location, full reconnect — touch was still silently dropped. Turned out
that the actual, final, honest-to-god reason was USB *tethering* being
switched off — a completely separate toggle from USB *debugging*, which
had been on the whole time. Every earlier layer of debugging had been
real and necessary. None of it was the actual blocker. The actual blocker
was one checkbox.

## What actually changed, over the whole thing

Not really the code — the code turned out fine each time, eventually. What
changed was the standing order of operations for a new problem: check the
compiler/toolchain first, verify with the device rather than trust a
capability list, test the risky assumption cheaply before building on top
of it, and commit a fix the moment it's confirmed rather than "later." All
five of those showed up more than once, in different disguises, and every
recurrence was cheaper than the first time specifically because it wasn't
being solved from scratch again.