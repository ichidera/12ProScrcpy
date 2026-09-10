# Emulating Input on a Real Android Device via `adb sendevent`

**Target:** `a010f622` (physical hardware — Qualcomm "Waipio" platform, Snapdragon 8 Gen1-class SoC, FocalTech `fts` touchscreen, Goodix in-display fingerprint sensor)
**Connection:** USB, `adb -s a010f622 ...`
**Status:** Confirmed working — root + SELinux permissive, power, volume, and tap all verified

---

## 1. Why this target is different from BlueStacks / VMware

This is real hardware, not an emulator. Consequences:

- Hardware functions (power, volume, touch) are split across **multiple physical driver nodes** instead of one virtual device per function.
- **SELinux enforcement is a real, active blocker** — root (`uid=0`) alone was not sufficient; `sendevent` into PMIC-backed key devices produced no visible effect until SELinux was set permissive.
- Touch uses the genuine **Type B multitouch protocol** (slots + tracking IDs), unlike BlueStacks' simplified Type A.
- Coordinate ranges are device-specific: **0–14399 (X) × 0–31999 (Y)** — do not reuse BlueStacks' (0–32767) or VMware's (0–65535) coordinates here.

---

## 2. Root & SELinux

Root access alone did not make hardware-key injection work. The actual blocker was SELinux policy, confirmed via:

```powershell
adb -s a010f622 shell su -c "getenforce"
# → Enforcing
```

Setting permissive (temporary — reverts on reboot) unblocked it:

```powershell
adb -s a010f622 shell su -c "setenforce 0"
adb -s a010f622 shell su -c "getenforce"
# → Permissive
```

**After this, power/volume `sendevent` and touch `sendevent` all worked as expected.**

> **Security note:** `setenforce 0` disables SELinux mandatory access control system-wide on the device, not just for input injection. This is a deliberate, session-scoped trade-off — treat it as something to re-apply each session (or revert with `setenforce 1` when done) rather than a permanent state, since it removes a real security boundary beyond just unlocking `sendevent`.

Device node permissions were also inspected as a secondary signal (`ls -la /dev/input/`):

```
event0  crw-rw----  root:input   gpio-keys        (volume up)
event1  crw-rw----  root:input   uinput-goodix
event2  crw-rw-rw-  root:input   pmic_pwrkey       (world read-write — only one that is)
event3  crw-rw----  root:input   pmic_resin        (volume down)
event4  crw-rw----  root:input   pmic_pwrkey_bark
event5  crw-rw----  root:input   pmic_pwrkey_resin_bark
event6  crw-rw----  root:input   fts               (touchscreen)
event7  crw-rw----  root:input   aw8697_haptic
event8  crw-rw----  root:input   headset jack (switch)
event9  crw-rw----  root:input   headset jack (buttons)
```

`event2` (`pmic_pwrkey`) being the only world-writable node reinforces it as the primary, everyday power key — the `_bark`/`_resin_bark` variants are PMIC sub-functions related to long-press/reset detection, not the normal short-press key.

`/dev/uinput` also exists and is root-accessible (`crw-rw---- uhid:uhid`) — a synthetic virtual input device could be registered here as an alternative to injecting into the real PMIC/touchscreen nodes, sidestepping per-device SELinux/permission quirks entirely. Not needed once SELinux was set permissive, but worth knowing about if a future Android build re-tightens policy around the physical nodes specifically.

---

## 3. Full device inventory (root-verified)

Cross-checked against `/proc/bus/input/devices` and `/sys/class/input/` — identical to the pre-root enumeration, confirming the unprivileged view was already complete (root revealed no hidden devices).

| Device | Path | Bus | Capabilities |
|---|---|---|---|
| gpio-keys | `/dev/input/event0` | platform | `KEY_VOLUMEUP` |
| uinput-goodix | `/dev/input/event1` | virtual | `KEY_HOME`, `KEY_UP/LEFT/RIGHT/DOWN`, `KEY_VOLUMEDOWN`, `KEY_VOLUMEUP`, `KEY_POWER`, `KEY_MENU`, `KEY_BACK`, `KEY_CAMERA`, `KEY_CHAT`, `KEY_SEARCH` |
| pmic_pwrkey | `/dev/input/event2` | SPMI (PMIC) | `KEY_POWER` — primary power key, world read-write |
| pmic_resin | `/dev/input/event3` | SPMI (PMIC) | `KEY_VOLUMEDOWN` |
| pmic_pwrkey_bark | `/dev/input/event4` | SPMI (PMIC) | `KEY_POWER` (long-press/bark variant) |
| pmic_pwrkey_resin_bark | `/dev/input/event5` | SPMI (PMIC) | `KEY_POWER` (combined resin+bark variant) |
| fts (touchscreen) | `/dev/input/event6` | SPI | Full Type B multitouch (see §4) + `BTN_TOOL_FINGER`, `BTN_TOUCH`, and a handful of gesture-shortcut `KEY_*` codes |
| aw8697_haptic | `/dev/input/event7` | I2C | `FF_RUMBLE`, `FF_PERIODIC`, `FF_CONSTANT`, `FF_CUSTOM`, `FF_GAIN` — vibration motor, output only |
| Headset Jack (switch) | `/dev/input/event8` | ALSA | `SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`, `SW_LINEOUT_INSERT`, `SW_JACK_PHYSICAL_INS` |
| Button Jack (headset remote) | `/dev/input/event9` | ALSA | `KEY_MEDIA`, `BTN_1`–`BTN_5` |

