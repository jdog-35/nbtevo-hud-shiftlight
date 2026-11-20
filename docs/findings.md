# Findings

The lab notebook of this project: what was measured and decided in the car, newest first. It is kept as written
during the work, so earlier sections describe builds and settings that later ones replaced (the README describes the
current state).

- **Logs:** files named `logs/...` were the author's local logs. They are not published: raw MOST traces contain the
  car's GPS position. The tests use trimmed copies in `tests/data/` (engine speed only).
- **"The driver":** the owner of the test car (a 2019 F32 430i, B46, ZF 8HP, NBT Evo ID6 with a 6WA cluster and HUD).
- **Build numbers** (e.g. e5b2d4fc) are the first 8 hex digits of a build's SHA-256.

# Living with the app: first drives (2026-11-19, evening)

**Route tests, parked (2026-11-19 evening):**
- **The BMW navigation's HUD drawing is invisible to our MOST client.** With guidance on, the driver saw arrows and the map on the HUD, but the worker saw no call (with the plugin's `guidanceStatus` 1 at 138 s, 0 at 155 s). So "another client draws" never fires for it.
  - Before the fix, in SPORT the bars would have fought its picture. After the route the navigation clears the HUD, wiping our prepared pictures while the worker thought they were there. That alone can explain "no bars until ticked again".
  - **Fix (worker ed1b4c84):** step aside while `navigation.guidanceStatus` > 0 (if a stage shows, only the panel goes off), and draw again from scratch 5 s after it is 0. Re-test: aside at 356 s (status 1), status 0 at 368 s, "drawing again" 372 s, drawn 379 s.
- **CarPlay route:** stepped aside at 270 s (route file), route ended 285 s, drawn again 292 s. No HUD call while aside.

The driver's report after a session of shifts:
- **High shift points are unusable.** At 7,000 the car held revs and cut power before stage 3. At 6,750 orange came ~200 ms before that, the shift barely made it and hesitated. 6,500 felt right. The limiter / rev-hold sits below the nominal 7,000 redline. So the menu goes up to 6,500.
- **Once, at 5,500, floored in 3rd from ~4,000: nothing, then straight to orange.** Most likely a kickdown to 2nd. The revs jump ~4,000 → ~6,000 (3rd/2nd = 1.49) in a few hundred ms. The white stages come and go within one or two readings at that rate, so orange right away is the right answer. Not changed.
- **Once, after a start and some waiting, no bars at 5,500;** the menu showed "Shift light" ticked, and ticking it again helped.
  - The likeliest cause: the worker had stepped aside earlier (another client drew on the HUD), and that waited for the next iDrive start or tick. When the car only sleeps, iDrive does not restart, so the next drive started with the light still aside. The logs were in RAM, so it can't be confirmed.
  - **Fix (worker f05dde6f):** it no longer waits for a restart. It comes back once `navigation.guidanceStatus` (iDrive car data) is 0 and the HUD has been quiet 5 s; with no status, after 60 s quiet; at the latest after 300 s quiet.
  - **Event log (plugin a1a21558):** rare events now also go to `events.log` on flash, so the next hiccup can be read after a reboot. The plugin also subscribes `navigation.guidanceStatus` (`{"guidanceStatus":%hu}`) and `vehicle.statePWF` (`{"statePWF":%d}`), found in `libhmiremoting.so`.

# Step 6 results: the Shift Light app in My Vehicle (2026-11-19)

**Installed:** `/var/opt/hmi/data/ID5/Plugins/ShiftLight`, connector ID 230. Final: plugin 96b23738, `ShiftLight.xml` 182a248f, manifest 338b46e3, worker 3baeb3d2. Four iDrive starts, every one counted good by the boot guard.

**Plugin in iDrive (HmiMain3D):**
- st1ng's skeleton (vtable copy, 41 slots, onInit at slot 22) works on this firmware.
- The car's frameworks have all the symbols: `PluginApp::post` 0x14, `cdsAddPropertyChangedEventHandler` 0x4c, `bmw::Runnable` vtable 0x14, `PluginApp` vtable 0xac.
- The menu: a My Vehicle entry (VehicleInformation) with plain lists, checkboxes and radio buttons, inline texts. It shows and saves as designed.

