# README.me.md — personal quick reference

Not for the public repo README — this is just *my* cheat sheet, kept
terse on purpose. The public `README.md` and `docs/DEVELOPMENT.md` have
the full explanations; this is "what do I actually type."
Incase you need it too🤷‍♂️

---

## Every new terminal, before touching cmake

```bat
call "X:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0
```

Forgetting this = silently building with whatever's on `PATH` (MinGW,
wrong SDK). Symptoms if forgotten: `mingw64\bin\c++.exe` in the build log
instead of `cl.exe`, or a broken `mt.exe`/SDK mismatch. **If the build log
doesn't say `cl.exe`, stop and re-run this line.**

## Full clean build

```bat
cd /d X:\Github\12ProScrcpy
rmdir /s /q build
mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="X:\Qt\6.8.0\msvc2022_64"
cmake --build . --config Release
X:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe --release X:\Github\12ProScrcpy\output\x64\Release\12ProScrcpy.exe
```

## Incremental rebuild (code change only, no need to redo `cmake ..`)

```bat
call "X:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0
cd /d X:\Github\12ProScrcpy\build
cmake --build . --config Release
```

## `LNK1104: cannot open file '...\12ProScrcpy.exe'`

Old exe still running. `taskkill /f /im 12ProScrcpy.exe`, relink, done.

## Rebuilding + placing the daemon

```powershell
cd X:\Github\12ProScrcpy\12ProScrcpy\12ProScrcpyCore\src\rawinputdaemon
aarch64-linux-android30-clang -static raw_input_daemon.c -o qtscrcpy_raw_input_daemon
copy qtscrcpy_raw_input_daemon X:\Github\12ProScrcpy\output\x64\Release\qtscrcpy_raw_input_daemon
```

Name and location matter — must be exactly `qtscrcpy_raw_input_daemon`
next to `12ProScrcpy.exe`. Manually `adb push`-ing it somewhere else for a
one-off test is fine, just rename it (`qtscrcpy_raw_input_daemon_manualtest`
or similar) so it doesn't collide with what the app expects, and remember
to `adb shell su -c "pkill -f raw_input_daemon"` when done testing it
standalone so it's not still running when the app tries its own copy.

## Before reporting "daemon not working" to myself in six months

1. **USB tethering ON** (Settings → Network & Internet → Hotspot &
   tethering → USB tethering) — separate from USB *debugging*. This has
   bitten me once already. Check first, always:
   ```
   adb shell su -c "ip -o addr show rndis0"
   adb shell su -c "ip -o addr show usb0"
   ```
   Both empty = tethering's off, full stop, nothing else matters yet.
2. Daemon binary correctly named + placed (see above).
3. **Fully disconnect/reconnect the device in the app** after fixing
   either of the above — the daemon is only attempted once per
   `Controller` lifetime (tried-once latch, deliberate). Fixing the cause
   after a failed attempt does NOT retroactively retry it mid-session.
4. Watch for the actual log line, not just "not available":
   ```
   adb logcat 2>&1 | findstr /C:"raw input daemon"
   ```
   `"raw input daemon connected..."` = working. `sessionError`'s warning
   = tells you which step (push/launch/RNDIS-resolve/connect) actually
   failed — read that message, don't just assume.

## SELinux (matters for the `AdbSendEventSession`/`sendevent` path)

```bash
adb shell su -c "getenforce"          # check
adb shell su -c "setenforce 0"        # fix if Enforcing - NOT persistent, redo every reboot
```

## Windows SDK

`10.0.26100.0` is the known-good pin on this machine. `10.0.28000.0` has a
broken `mt.exe` (silently produces nothing instead of erroring — easy to
miss). If a build ever mysteriously breaks at the link/manifest step after
a Windows Update, check `dir "C:\Program Files (x86)\Windows Kits\10\bin"`
for a new SDK version that might have become the "latest" default before
assuming the code broke.

## Committing

**Commit early, commit often** on anything that fixes a build error or a
gotcha — the recurring pattern this whole project has hit is fixing
something locally, not committing it, and then re-hitting the exact same
already-solved problem after a reconnect/rebuild/reupload. If it took more
than five minutes to figure out, it's worth a commit the moment it works,
not "later."

## When a zip export looks wrong / doesn't match what's building

`scripts/zip_project.py` reads live off disk via `git ls-files -c -o
--exclude-standard` — it's not stale-by-design. If a zip's content doesn't
match what the compiler's actually seeing, the likely cause is the file
wasn't saved yet in the editor at zip time, not a bug in the script. For
anything actively mid-edit, `type <file>` and paste directly instead of a
zip round-trip — faster and can't be stale.