---

## 4. Touch protocol: Type B multitouch

`fts` on `/dev/input/event6`:

| Axis | Range | Meaning |
|---|---|---|
| `ABS_MT_SLOT` | 0–9 | Which finger (up to 10 simultaneous contacts) |
| `ABS_MT_TRACKING_ID` | 0–65535, `-1` | Unique ID for a contact; `-1` closes/lifts it |
| `ABS_MT_POSITION_X` | 0–14399 | X coordinate |
| `ABS_MT_POSITION_Y` | 0–31999 | Y coordinate |
| `ABS_MT_TOUCH_MAJOR/MINOR` | 0–14400 / 0–32000 | Contact ellipse size |
| `ABS_MT_WIDTH_MAJOR/MINOR` | 0–127 | Approaching tool width |
| `ABS_MT_ORIENTATION` | -90–90 | Contact ellipse angle |
| `ABS_MT_DISTANCE` | 0–127 | Hover distance (device supports hover-sensing) |
| `BTN_TOUCH` | 0/1 | Explicit touch-down/up flag — this driver expects it alongside the slot/tracking-ID handshake |

Type B frame shape: **no `SYN_MT_REPORT`** (that's Type A's idiom). Just set the slot, set tracking ID/position, then a single `SYN_REPORT` flushes the whole frame.

---

## 5. Reusable PowerShell functions

```powershell
$serial = "a010f622"
$dev    = "/dev/input/event6"

function Send-Real($t, $c, $v) {
    adb -s $serial shell su -c "sendevent $dev $t $c $v"
}

function Real-TouchDown($slot, $trackId, $x, $y) {
    Send-Real 3 47 $slot       # ABS_MT_SLOT
    Send-Real 3 57 $trackId    # ABS_MT_TRACKING_ID (new contact)
    Send-Real 1 330 1          # BTN_TOUCH down
    Send-Real 3 53 $x          # ABS_MT_POSITION_X
    Send-Real 3 54 $y          # ABS_MT_POSITION_Y
    Send-Real 0 0 0            # SYN_REPORT
}

function Real-TouchMove($slot, $x, $y) {
    Send-Real 3 47 $slot
    Send-Real 3 53 $x
    Send-Real 3 54 $y
    Send-Real 0 0 0
}

function Real-TouchUp($slot) {
    Send-Real 3 47 $slot
    Send-Real 3 57 -1          # ABS_MT_TRACKING_ID -1 = lift
    Send-Real 1 330 0          # BTN_TOUCH up
    Send-Real 0 0 0
}
```

---

## 6. Confirmed: tap

```powershell
$x = 7200    # mid of 0-14399
$y = 16000   # mid of 0-31999

Real-TouchDown 0 1 $x $y
Start-Sleep -Milliseconds 50
Real-TouchUp 0
```

**Verified working.** `BTN_TOUCH` was necessary here — this driver also reports `ABS_MT_DISTANCE` (hover), so the explicit touch flag likely distinguishes a real contact from a hover event in a way BlueStacks' simpler Type A driver didn't need.

---

## 7. Swipe / drag

Same down → interpolated moves → up shape as the other targets, using the Type B functions above:

```powershell
function Real-Swipe($x1, $y1, $x2, $y2, $steps = 15, $stepDelayMs = 15, $slot = 0, $trackId = 1) {
    Real-TouchDown $slot $trackId $x1 $y1
    Start-Sleep -Milliseconds 20

    for ($i = 1; $i -le $steps; $i++) {
        $t  = $i / [double]$steps
        $ix = [int]($x1 + ($x2 - $x1) * $t)
        $iy = [int]($y1 + ($y2 - $y1) * $t)
        Real-TouchMove $slot $ix $iy
        Start-Sleep -Milliseconds $stepDelayMs
    }

    Start-Sleep -Milliseconds 20
    Real-TouchUp $slot
}

# Example: swipe up (scroll a feed) from lower-middle to upper-middle of the screen
Real-Swipe 7200 24000 7200 8000
```

For a **long-press-then-drag** (drag-and-drop UI), add a dwell before the first move:

```powershell
Real-TouchDown 0 1 $x1 $y1
Start-Sleep -Milliseconds 300   # let the UI register the grab
# ...then proceed into the move loop as in Real-Swipe
```

---