**Starting a process from iDrive:**
- `spawnl(P_NOWAITO, "/bin/ksh", ...)` returned −1 every time.
- QNX `spawn()` works, the way ID6's plugins call it: an fd map of 3 (only stdin/stdout/stderr), a `struct inheritance` of 88 bytes, flags 6 (SETSIGMASK | SETSIGDEF; sigmask at 8, sigdefault at 16), and `waitpid(pid, 0, 64)`, WNOHANG = 64. All of these were read from ID6's compiled `libShiftDeck.so`.
- The worker started 24 s after boot, drew at 30–31 s, and the blips were fine.

**Car data (CDS, read from a plugin):**

| Property | Values |
| --- | --- |
| `driving.mode` | ECO PRO 7, COMFORT 3 (also at start-up), SPORT 4, SPORT+ 5 |
| `driving.mode35up` | never reports |
| `driving.gear` | P 3, R 2, N 1, D 5; **S and M1 also 5** |
| `driving.shiftIndicator` | 0 in P, D, S and M1 |

- The other CDS properties `libhmiremoting.so` offers are listed in it. There is no gearbox-mode property.
- **No manual mode on MOST either:** a 90 s trace while switching D/S/M1 (`logs/2026-11-19-trace-gearbox.txt`) showed no message following the switches. The selector likely reaches iDrive over CAN only. So the "Only in manual mode" row was taken out.
- **"Only in SPORT and SPORT+"** (car-map `mode 4 5`): COMFORT → "hidden: not a sport mode", SPORT → "showing", within the 1 s poll.

**ImageGetSize (C04) is not drawing:** st1ng's CarPlay client asks it at every start without a route. The worker no longer counts C04 as another client drawing; otherwise it would step aside for a whole drive.

# Third drive run: orange off at the first falling reading (2026-11-19)

**Run:** `libshiftlight.so` e5b2d4fc, `SL_SHIFT=5500 SL_VERBOSE=1`, 5 min, log `logs/2026-11-19-drive3.txt`. Only 1st and 2nd gear (police about).
- 12 stage changes, 124 cluster answers, 0 errors. It stopped by itself and cleared the HUD.
- Day/night switched twice (night at 31.8 s, day at 39.9 s) and the orange was redrawn each time.
- **Upshifts:** all 3 went 3 → 0, logged "shift". Each was cut at the first falling reading: ~100 ms after the last reading before the fall (204 ms after the peak at 259.5 s, where the first step fell only 2,300 rpm/s). On the second drive it took 300–410 ms. The first falling step was 3,570–7,300 rpm/s (closest to the 3,000 line at 167.3 s: one reading later at worst).
- No stage in between, no flashes. The steepest non-shift fall was 2,220 rpm/s.
- **Driver:** "From what I saw ... it'll do."

# Second drive run: timed stages, upshift falls (2026-11-19)

**Run:** `libshiftlight.so` 61a33a69, `SL_SHIFT=5500 SL_VERBOSE=1`, 20 min by day, log `logs/2026-11-19-drive2.txt` (every reading). Summary: `python3 -I tools/drive_stats.py <log>`.
- 72 stage changes, 362 cluster answers, 0 errors, 9.8 broadcast samples/s. It stopped by itself and cleared the HUD.

**Upshifts:** all 16 went straight from 3 to 0, each marked "landed". None stepped through a stage; on the first drive 14 of 17 did.
- The revs fell 970–1,560 rpm in 200–310 ms, at a steepest 5,560–8,860 rpm/s per step.
- The steepest fall that was not a shift (a lift-off) was 2,720 rpm/s, so the 3,000 rpm/s line sits between the two. A lift-off past it would also go straight to off, which is harmless.
- Orange went off 300–410 ms after the revs started falling: the reading after the fall ended, plus the HUD's ~250 ms to show it. It could go off at the first falling reading instead, ~0.2–0.3 s sooner, if it feels late.

**Stage spacing:**
- Slow and medium pulls (280–1,300 rpm/s): ~0.2–0.5 s between stages, against 0.8–1.8 s on the first drive. In the slowest pulls stage 1 came at 5,080–5,260 rpm instead of 4,500.
- Stage 2 → 3 ran ~400–500 ms, a bit over 350: the climb slows near 5,500 (the turbo runs out).
- Fast pulls (≥ 1,750 rpm/s): 0.1–0.4 s, as before.
- The spacing is uneven when the rate jumps (e.g. 96 ms then 409 ms at 388 s).

