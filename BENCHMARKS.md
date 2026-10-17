# DiiS performance: before and after

How much faster DiiS runs than the DeSmuMEWii code it started from, measured on the same
in-game savestates.

## Results

| Game | Before: fps | Before: ms/frame | DiiS: fps | DiiS: ms/frame | Speed-up |
|---|--:|--:|--:|--:|--:|
| Super Mario 64 DS | 19.3 | 52.0 | 81.9 | 12.2 | **4.3×** |
| LEGO Indiana Jones | 10.3 | 96.7 | 48.4 | 20.7 | **4.7×** |
| New Super Mario Bros. | 12.4 | 80.5 | 31.2 | 32.0 | **2.5×** |
| Zelda: Spirit Tracks | 10.9 | 91.6 | 12.2 | 81.8 | 1.1× |
| Mario Kart DS | 7.8 | 128.0 | 12.4 | 80.5 | 1.6× |

A real DS runs at 59.8 fps, so anything at or above that is full speed. In this test SM64 DS
runs faster than real time because the frame limiter is off.

- **Before:** the original renderer design: GX for the screens, both CPUs interpreted (no JIT).
  This is the closest stand-in for the original DeSmuMEWii.
- **DiiS:** the current build: ARM9 + ARM7 JIT, GX Fast render mode.

## Why two games barely moved

Spirit Tracks and Mario Kart DS still use a 3D feature the GX renderer can't draw yet, so
every 3D frame falls back to the CPU renderer. That alone costs about 48 ms a frame, and
their 2D also falls back to the CPU in part. Once those features are drawn on GX they should
speed up like the other games.

## Known rendering issues

- **Before:** LEGO Indiana Jones draws its top-screen 3D black, and Spirit Tracks draws its
  bottom screen black. Their timings still count, but those frames weren't drawn correctly.
- **DiiS:** New Super Mario Bros. draws Mario only while he animates (the game itself plays
  normally). Kirby: Canvas Curse is left out because its rendering is badly broken in DiiS.

## How this was measured

- Every run uses the same savestate, which loads about 90 frames after boot. Each result is
  the average over frames 150 to 1050, so all runs cover the same stretch of gameplay.
- Both builds ran in Dolphin 2606a on an Intel i5-1145G7, through the project's benchmark
  harness (`tools/benchmark/run.sh`).
- **Dolphin isn't a real Wii.** It underestimates CPU-side cost, roughly by half on this
  project's measurements. Expect lower fps on hardware for both builds. The speed-ups are
  more trustworthy than the absolute numbers.

| | Before | DiiS |
|---|---|---|
| Build | commit 285f69b | commit e22b52a |
| CPUs | interpreter (no JIT) | ARM9 + ARM7 JIT |
| Renderer | original GX path | GX Fast |
