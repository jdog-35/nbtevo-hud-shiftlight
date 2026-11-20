# nbtevo-hud-shiftlight

An M-style shift light on the factory head-up display of BMW cars with NBT Evo (iDrive 6). Two white bars grow in from the
edges as the revs climb, and one orange bar runs across at the shift point. It runs on the head unit by itself, with a
menu under **My Vehicle → Shift Light**, and needs no cable, phone or laptop once installed.

> **AI disclosure.** This project was built with AI assistance. The code, tests and documentation were written by
> Claude (Anthropic) in Claude Code, directed by the owner, who tested every step in their own car. Read it and test it
> yourself before you trust it in yours.

> **Likely no support, but discussion is welcome.** I'm not planning to fix every single bug or answer support request:
> unfortunately am running tight on token usage, and am not super comfortable dealing with other car configurations by hand. 
> With that being said, you can start a [discussion](../../discussions) to share
> findings, problems and fixes, so people with other cars can help each other. It's published so others can build on it,
> like the projects it builds on: feel free to fork it and make it yours, within the licenses.

> **At your own risk.** This puts native code into your head unit's iDrive process. It was tested on one car. It never
> writes to the car's control units, but a bug can still make iDrive crash or misbehave. Keep a way back: SSH access and
> the uninstall below.

## What it does

| Stage | When | On the HUD |
| --- | --- | --- |
| 1 | about 0.7 s before the shift point, at the current climb rate | a white bar at each end |
| 2 | about 0.35 s before it | longer white bars |
| 3 | the shift point | one orange bar across |

- **Timed stages:** the white stages come a set time before the shift point, so they feel the same in every gear. In the
  low gears they are capped at 1,000 and 500 rpm before it.
- **Upshifts:** when the revs fall like a shift, orange goes off at the first falling reading, with no stage in between.
- **Day and night:** the orange follows the car's day/night signal.
- **Drive modes:** an option shows the light only in SPORT and SPORT+.
- **Navigation:** it stays off the HUD while a CarPlay route (via nbtevo-carplay-advanced) or the BMW navigation is
  guiding, and comes back afterwards.
- **Shift point:** 5,000–6,500 rpm in steps of 250, from the menu. On the test car, higher points were unusable: the car
  holds revs and cuts power before you can react to orange.

Engine speed comes from the car's own MOST broadcast (FBlock 05.00, function CD0, about every 100 ms). The tool never
sends diagnostic requests in normal use.

## Tested on


- 2019 F32 430i xDrive (B46), ZF 8-speed automatic
- NBT Evo ID6, NBTevo_Y22292I, HMI ID5_2211-490-2222A Build 13, 1280 × 480
- 6WA cluster with the factory HUD (option 610)
- Installed alongside nbtevo-carplay-advanced 1.3.0 (HUD-only mode) and the ID6 apps

Other cars and firmware may differ in the HUD palette, the picture layout, the iDrive framework, the car-data values and
where the limiter sits. In theory, `tools/plugin.sh check` tests the framework part before installing.

## What you will need