**Faster off at upshifts (driver: "slightly faster ... in line with the request to shift", build e5b2d4fc):**
- **The change:** with timed stages, a shift always lands at "off", so the light now goes off at the first falling reading.
- **Tested on this drive's real readings:**
  - The logic as it drove reproduces the run exactly: 16 upshifts, 72 changes.
  - With the cut, orange goes off 215 ms sooner on average (100–304 ms) at all 16, and nothing else changes.
  - Simulated: off 81 ms after the fall starts on average (164 at most), against 258.
- **Not done:** reacting to the paddle pull itself. That would need a shift or gear signal on MOST. None is known, and finding one would mean a trace while driving, which is the significant cost the owner chose to skip.

**Flashes:** 1 stage 1 flash (5 on the first drive), at 386 s. It came from a ~0.4 s kickdown flare, 2,930 → 4,135 rpm, held 308 ms; the ≥ 300 ms rate can't tell those from a pull. There was also one 1 → 2 → 1 wobble on a kickdown at 309 s before the pull went on to 3.

# First drive run: stages 4,500 / 5,000 / 5,500 (2026-11-19)

**Run:** `libshiftlight.so` 15e66127, 20 min at night, log `logs/2026-11-19-drive.txt`, summary with `python3 -I tools/drive_stats.py <log>`.
- 119 stage changes, 500 cluster answers, 0 errors, 9.8 broadcast samples/s. A stage change was confirmed in a median 256 ms (32–285 ms).
- Driver: "overall pretty good". Two complaints: the stages lag after upshifts in 1st and 2nd gear, and the white stages take too long in 3rd and 4th.

**Upshifts step down through a stage in between.**
- At an upshift the revs fell at 4,760–9,400 rpm/s. Lifting off fell at most 2,515 rpm/s.
- 14 of 17 upshifts went 3 → 2 → 1/0 or 3 → 1 → 0. One sample during the fall (~4,800–5,150 rpm) still met stage 2 (≥ 5,000 − 250 hysteresis), and SL_MINON held that stage ≥ 300 ms.
- The revs landed at ~4,050–4,750, rising again at once.
- The driver wants the samples during the shift ignored, going straight from 3 to the stage where the revs landed.

**The time between stages depends on the gear (fixed 500 rpm spacing).**
- Fast pulls, 2,400–4,300 rpm/s at stage 1 (1st/2nd): stage 1 to 3 took 200–500 ms, 100–400 ms per stage.
- Slow pulls, 280–900 rpm/s (3rd/4th, or part throttle): stage 1 to 3 took 1.7–2.7 s, 0.8–1.8 s per stage.

**Stage 1 flashes.** 5 times stage 1 came on at ~3,850–4,100 rpm for 300–500 ms and the revs went no higher. In each case one 100 ms step of 2,800–5,000 rpm/s pushed the prediction just over 4,500. These were likely kickdown blips or quick stabs of throttle.

**Parked check of 61a33a69 (log `logs/2026-11-19-parked-timed.txt`):** 47 stage changes, 0 errors.
- Free-rev blips rose at ~3,000–4,600 rpm/s, so the stages came at their limits (2,000 / 2,500 / 3,000), as designed.
- They fell at only 800–1,000 rpm/s, far below the 3,000 rpm/s of an upshift. So they stepped down like a lift-off (3 → 2 → 1 → 0, ~0.5 s each), as designed. The upshift skip needs a real upshift on the road.

