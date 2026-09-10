# 12ProScrcpy — Integration Plan: Real-Device `sendevent` Touch Injection

**Project:** 12ProScrcpy (QtScrcpy fork)
**Target device:** `a010f622` — Xiaomi 12 (zeus), rooted, SELinux permissive required
**Goal:** Replace scrcpy's normal control-channel touch injection (the socket-based `MotionEvent` protocol the bundled `scrcpy-server` normally uses) with raw `/dev/input` `sendevent` calls issued directly over adb, using the device-specific protocol and coordinate mapping already confirmed working in `real-device-adb-sendevent.md`.

---

## 1. Why this is a different code path than normal scrcpy touch

Stock QtScrcpy/scrcpy touch works like this:
```
Qt UI mouse event → controller/inputconvert (QtScrcpy) → control socket → scrcpy-server (on device) → Android InputManager.injectInputEvent()
```
This goes through Android's **software** input injection API (`InputManager`), which is a synthetic event path some apps and games can detect and reject (many anti-cheat / anti-automation checks specifically look for `FLAG_INJECTED` markers InputManager attaches).

`sendevent` instead writes **raw kernel input events directly to the `/dev/input/eventN` device node** — the same interface the physical touchscreen driver itself uses to report real touches. From the app/game's perspective, this is indistinguishable from a real finger, because it *is* the same kernel path a real finger uses. This is the entire reason to bypass the existing `controller/inputconvert` + `scrcpy-server` pipeline for touch specifically.

**Trade-off to keep in mind:** this only works because you have root + SELinux permissive on this specific device. It is not portable to non-rooted devices the way stock scrcpy control is. This feature branch should be treated as a **rooted-device-only mode**, not a replacement for the default control path — 12ProScrcpy should keep the stock path as the fallback for unrooted devices.

---

## 2. Where this plugs into the existing codebase

You've already identified the relevant files:

```
QtScrcpy/QtScrcpyCore/include/adbprocess.h
QtScrcpy/QtScrcpyCore/src/adb/adbprocess.cpp
QtScrcpy/QtScrcpyCore/src/adb/adbprocessimpl.h
QtScrcpy/QtScrcpyCore/src/adb/adbprocessimpl.cpp
```

This is the correct layer. `AdbProcessImpl` is where every real `adb`/`shell` command gets built as a `QStringList` and handed to `QProcess::start()`. New touch-injection methods belong here, following the exact same pattern as `push()`, `install()`, `forward()`, etc.

The **consumer** of these new methods will be wherever mouse/touch events currently reach `controller/inputconvert` (in `QtScrcpy/QtScrcpy/`) — that layer needs a new branch: "if rooted sendevent mode is active for this device, call the new `AdbProcess` sendevent methods instead of sending a control-socket message."

---

## 3. Critical performance problem to solve first

Every `sendevent` call in the reference doc is issued as its own separate `adb shell su -c "sendevent ..."` invocation. Each of these spawns a brand-new `adb` process and a brand-new `su` shell — this has real process-spawn latency (tens of milliseconds per call, sometimes more on Windows).

A single tap is 6 events (slot, tracking ID, touch-down, X, Y, SYN_REPORT) = **6 separate process spawns** for one tap. A swipe with 15 interpolation steps is 15 × 4 events = 60 process spawns. This is far too slow for anything resembling real-time control (mouse-drag responsiveness, game input) if implemented literally as shown in the PowerShell reference doc.

**This must not be ported 1:1.** The PowerShell functions in the reference doc were fine for one-off manual testing; they are not fine for a live control loop in the app. Two real solutions:

### Option A (recommended): persistent interactive shell process
Instead of spawning `adb shell su -c "..."` per event, open **one long-lived `adb shell` process** (via `QProcess`, kept running, stdin piped) and write successive `sendevent` lines to its stdin. This avoids repeated process-spawn overhead entirely — only one process spawn for the whole session, not one per event.

This requires a new class, not just new methods on the existing `AdbProcessImpl` (which is built around single-shot `execute()` calls, not a persistent piped session). Suggested addition:

```
QtScrcpy/QtScrcpyCore/src/adb/adbsendeventsession.h
QtScrcpy/QtScrcpyCore/src/adb/adbsendeventsession.cpp
```

