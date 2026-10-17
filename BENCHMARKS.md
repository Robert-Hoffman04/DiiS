# DiiS performance: before and after

How much faster DiiS runs than the DeSmuMEWii code it started from, measured on the same
in-game savestates.

## Results

| Game | Before: fps | Before: ms/frame | DiiS: fps | DiiS: ms/frame | Speed-up |
|---|--:|--:|--:|--:|--:|
| Super Mario 64 DS | 19.3 | 52.0 | 59.8 | 16.7 | **3.1×** (capped) |
| LEGO Indiana Jones | 10.3 | 96.7 | 33.5 | 29.8 | **3.3×** |
| New Super Mario Bros. | 12.4 | 80.5 | 22.1 | 45.2 | **1.8×** |
| Zelda: Spirit Tracks | 10.9 | 91.6 | 24.7 | 40.5 | **2.3×** |
| Mario Kart DS | 7.8 | 128.0 | 19.1 | 52.5 | **2.4×** |

A real DS runs at 59.8 fps, so anything at that rate is full speed. SM64 DS hits the frame
limiter, so its real headroom is higher than the table shows.

- **Before:** the original renderer design: GX for the screens, both CPUs interpreted (no JIT).
  This is the closest stand-in for the original DeSmuMEWii.
- **DiiS:** the current build: ARM9 + ARM7 JIT, GX Fast render mode, measured on a real Wii.

**The two columns come from different machines.** The DiiS numbers are from a real Wii. The
"before" numbers are from Dolphin. Dolphin underestimates CPU cost by roughly half, which flatters
the old interpreter-only build most, so the real speed-ups are likely larger than shown.

## What still holds games back

Spirit Tracks and Mario Kart DS now draw their 3D on GX. Their remaining cost is mostly 2D
features the GX compositor can't draw yet, which fall back to the CPU:

- Spirit Tracks: OBJ mosaic, on both screens, nearly every frame.
- Mario Kart DS: OBJ window on the top screen.
- LEGO Indiana Jones: blended bitmap OBJs on the bottom screen.

## Known rendering issues

- **Before:** LEGO Indiana Jones draws its top-screen 3D black, and Spirit Tracks draws its
  bottom screen black. Their timings still count, but those frames weren't drawn correctly.
- **DiiS:** New Super Mario Bros. draws Mario only while he animates (the game itself plays
  normally). Kirby: Canvas Curse is left out because its rendering is badly broken in DiiS.

## How this was measured

- Every run starts from the same gameplay savestate per game.
- **DiiS:** a real Wii, with the build's frame-time log (`-DDIIS_PERFZONES`, one log per game
  on the SD card). Each result is the median 60-frame block of the run, skipping the first
  three blocks (savestate load).
- **Before:** Dolphin 2606a on an Intel i5-1145G7, through the project's benchmark harness
  (`tools/benchmark/run.sh`), averaged over frames 150 to 1050.

| | Before | DiiS |
|---|---|---|
| Build | commit 285f69b | commit fdcb4d7 + GX Fast translucency/fog/shadow fix |
| Platform | Dolphin | Wii |
| CPUs | interpreter (no JIT) | ARM9 + ARM7 JIT |
| Renderer | original GX path | GX Fast |