**Fix built (`libshiftlight.so` 61a33a69, on the head unit since the parked check).** `SL_SHIFT` is the only rpm setting, at the driver's choice ("rather it look nicer").
- **Timed white stages:** 0.7 s and 0.35 s before the shift point at the current climb rate, never more than 1,000 and 500 rpm before it. Tests: 350 ms per stage at 300–1,300 rpm/s, and 1st/2nd as on the drive (115–250 ms).
- **Upshifts:** a fall of ≥ 3,000 rpm/s keeps the stage, then goes straight to where the revs land. With the timed stages that is orange straight to off. Tests: 0 of 600 modelled upshifts went through a stage in between (169 with the drive's logic). The final stage came at most ~180 ms after the revs landed.
- **Flashes:** the rate over ≥ 300 ms stops them only when the blip is shorter than ~300 ms (modelled: 5 → 0 for 200 ms blips, 5 → 5 for 400 ms). A drive with `SL_VERBOSE=1` will show which kind they were.

# Step 5 results: three stages in the car, and how the HUD stacks shapes (2026-11-19)

**Stages run (`libshiftlight.so` d618fd01, SL_SHIFT=3000, night, log `logs/2026-11-19-stages.txt`):**
- 65 stage changes and 316 cluster answers, 0 errors. Calls per change were as designed.
- Driver: "in person it felt great". On video against the cluster's needle, stage 1 came at ~1,350, stage 2 at ~1,600–1,950 and stage 3 at ~2,100 on fast blips. Going down, the needle read ~2,900 when our broadcast said ~2,700.
- So the needle lags the real revs by ~200 ms, which is ~800–1,000 rpm at free-rev rates (4,000–5,700 rpm/s). In real rpm the stages came 100–550 rpm early on blips, by the 150 ms prediction lead. At ~1,300 rpm/s in gear that is ~200 rpm.
- Two quirks:
  - Stage 2 drew its left bar, then its right: two calls, rendered one after the other.
  - Going 2 → 1 blinked: picture 5 was redrawn.

**HUD tests through st1ng's test channel** (`tools/bars.sh`, picture 6):

| Test | Result |
| --- | --- |
| `mask 7`: black (7) over a white bar | **black covers white**: two ends with a gap |
| `unmask`: ImageRemoveItem 0x21F | 3, 4 and 5-byte payloads: Error 05 (invalid length); 6 bytes: no answer, no effect. **Not usable yet.** |
| AddPolygon with an item number already used | does not replace: the new shape is added |
| `order`: black item 5 sent first, white item 2 after | **one whole white bar: stacking is by send order**, not by item number |
| `twobars`: both stage 2 bars as one 8-point shape joined along the top edge | **two bars**: non-convex shapes fill. A really faint grey hairline shows where the join runs; the driver likes it. |

Design from this (the driver's idea): each stage's two bars are one shape (one call, both sides at once). Coming down, a black mask is added on top (one call, no blink). Picture 5 is reset while hidden.

**Stages v2 in the car** (`libshiftlight.so` 46fbc3c7, SL_SHIFT=3000, night, log `logs/2026-11-19-stages-v2.txt`):
- 49 stage changes and 258 cluster answers, 0 errors. The 8-point shapes and black masks were all accepted.
- Driver: stage 2's bars now appear **both at once**, and stage 2 → 1 is a **clean shrink, no blink**.
- Calls per change:

  | Calls | Changes | Which |
  | --- | --- | --- |
  | 1 | 21 | |
  | 2 | 19 | a colour call plus one shape |
  | 3 | 1 | |
  | 5 | 8 | going to 0: panel off, then picture 5 reset while hidden |

# Step 4 results: the proof of concept in the car (2026-11-19)

Tool: `shiftlight/` → `/var/opt/shiftlight-dev/libshiftlight.so`. Parked, engine running, no route, CarPlay connected. Logs: `logs/2026-11-19-*.txt`.

**It works without the cable, on our own MOST client.**
- The cluster accepted every call: 265 answered calls across 4 runs (33 + 43 + 41 + 148), 0 errors.
- The bar showed and hid as the driver revved and feathered around 3,000. The driver said it felt "pretty dang responsive", and the bar went away about as the revs left 3,000.
- The HUD was cleared after every run.

**HUD call timing:**
- The cluster confirms each call (ResultAck) 240–275 ms after it is sent, median ~258 ms. That is the same for all three show/hide methods: `frame` (panel 1), `buffer` (picture 6) and `switch` (5↔6).
- By eye the bar is quicker than that, so the confirmation comes after the drawing.
- The driver saw no difference between `frame` and `buffer`, while `switch` maybe felt a little off. `frame` stays the default.
- Waiting for each confirmation makes the set-up take ~3.3 s for 13 calls. The show/hide calls themselves don't wait.

**rpm from a broadcast: no DME requests at all**

- **Source:** device 0x5000 (FBlock 05.00, the vehicle) sends **CD0 Status** every ~101 ms (95–120 ms). The data is a big-endian u16 of rpm × 4, the same scale as OBD.
- **Fresher than OBD:** 515 of 591 messages carry a new value, against 279 of 820 OBD readings. It runs **~100 ms ahead** of the OBD reading (r = 0.993).
- **Subscription:** our client only sees it with `SetNotification` 0x80000001…7 and 0xC0000003 (st1ng's kSubs, `SL_ALL=1`). Without them the server delivers only what is addressed to us.
- **Recipient:** it is addressed to HMI device 0x2021. We read a copy and send nothing.
- **Test run:** 588 samples in 62 s, OBD fallback never needed.

**Day/night: confirmed against the headlight switch**

| Signal | Day | Night | Notes |
| --- | --- | --- | --- |
| 0x5000 `05.00 C31` Status | 01 | 02 | Sent on a change. A property Get (op 1, no data) answered in 12 ms. |
| HMI → CarPlay `24.70 CD7` | 00 | 01 | Matches st1ng's "non-zero = night". |

- Auto lights at night count as night, and auto → on caused no change.
- The tool draws orange 4 at night and 8 by day, and redraws on a change (4 calls, ~80 ms).
- **Known effect:** switching the lights while the bar is up shows it white for ~0.5 s (index 4 is white by day, 8 is pale at night) until the redraw lands.

**Other traffic** (`SL_ALL=1`, 60 s parked): 6,795 messages, none lost.
- No 82.00 drawing calls from anyone at idle, so watching for the BMW navigation causes no false step-asides.
- Our own HUD calls come back with source 0x2000 and are recognised by function, length and payload hash, as st1ng does it.

**MOST registry (109 entries):** the parts that matter here.
- **Our targets:** 82.00 HUD at 0x0101; 06.F0 tunnel at 0x1002; 05.00 vehicle at 0x5000.
- **CarPlay / HMI:** 24.70 at 0x202d.
- **Full list:** `logs/2026-11-19-trace.txt`.

**Scripts:** `tools/analyze_trace.py` (fields vs rpm, rarely changing fields) and `tools/cd0_lag.py`.

# Step 3c results: where bars can go, and the chosen look (2026-11-18)

Method: `tools/bars.sh` through the same test channel, picture buffer 6; the driver read the HUD. Every call was accepted (`op d`).

**Drawing area:** the navigation picture (buffers 5/6, 126 × 77) sits above and to the right of the speed reading. Visible part:

| Test | Seen |
| --- | --- |
| Outline at the buffer edges (x 0–2 / 123–125, y 0–2 / 74–76) | only the top edge |
| Outline inset 12 (x 12–113, y 12–64) | top, bottom, left fully; right edge faint, lower part only |
| Top row ruler (y 0–3) | bar x 14–73 whole; segments 76–83, 86–93 seen; 96–103 barely, cut off |
| White bar at y −8…−3 (above the buffer) | accepted by the cluster, not shown: drawing is clipped to the buffer |

- Visible: x ≈ 12…96 at the top, reaching ≈ 111 lower down (the right side is slanted); y from 0 down to between 64 and 73.
- The left end fades softly (edge of the visible window and/or the HUD eyebox).
- **y = 0 is the highest a bar can go.** The HUD space above (where the media list shows) belongs to the cluster, not to 82/00.

**Chosen look (the driver, after four previews):** bars along the top, x 21–95, all stages 6 px tall (y 0–5). The driver kept 21–95 over the widest possible span (~14–96, which has more fade at the left).

| Stage | Look | Colour index |
| --- | --- | --- |
| 1 | a bar at each end, 20% of the span each | 6 white |
| 2 | a bar at each end, 35% each (gap ~30% in the middle) | 6 white |
| 3 (shift) | one bar across | orange: 8 by day, 4 by night |

- Rejected: grey (3) for stage 1 (invisible at night), 2 px bars (too faint by day), white as the shift colour (orange reads as more urgent), orange 8 at night (weak; 4 is solid).
- Needs day/night detection to pick 8 or 4. st1ng's hud.h watches MOST messages for night mode (FB 24/70 fkt CD7, FB 05 fkt C31; "not seen in a car yet").
- In the previews each stage was redrawn (RemoveAll, colour, line width, 1–2 polygons, text: 5–6 calls). For speed, the final tool should draw the stages ahead of time and switch them with single visibility calls (buffers 5/6, and 7/8 on the same coordinates).

# Step 3b results: HUD fill colours, seen in the car (2026-11-18)

Method: st1ng's colours probe through the CarPlay plugin's test channel (`tools/colours.sh`): a filled square (40,20)-(80,63) in picture buffer 6 and the index as digits in the distance text. The cluster answered `op d` (accepted) to every call, all indexes 0–31. the driver read the HUD from the driver's seat, engine running, parked, no route.

| Index | Night (headlights on) | Day (headlights off) |
| --- | --- | --- |
| 0, 1 | nothing | nothing |
| 2 | fairly solid orange | slightly translucent white |
| 3 | very faint orange | faint grey |
| 4 | solid orange | fairly solid white |
| 5 | medium orange (between 2 and 4) | orange, ~50% |
| 6 | **white** | **most solid white** |
| 7 | nothing | nothing |
| 8 | orange, between 2 and 3 | **orange, ~70% (most solid orange)** |
| 9, 10 | nothing | nothing |
| 11 | orange, a bit more solid than 8 | orange, about like 5 |
| 12–15 | nothing | nothing |
| 16–31 | nothing | nothing (16–19 re-run and seen) |

- At night the HUD maps its palette onto shades of orange (the speed reads orange too). Index 6 stays white: the only non-orange colour at night.
- "Nothing" may be black or a very dark colour: black emits no light on a HUD. NBT-AAD's F20 sweep had 7 = black.
- **No red found** in 0–31, day or night: all 32 indexes seen in both modes. Orange in both modes: 5, 8, 11. White in both modes: 6.
- Shift light colour plan from this: orange bars = 8 (or 5/11), final "shift" stage = 6 (white in day and night) and/or blinking. Red would need another route (e.g. ImageAddBitmap, not tried).
- Picture buffer 5/6: 126 × 77 (ImageGetSize). Where it sits on the HUD and how much of it is visible beyond the arrow box (36,16)-(89,69) is not known yet: needed for bars "along the top".

# Step 3 results: rpm over MOST, measured in the car (2026-11-18)

Tool: `shiftlight/src/slrate.c` → `/var/opt/shiftlight-dev/libslrate.so`. Parked, engine idling, ENET in (only for SSH), no route, no ID6 workers running, CarPlay connected (mm-ipod up).

| Run | Requests | Replies | Round trip (min / median / p90 / max / mean) | Rate |
| --- | --- | --- | --- | --- |
| OBD `01 0C`, idle | 300 | 300 | 61 / 69 / 78 / 88 / 70.2 ms | 13.9 replies/s |
| OBD `01 0C`, throttle blips to 3,156 | 170 | 170 | 60 / 69 / 79 / 90 / 71.0 ms | 13.7 replies/s |
| UDS `22 F40C` | 5 | 0 | negative `7F 22 31` (out of range), also ~70 ms | stopped |

- The cable-free path works: open the MOST client in 7 ms, then the tunnel 06/F0 at device 0x1002 in 7 ms.
- Round trip is ~70 ms whatever the reply, so it's tunnel transport, not DME work.
- **The DME refreshes its OBD rpm only every ~200 ms.** During blips the value changed every 2–3 readings (140–210 ms apart, mean 227 ms incl. steady stretches); 88 of 142 intervals above 1,000 rpm repeated the last value. So: ~5 fresh values/s, not 14.
- Free-rev blip in Park rose ~4,300 rpm/s (852 → 3,077 in 420 ms). Under load in 2nd it will be far slower (~1,300 rpm/s estimate).
- Registry: HUDTerminal 82/00 is at device 0x0101 (the cluster), as CLAUDE.md says.
- Log of the blip run: `logs/2026-11-18-slrate-rev-blips.txt`.

## What it means for the shift light

Age of the rpm we act on: DME hold 0–200 ms + reply leg ~35 ms + wait for our next reading 0–70 ms, i.e. **~170 ms on average, ~300 ms worst**, plus the HUD call.
At 1,300 rpm/s that is ~220 rpm late on average, ~400 worst.

- Trigger on a prediction: `rpm_last + slope × (age + HUD delay) ≥ shift point`, with the slope taken across fresh values (not readings, since 2 of 3 repeat).
- Keep polling back to back (~14/s) to catch each new DME value within ~70 ms of its refresh.
- Worth looking for a faster source before building on this one: a BMW DME DID for engine speed (unknown; none in the three repos) or vehicle data the head unit already gets over K-CAN/MOST. `22 F40C` is not it.

# Step 2 findings (source read, no car)

Repos cloned here:
- `ID6_apps_prerelease` @ d0df14a — `source/` is **MIT**.
- `nbtevo-carplay-advanced` @ 9a80636 (v1.3.0) — **PolyForm Noncommercial 1.0.0**.

## How the ID6 apps read rpm without the cable

Files: `ID6_apps_prerelease/source/native/src/most/*`, `src/diag_link.c`, `src/collector.c` (ShiftDeck part, `SHIFT_DECK_ONLY`).

1. **Fallback:** a loop worker tries HSFZ to 169.254.66.49:6801; after 5 failed connects it opens MOST (`diag_link.c`). With `--transport most` it goes straight to MOST.
2. **Library:** `dlopen("libsys_dsi_base.cpp-ne.so.4")`, then `/opt/sys/lib/...`. C API: `DSIInit`, `DSIClientInit(app,"DSYSMOSTServer","MOSTSubSystem",4,5)`, `DSIClientRun` on its own thread.
3. **Open:** wait for `responseMOSTClient` → register key `0x06F0` (FBlock 0x06, inst 0xF0) → wait for informationConnect. Each phase 2 s max.
4. **Request:** UDS wrapped in RoutineControl `31 01 0F0B`:
   `[63][F0] 31 01 0F 0B 12 <len:2> 01 0C` sent as FktID `0x050`, op 0 to the 06/F0 owner.
   Reply: `71 01 0F 0B 12 <len:2> 41 0C AA BB` → **rpm = (AA·256 + BB) / 4**.
5. **One request in flight**, synchronous (condition variable). `7F 01 78` (pending) extends the wait up to 5 s.
6. **Lease:** all ID6 workers share a loopback UDP lock on 127.0.0.1:42861. On MOST a loop worker takes it per request (waits ≤300 ms). ShiftDeck/BoostDeck running at the same time will compete with us.

ShiftDeck loop (`collector.c` ~line 300): reads `01 0C` (rpm) then `01 0D` (speed) each cycle, cycle padded to **70 ms** (or 20 ms in the `NATIVE_FAST` build). Speed costs a second round trip — we don't need it.

## How fast could it go

Not measured anywhere in either repo. MOST.md only says "ShiftDeck and the gauges refresh more slowly on MOST" and "MOST latency may differ from ENET".

What sets the rate: one tunnel round trip per sample (head unit → MOST → gateway → PT-CAN → DME → back), nothing else, if we poll back-to-back with only `01 0C`.

My estimate, to be replaced by the measurement: **~40–100 ms per read → ~10–25 samples/s.**

Lag budget at 1,300 rpm/s:

| Part | Estimate |
| --- | --- |
| rpm sample age (½ period + round trip) | 50–150 ms |
| HUD show/hide (1 call, cluster ~10 calls/s) | ~100 ms |
| **Total** | **~150–250 ms ≈ 200–330 rpm late** |

So fire the bar at `threshold − rpm_rate × measured_lag`, with rpm_rate from the last 2–3 samples.

## How st1ng draws on the HUD (`src/hud.h`)

- Different DSI library: C++ `libsys_dsi_colibry.gpp.so.8` (`CDSIClient`), not ID6's `libsys_dsi_base`. Both talk to the same `DSYSMOSTServer`.
- Finds the 0x82/00 owner in the registry, sends request `0x02` with `[client][0][1][0][dst][self][82][00][fkt][op=6][seq][len][data]` — the same message layout as ID6's `dsi_send_fn`.
- Paces 20 ms between calls. Steps aside while another client (BMW nav) sent to 0x82 recently (`g_navMs`).
- AddPolygon 0x21D: `[h:2][idx:2][03][n][x:2 y:2]…`, big-endian.
- **Gotcha:** on this HUD-only install the MOST client lives in **mm-ipod, which runs only during a CarPlay session**. So st1ng's test channel (`hud_probe.sh colours`) needs the iPhone connected on CarPlay (no route). Our tool can't rely on mm-ipod for daily use.
- **Run trick:** st1ng runs standalone code as `LD_PRELOAD=lib.so /bin/cat` (constructor starts a thread). That avoids needing QNX crt files to build an executable.

## HUD Gauges (ID6 `packages/hud-gauges.zip`)

- Binary only, no source in the ID6 repo. `hud-shape-qnx` is not stripped; built with QNX GCC 4.4.2.
- Reads rpm over **HSFZ only** (hard-coded 340i gateway 169.254.21.3, `--check-parked`, `--draw-live`). No MOST rpm path, so it can't help with cable-free rpm.
- Its HUD code is `nbt::hudterm` / `nbt::most` from **NBT-AAD** (Required Notice in the zip), PolyForm Noncommercial. That source is public and cloned here as `NBT-AAD` @ f288ab9, in `milestone-7/src/cluster/`.
- Fill colours are picked at run time, so the indexes can't be read from the binary. Its white/orange modes show that orange exists on an F3X HUD.

### Useful from NBT-AAD (`hudterm.hpp`, `hud_terminal.hpp`, `hud6wa_profile.hpp`)

- **Colour is a role index; the cluster's colour scheme picks the RGB.** On their F20 6WA: 4 = white at night (the nav picture colour; orange on a related firmware), 7 = black, 3 = light grey. Red is still unknown: probe.
- 6WA canvas: about (0,0)–(199,129), clipped; glyph box (70,20)–(170,120). st1ng's ImageGetSize said 126 × 77 on this car. Check on the car with ImageGetSize.
- Extra handles: 7/8 are a double-buffered route overlay on the same coordinates as 5/6, and 9/10 are lanes. 7/8 could hold the bar so it's independent of picture 5/6.
- Other calls: ImageAddCircle 0x21E; SetLineColor 0x218; SetFillMode 0x21C. Polygons have at most 99 points. IsVisible 0x205 gets no answer.
- **Rate:** calls are pipelined, with up to 8 in flight and reply timeout 1.5 s. So the "10 calls/s" figure is batch throughput, and one SetVisibility should land faster than 100 ms. Measure it.
- **Nav yield logic:** a route has started once the nav sends SetVisibility(frame 1,1), a switch, an item or text. It ends with SetVisibility(1,0) and then 5 s of quiet. The nav only sends on changes, so quiet alone does not mean the route has ended.
- At route end the nav leaves 3, 4 and buffer 6 visible. Clean up our handles after the nav has finished.

## Shift point

- Redline 7,000. The driver short-shifts (~5,500–6,000; the turbo runs out past 5,500). The gearbox may upshift on its own ~6,000 outside manual mode.
- The trigger point is set from the iDrive menu; the M-style stages are placed relative to it.

## Smallest standalone tool (proposal)

One process, one `.so`, run as `LD_PRELOAD=/var/opt/shiftlight-dev/libshiftlight.so /bin/cat`, built with clang + ld.lld like st1ng's `build.sh`.

- Reuse ID6's MIT DSI client (`dsi_bind.c`, `dsi_client.c`, `most_link.c`, `most_proto.c`), keeping the license notice.
- Mode `rate` (first, read-only): open MOST, loop `01 0C` for 30 s, print min/median/max round trip, samples/s and rpm. Take the 42861 lease per request like ID6 does.
- Later mode `hud`: extend the same client to find the owner of key `0x8200` in the registry and send HUD calls there; probe colours; then draw bar in picture 5, flip with one SetVisibility.

Needs on the Mac: `brew install lld` (no ld.lld yet; clang is there).

Open questions for the car:
- Real round trip of `01 0C` over MOST.
- Whether the HUD and tunnel calls slow each other down.
- Palette indexes for orange/red.
- Redline — confirm with the owner.