Sketch:
```cpp
class AdbSendEventSession : public QObject
{
    Q_OBJECT
public:
    bool start(const QString &serial); // spawns `adb -s <serial> shell` and keeps it open (as root)
    void sendRaw(const QString &devicePath, int type, int code, int value);
    void stop();

private:
    QProcess m_shell;
};
```

`start()` launches something like:
```cpp
m_shell.start(getAdbPath(), {"-s", serial, "shell"});
m_shell.write("su\n"); // escalate inside the persistent shell
```
Then `sendRaw()` just does:
```cpp
QString cmd = QString("sendevent %1 %2 %3 %4\n").arg(devicePath).arg(type).arg(code).arg(value);
m_shell.write(cmd.toUtf8());
```
No new process per event — just a write to an already-open stdin pipe. This is the difference between "60 process spawns per swipe" and "1 process spawn per session, 60 cheap writes."

### Option B: batch a whole gesture into one shell call
For discrete gestures (a single tap, a single swipe) rather than continuous freeform control, you can build the entire sequence of `sendevent` calls as one semicolon-joined shell string and send it as a single `execute()` call, matching the existing `AdbProcessImpl` pattern with no new persistent-process class needed:

```cpp
void AdbProcessImpl::realTap(const QString &serial, const QString &devicePath, int slot, int trackId, int x, int y)
{
    QString shellCmd = QString(
        "sendevent %1 3 47 %2; sendevent %1 3 57 %3; sendevent %1 1 330 1; "
        "sendevent %1 3 53 %4; sendevent %1 3 54 %5; sendevent %1 0 0 0; "
        "sleep 0.05; "
        "sendevent %1 3 47 %2; sendevent %1 3 57 -1; sendevent %1 1 330 0; sendevent %1 0 0 0"
    ).arg(devicePath).arg(slot).arg(trackId).arg(x).arg(y);

    QStringList args;
    args << "shell" << "su" << "-c" << shellCmd;
    execute(serial, args);
}
```

**Note on the doc's PowerShell gotcha:** the reference doc's warning about `for`/`do`/`done` breaking was specifically about *PowerShell's* quoting mangling shell **control-structure keywords** when double-nested through its own parser. Semicolon-joined simple commands (no `for`/`if`/loops) inside a single C++ `QString` passed straight into `QProcess::start()`'s argument list does **not** go through PowerShell's parser at all — that gotcha is specific to typing commands manually in a PowerShell terminal, not to argument strings built and passed programmatically from C++. Semicolon chaining like the tap example above is safe here.

**Recommendation:** implement **Option A** (persistent shell session) for anything continuous (drag, freeform mouse-follow, swipe with live interpolation) since that's where per-event latency actually matters for responsiveness. Use **Option B** (batched single call) for discrete one-shot gestures (single tap, long-press, a canned swipe/pinch) where the entire event sequence is known upfront and a single command with a short `sleep` in between is simpler than managing a persistent process. You likely want both — Option A backing live control, Option B backing "gesture macro" style actions.

---

## 4. SELinux permissive — where to trigger it

Per the reference doc, none of this works with SELinux `Enforcing` (root alone is insufficient). This needs to run once per device-connect, before any sendevent call is attempted:

```cpp
void AdbProcessImpl::setSelinuxPermissive(const QString &serial)
{
    QStringList args;
    args << "shell" << "su" << "-c" << "setenforce 0";
    execute(serial, args);
}
```

Call this once in whatever device-connection setup routine already runs (`device`/`AndroidDevice` init code, wherever `push`/`install`/`forward` currently get called during session bring-up), gated behind a "rooted sendevent mode enabled" flag — **do not** call this unconditionally for every device, since it's a device-wide security posture change and should only apply when the user has explicitly opted into this mode for a rooted device they control.

Since this doesn't persist across reboots (per the doc), it should be re-triggered every time a new adb session/connection is established, not just once ever.

---

## 5. Device-specific values — do not hardcode globally

The reference doc's values (`/dev/input/event6`, X range 0–14399, Y range 0–31999, `BTN_TOUCH` requirement) are specific to **this exact device's** FocalTech touchscreen driver. A different phone will have a different event node number and a different coordinate range entirely.

Since 12ProScrcpy (as a QtScrcpy fork) may eventually run against devices other than the zeus, structure this as a **per-device profile**, not a hardcoded constant:

