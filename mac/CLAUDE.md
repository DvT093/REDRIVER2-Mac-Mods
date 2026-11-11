# Driver 2 (REDRIVER2) on macOS - mods

REDRIVER2 = open-source PC port of Driver 2 (PS1), built from the Apple Silicon branch. Diederick plays it and asks for gameplay mods. He is not a developer: answer in Dutch, short, and show results (screenshots), not code.

## Where things live
- Source + git: `src/` (branch `mac-mods`), public on GitHub as [DvT093/REDRIVER2-Mac-Mods](https://github.com/DvT093/REDRIVER2-Mac-Mods) (fork of ebellumat/REDRIVER2, default branch `mac-mods`). `src/src_rebuild/PsyCross` is a submodule on its own `mac-mods` branch, pushed to [DvT093/PsyCross](https://github.com/DvT093/PsyCross): commit and push the submodule first, then the main repo (the submodule pointer). Commit as `6576986+DvT093@users.noreply.github.com`, never the work address. README (Dutch + English mod list, keys) = `src/README.md`: add new mods and keys there.
- Game logic: `src/src_rebuild/Game/C/` (mods marked `// [mod]`). Own files: `pistol.c`, `gps_route.c`, `lamps.c`, `traffic.c`, `farcity.c`, `modtest.c`, `colourdump.c`. PS1 API / renderer: `PsyCross/`.
- Runtime: `game/` (binary, `config.ini`, `mod/gunshot.wav`, `mod/DRIVER2/LEVELS` + `NLEVELS` = extracted level copy for the far city, 83 MB, not in git). `Driver 2.app` starts `game/REDRIVER2` without Terminal.
- Backup of files outside git: `src/mac/` (build.sh, config.ini, app launcher, gunshot.wav, `extract_levels.py`). Copy `game/config.ini` to `src/mac/config.ini` when you change it.
- Feature notes (his task list): Obsidian vault `~/Projects/todo/Obsidian/Initiate/tasks/Driver 2.md` (parent, key table) + one note per feature in `notes & resources/Driver 2/REDRIVER2 - <feature>.md`. Vault rules from `~/Projects/todo/CLAUDE.md` apply there (no blank lines, `## Hoe het werkt` above `## Plan`, tick the parent checkbox when done).

## Build
- Always `./build.sh` from this folder: premake + make (release_arm64) + rm/cp/codesign into `game/`. Copying over the binary without re-signing gets it killed by macOS.
- One build at a time, never in parallel (16 GB laptop; parallel builds crashed it on 2 Nov 2026). Do not run a game test while building.
- `jq` is an asdf shim without a version here: use `python3` for JSON.

## Test before handing over (he expects this)
- `cd game && ./REDRIVER2 -nointro -nofmv -modtest [mode]` drives the game itself, takes screenshots `modtest_*.bmp` and quits. Modes: (none) pistol, `aim`, `sprint`, `exit`, `cops`, `driveby`, `farview` (+ `REDRIVER2_TEST_DRIVE=1`), see `modtest.c`.
- Screenshots are upside down: `sips -f vertical -s format png x.bmp --out ../testshots/x.png`, then look at them (crop/compare with PIL).
- Useful env: `REDRIVER2_TEST_CITY=0..3`, `REDRIVER2_TEST_CAR=0..9`, `REDRIVER2_TEST_TURN`, `REDRIVER2_VIEW_CELLS`, `REDRIVER2_FAR_CITY=0`, `REDRIVER2_NO_SKY=1`, `REDRIVER2_DRAW_DIAG=1`, `REDRIVER2_SPLIT_DIAG=1`, `REDRIVER2_VAN_LIFT`, `REDRIVER2_FE_KEYS="....XDX.S..Q"` (scripted menu input: X/T/U/D/L/R, S = screenshot, Q = quit).
- Run the old regression set after renderer or core changes: default modtest (pistol hits), `driveby` (CARSHOT 4 directions), `farview`.
- Runs are not deterministic (traffic): compare numbers, or toggle a feature with env in the same position.

## Gotchas (each cost real time)
- Custom drawing: save and restore GTE matrices around it (`gte_ReadRotMatrix`/`gte_SetRotMatrix`/`gte_SetTransMatrix`), or Tanner disappears. With PGXP, write screen coords straight into the primitive (`gte_stsxy3`), else nothing shows.
- Y axis: peds/cells/camera up = negative; car `hd.where.t[1]` up = positive.
- Keys for mods are read with `SDL_GetKeyboardState` (not in replays). Up+Space = the game's leave-car combo; moving exit is Enter only.
- Car bitfields are `CARBITS` (128-bit) for `MAX_CARS` 120; `CAR_DATA.id` is a char.
- Far distance: GTE depth is 16-bit (saturates ~65535 units), OT index past `OTSIZE` was silently dropped (`OT_CLAMP`), integer back-face test drops sub-pixel faces (`FarFacing`), renderer far plane 8000. PsyCross used to make one draw split per polygon; keep `AddSplit` free of the depth comparison.
- `config.ini` has no inline comments: `l1=Left GUI   # note` makes the whole rest of the line the key name, so the key silently falls back to its default. Put comments on their own line starting with `;`.
- SDL3 (via sdl2-compat) renames modifier keys once the window is up ("Left GUI" -> "Left Command", "Left Alt" -> "Left Option"); `PsyX_LookupKeyboardMapping` has aliases for both. Releasing Cmd also made SDL drop held arrows, so keyboard reads fall back to the hardware state (`PsyX_KeyDown`). Fast steer (L1) = Left Option. Synthetic Option key events (CGEvent) do not register here: test Option by hand.
- The red triangles at the bottom during a chase are the original cop indicators, not a bug.
- Chicago car slot 8 crashes without `CARMODEL_11_clean.MDL`; keep it out of the car menu.
- 60 Hz screens (LG UltraFine, MacBook Air): the 30 fps limiter plus swap interval 2 gave exactly 50 ms/frame (20 fps), with or without mods (old 30 Oct build measured the same). On the 120 Hz laptop screen it was fine. Fixed in `PsyX_BeginScene` (`PsyX_main.cpp`): 60 Hz -> interval 1. Keep `vsync=1`. A flat 50.0 ms in every test = suspect vsync/screen first, not a commit.

## Share with someone else
- `src/mac/make_app.sh` builds `dist/Driver 2.app` + `dist/Driver 2.zip`: binary, Homebrew dylibs (incl. SDL3, which sdl2-compat loads), game data and disc 1 image all inside the app. Disc 2 is not used. Apple Silicon only. Run it again after every mod build.

## 60 fps (interp.c)
- Sim stays 30 Hz on the real clock (`State_GameLoop60` in main.c); every screen refresh draws, with camera, cars and peds lerped between the previous and current step (`interp.c`). `REDRIVER2_60FPS=0` = old loop. `REDRIVER2_FPS_LOG=1` prints draws/s and steps/s (should be ~60 / 30).
- Camera (`InitCamera` + `DoFreeCamera`) now runs once per sim step, not in `RenderGame2`. `FrameCnt` counts sim steps; `gRenderCnt` counts draws (buffer swap, `ObjectDrawnValue`).
- Anything in the draw path that changes state (particles, timers, flashes, frame counters) must skip when `gInterpFrame` is set: use `FX_FROZEN` (pause.h) or `if (!gInterpFrame)`. New mod drawing code: same rule, or it runs twice as fast.