## 8. Long press

```powershell
Real-TouchDown 0 1 $x $y
Start-Sleep -Milliseconds 700
Real-TouchUp 0
```

To keep the contact looking "alive" during a long hold (some UIs watch for micro-jitter to distinguish a held finger from a stuck sensor):

```powershell
Real-TouchDown 0 1 $x $y
for ($i = 0; $i -lt 5; $i++) {
    Start-Sleep -Milliseconds 100
    Real-TouchMove 0 $x $y
}
Real-TouchUp 0
```

---

## 9. Pinch / zoom (two-finger, real multitouch)

This device genuinely supports it — up to 10 slots (`ABS_MT_SLOT` max 9). A two-finger pinch uses **slot 0** and **slot 1** with independent tracking IDs, moved together:

```powershell
function Real-PinchOut($cx, $cy, $startDist, $endDist, $steps = 15, $stepDelayMs = 15) {
    # open both contacts at the starting distance
    $x1 = $cx - [int]($startDist / 2)
    $x2 = $cx + [int]($startDist / 2)
    Real-TouchDown 0 1 $x1 $cy
    Real-TouchDown 1 2 $x2 $cy
    Start-Sleep -Milliseconds 20

    for ($i = 1; $i -le $steps; $i++) {
        $t    = $i / [double]$steps
        $dist = [int]($startDist + ($endDist - $startDist) * $t)
        $ix1  = $cx - [int]($dist / 2)
        $ix2  = $cx + [int]($dist / 2)

        Real-TouchMove 0 $ix1 $cy
        Real-TouchMove 1 $ix2 $cy
        Start-Sleep -Milliseconds $stepDelayMs
    }

    Start-Sleep -Milliseconds 20
    Real-TouchUp 0
    Real-TouchUp 1
}

# Zoom in around screen center, fingers moving from close together to far apart
Real-PinchOut 7200 16000 1000 6000
```

Pinch-in (zoom out) is the same call with `$startDist` and `$endDist` swapped.

**Why this is more robust than BlueStacks' Type A pinch:** Type B identifies each finger by a persistent `ABS_MT_TRACKING_ID`, not by emission order — so there's no risk of the driver misreading which finger is which if timing jitters slightly. This is the more "correct" multitouch protocol and the one real touchscreen hardware almost always uses.

---

## 10. Hardware keys: power & volume (confirmed working)

All require SELinux permissive (§2) to actually register.

### Power (`event2`, `pmic_pwrkey` — primary key)

```powershell
adb -s a010f622 shell su -c "sendevent /dev/input/event2 1 116 1"
adb -s a010f622 shell su -c "sendevent /dev/input/event2 0 0 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event2 1 116 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event2 0 0 0"
```

### Volume up (`event0`, `gpio-keys` — only exposes up)

```powershell
adb -s a010f622 shell su -c "sendevent /dev/input/event0 1 115 1"
adb -s a010f622 shell su -c "sendevent /dev/input/event0 0 0 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event0 1 115 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event0 0 0 0"
```

### Volume down (`event3`, `pmic_resin` — only exposes down)

```powershell
adb -s a010f622 shell su -c "sendevent /dev/input/event3 1 114 1"
adb -s a010f622 shell su -c "sendevent /dev/input/event3 0 0 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event3 1 114 0"
adb -s a010f622 shell su -c "sendevent /dev/input/event3 0 0 0"
```

**Important:** volume up and down live on two entirely different physical devices here — unlike BlueStacks where both lived on the same keyboard node. Sending both codes to the same device won't work; each direction must target its own node.

---

## 11. Notes & gotchas specific to this target

- **PowerShell multi-line shell loops break.** A `for d in ...; do ...; done` one-liner piped through `adb shell su -c "..."` hit `syntax error: unexpected 'do'` — PowerShell's quoting mangles the semicolons before they reach the Android shell. Prefer single, self-contained commands per `sendevent` call (as used throughout this doc) rather than shell control structures inside the `su -c` string. If a multi-command script is needed, write it to a file on the device first (`adb push script.sh /data/local/tmp/`) and execute that instead of inlining it through PowerShell's quoting.
- **`BTN_TOUCH` matters on this driver.** BlueStacks' Type A driver worked without it; this real FocalTech driver expects the explicit touch flag, likely because it also does hover-sensing (`ABS_MT_DISTANCE`) and needs to distinguish "finger down" from "finger hovering."
- **Root ≠ bypassing SELinux.** `uid=0` alone did not unlock hardware-key injection — SELinux enforcement is a separate, additional gate on top of Unix permissions, and matters more on real hardware than it did on either emulator target.
- **Coordinate range is unique to this device.** 0–14399 × 0–31999 — don't carry over BlueStacks' or VMware's raw values.
- **`setenforce 0` is not persistent** — reverts on reboot, and is a device-wide security posture change, not an input-specific toggle. Treat it as scoped to the current testing session.