```cpp
struct SendEventTouchProfile {
    QString devicePath;      // e.g. "/dev/input/event6"
    int xMax;                // e.g. 14399
    int yMax;                // e.g. 31999
    bool requiresBtnTouch;   // true for this driver
    int maxSlots;            // e.g. 10 (0-9)
};
```

For now, hardcode a single entry for zeus/`a010f622` as the default/only profile, but leave the structure in place so a second device later is a config addition, not a rewrite. A reasonable first-pass approach: auto-detect via `getevent -pl` or `cat /proc/bus/input/devices` on connect and try to auto-identify the touchscreen node by driver name (`fts`, `goodix`, `synaptics` are all common Android touchscreen driver names), falling back to a manual profile if auto-detection fails. This is a "nice to have later" — not blocking for getting the zeus path working first.

---

## 6. Concrete method additions (first pass — zeus-only, hardcoded)

Add to `adbprocessimpl.h` (private section stays private; these go in `public:`):

```cpp
void realTouchDown(const QString &serial, int slot, int trackId, int x, int y);
void realTouchMove(const QString &serial, int slot, int x, int y);
void realTouchUp(const QString &serial, int slot);
void realTap(const QString &serial, int x, int y);
void realSwipe(const QString &serial, int x1, int y1, int x2, int y2, int steps = 15, int stepDelayMs = 15);
void setSelinuxPermissive(const QString &serial);
```

Add corresponding pass-through methods to `AdbProcess`/`adbprocess.h`/`.cpp`, exactly mirroring how `install()`/`push()` are already forwarded from `AdbProcess` → `AdbProcessImpl` — no change in that forwarding pattern, just more methods following the same shape.

Implementation bodies in `adbprocessimpl.cpp` follow the Option B batching pattern from §3 for the discrete gestures (`realTap`, `realSwipe`), using the hardcoded zeus values (`/dev/input/event6`, ranges as documented) directly inline for this first pass.

---

## 7. Wiring into the existing input pipeline

Locate where `controller/inputconvert` currently packages a mouse/touch event into a control-socket message (this is in `QtScrcpy/QtScrcpy/` per the earlier project tree — likely `inputconvertnormal.cpp` or similar, based on the class name seen in the original scrcpy commit history you found earlier). Add a mode check:

```cpp
if (m_useRealSendEvent) {
    m_adb.realTouchDown(serial, slot, trackId, mappedX, mappedY); // or move/up
} else {
    // existing control-socket path, unchanged
}
```

`mappedX`/`mappedY` need to be converted from the Qt widget's mouse coordinates into the device's real touchscreen range (0–14399 × 0–31999 for zeus) — this mapping math likely already exists somewhere in the current inputconvert code (since it already maps Qt coordinates to *some* target range for the control-socket protocol); it just needs to target the sendevent profile's `xMax`/`yMax` instead of whatever range the control-socket protocol expects.

---

## 8. Suggested build/test order

1. Add `setSelinuxPermissive()` and confirm it fires once per connect (check via `adb shell su -c getenforce` after connecting).
2. Add `realTap()` only, wire a temporary test button in the UI (similar to the existing `on_adbCommandBtn_clicked()` you saw in `dialog.cpp`) to fire a single hardcoded tap. Confirm it actually registers on-screen before doing anything else.
3. Add `realSwipe()`, same temporary-button test approach.
4. Only once both discrete gestures are confirmed working, move to the Option A persistent-session class for live mouse-drag responsiveness — this is the most complex piece (process lifecycle management, needs graceful start/stop tied to the mirroring session's connect/disconnect) and should be last, not first.
5. Add the changelog entry once each stage is confirmed working, per your existing changelog practice — worth noting SELinux-permissive-per-session as a callout in the security section given it's a standing risk trade-off, not a one-time note.

---

## Open questions to resolve before starting implementation

- Should sendevent mode be a global app setting, or per-device (since it only make sense for this specific rooted zeus unit right now)? Recommend per-device, stored keyed by serial, defaulting off.
- Does 12ProScrcpy need a UI toggle exposed to you, or is this purely an internal/hardcoded-on mode for now while testing? Recommend hardcoded-on for now, promote to a UI toggle once Option A (persistent session) is stable.