- A Mac or Linux machine with `clang`, `ld.lld` (for example `brew install lld`) and Python 3. No QNX SDK.
- An ENET cable, and a way to turn on SSH on the head unit, for example the
  [Bavarian SSH Client](https://ssh.bavarian-aa.uk/).
- An SSH host entry named `hu` that keeps one shared connection, so the scripts don't need the password:

  ```
  Host hu
    HostName 169.254.199.119
    User root
    HostKeyAlgorithms +ssh-rsa
    ControlMaster auto
    ControlPath ~/.ssh/cm-%C
    ControlPersist 4h
  ```

  Open it once with `ssh -fN hu`. After every head-unit restart, run `ssh -O exit hu`, turn SSH on again, and reconnect.

## Build

```
./build.sh
```

| Output | What |
| --- | --- |
| `out/plugin/libShiftLight.so`, `ShiftLight.manifest`, `ShiftLight.xml` | the My Vehicle menu, an iDrive plugin |
| `out/libshiftlight.so` | the shift light itself (installed as `shiftlight-worker.so`) |
| `out/libslrate.so` | a MOST rpm rate test from the early research |

`build.sh` also checks every library's imports, and runs the tests on your machine: the HUD call encodings, the stage logic
on simulated pulls and upshifts, two recordings from the test car, the drive-mode filter and the menu XML.

## Install

Parked, ignition on, SSH connected:

1. **Check:** `tools/plugin.sh check` (read-only). The car's iDrive frameworks must have the functions the plugin uses,
   and connector ID 230 must be free.
2. **Install:** `tools/plugin.sh install`. It stages the files, checks every hash, then moves them to
   `/var/opt/hmi/data/ID5/Plugins/ShiftLight/`.
3. **Restart iDrive:** hold the volume knob until the screen goes dark.
4. **Turn it on:** in **My Vehicle → Shift Light**, tick **Shift light** and pick your shift point.
5. **Drive modes (optional):** the drive-mode numbers vary by car. Run `tools/plugin.sh status` while you switch modes.
   Then write the sport ones, for example `tools/plugin.sh map 'mode 4 5'`, and tick **Only in SPORT and SPORT+**.

**Uninstall:** `tools/plugin.sh uninstall` stops the shift light (it clears the HUD) and moves the folder to
`/var/opt/hmi/data/ID5/PluginBackups/`. Then restart iDrive. To only turn it off, untick **Shift light**.

**Updates:** `tools/plugin.sh update-plugin` (then restart iDrive) or `update-worker` (no restart). Both first copy the
car's current file to `out/archive/`.

## How it works

- **The plugin** (`src/slplugin.cpp`) is a native iDrive plugin app, loaded by HmiMain3D at every iDrive start. It shows
  the menu and keeps the switches as files in its folder. It subscribes to car data (`driving.mode`,
  `navigation.guidanceStatus` and others), and starts the worker with QNX `spawn()`. A boot guard disables the plugin
  after three iDrive starts in a row that end within 20 s.
- **The worker** (`src/shiftlight.c`, `SL_MODE=service`) is a separate process with its own MOST client.
  - It reads engine speed from the broadcast and predicts the revs about 150 ms ahead (`src/logic.c`).
  - It draws the bars ahead of time into the HUD's two picture buffers. So each stage change is one call
    (`src/stages.c`), which matters because the cluster takes about 250 ms to confirm a call.
- **On the HUD:** calls go to the cluster's HUDTerminal (MOST FBlock 82/00) with the encodings in `src/hud.h`. Colours are
  palette indexes: on the test car white is 6, and orange is 8 by day and 4 at night.
- **Logs:** `tools/plugin.sh status` shows the switches, the car data, both logs, and `events.log`. That file keeps rare
  events across restarts.

## Building on it

- **Start here:** `docs/findings.md` is the full lab notebook. It covers the HUD palette and geometry probes, rpm
  latency, the stage tuning drives, the plugin ABI, the car-data values, and what didn't work.
  - For example: manual mode can't be detected on this car, and the BMW navigation's HUD drawing is invisible on MOST.
- **The pieces are separate:**
  - `src/hud.h` (HUD protocol), `src/stages.h` (drawing ahead), `src/logic.h` (timing) and `src/gate.h` (drive-mode
    filter) are pure C, tested on the host.
  - `src/dsi.c` is the MOST client.
- **Development tools** run the shift light by hand, without the plugin:
  - `tools/run.sh` (short parked runs) and `tools/drive.sh` (detached drive runs with a log).
  - `SL_MODE=trace`: records every MOST message, with `tools/analyze_trace.py`.
  - `tools/drive_stats.py`: summarises drive logs.
  - Untick **Shift light** first, so only one client draws on the HUD. They use `/var/opt/shiftlight-dev/` on the head
    unit.
- **Never commit raw MOST traces:** they contain the car's GPS position.

## Credits

This builds on the work and published research of (huge thanks to these guys):

- [nbtevo-carplay-advanced](https://github.com/st1ng/nbtevo-carplay-advanced) by st1ng: the HUDTerminal protocol, the
  freestanding clang build, and the plugin skeleton.
- [NBT-AAD](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) by nanaisu: the plugin ABI, the car-data properties
  and HUD notes.
- [ID6 apps](https://github.com/inotbarnacle/ID6_apps_prerelease): the MOST client, and how a plugin starts its workers.

See [NOTICE](NOTICE) for what came from where.

## License

MIT ([LICENSE](LICENSE)), except `src/slplugin.cpp`, which is under the
[PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt) because it follows code under that
license. Anything built from that file, including the menu plugin, is noncommercial only.

Not affiliated with or endorsed by BMW. BMW, iDrive and CarPlay are trademarks of their owners.
