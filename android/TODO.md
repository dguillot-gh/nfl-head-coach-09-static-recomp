# HC09 Android: to-do

Features to bring over from the PS1 and DS Android ports, plus what HC09 needs on top. Order is roughly
the order to build them in. Targets: **Pixel 8** (leads) and **LG V30** (gate on every renderer change).
Nothing on V30 is locked away: lower defaults are fine, with a "Try anyway" for the rest.

Port phases (from `docs/PHASE1-AUDIT.md` on the USB drive): 2 headless boot, 3 memory, 4 platform,
5 audio, 6 Vulkan renderer, 7 input/UI, 8 perf. The items below land mostly in 7 and 8.

## Launcher (first screen when the app opens)

Same idea as the PS1 `LauncherActivity` and the DS launchers.

- [ ] Small launcher window: game title and art, which disc image is selected, **Play**, **Select game file**.
- [ ] Pick the `.iso` through the Storage Access Framework and read it in place (no 7.8 GB copy); remember it.
- [ ] Check the pick is HC09 (title ID `45410841`) and say so plainly if not.
- [ ] Choose **HC09** or **HC27** (modern rosters) from the launcher. They keep separate saves.
- [ ] **Continue**: start from a save state picked in the launcher (as in the PS1 and PH ports).
- [ ] Settings screen: renderer options below, aspect ratio, resolution, controls, audio.
- [ ] Game runs in its own process (`:game`), like the PS1 layer, so it can quit and start again cleanly.
- [ ] Icon from the box art (the PS1 `tools/icon.ps1` approach).

## Save states

Harder than on PS1/DS: the "machine" is our host layer, not an emulator. Plan:

- [ ] Research first: what has to be captured. Guest memory (only committed pages), guest thread
      contexts (each thread is a host thread running recompiled code, so states can only be taken
      when every guest thread is parked at a known point: in a kernel wait, or a safe point we add),
      kernel objects and handles, open files and positions, GPU register file and ring buffer state,
      XMA contexts, and the renderer's caches (rebuild these rather than save them).
- [ ] Decide if it's feasible. If full states aren't, fall back to strong in-game saves plus a quick
      "suspend" that keeps the process alive in the background.
- [ ] If it works: slots with thumbnails, save/load from the pad menu, **Continue** in the launcher.
- [ ] HC09's own saves (`userdata/content`) must keep working and must never be overwritten by a state load.

## Picture

- [ ] Aspect ratio: **4:3, 16:9 (the game's native), 21:9**. 21:9 needs a widescreen hack (patch the
      game's projection/viewport) like the DS widescreen work; HUD and menus may need anchoring.
- [ ] Fullscreen that fills the screen height, aspect-correct, never stretched (black bars where needed).
- [ ] **Internal resolution / upscaling**: 0.5x, 0.75x, 1x (720p), 1.5x, 2x. HC09 already has
      `HC09_RESOLUTION_SCALE` on Windows. Defaults: V30 lower, Pixel 8 1x or more.
- [ ] Output filter: sharp-bilinear / bilinear / nearest.
- [ ] The extras HC09 added on Windows: SSAO and field reflections, 4x MSAA. Off on the V30 by default,
      optional on the Pixel 8.
- [ ] Device profile and test: at first launch probe the GPU (like `vkprobe`) and pick defaults; a
      setting that fails at run time drops one step instead of crashing (as in the DS ports).

## Controls

- [ ] Gamepad first (Xbox layout): Bluetooth/USB controllers, rumble.
- [ ] Full on-screen **Xbox 360 pad** in the shared PadOverlay style: D-pad, A/B/X/Y, LB/RB, LT/RT,
      Back/Start, both analog sticks with stick clicks. Multi-touch, slide between buttons, haptics.
- [ ] Pad editor: drag to move, bigger/smaller, rename, hide, opacity, reset (saved per game).
- [ ] Hide the pad automatically when a controller or keyboard is used.
- [ ] HC09 is menu-heavy (play calling, career screens): **direct touch** for menus (tap a menu item),
      the way Policenauts got tap-to-point. Wanted (2026-10-30): an add-on after the pad.
- [ ] Pad menu (≡): Save state, Load state, settings, Quit.

## Audio

- [ ] Oboe output (OpenSL ES fallback on the V30's Android 8.0), low latency.
- [ ] Volume balance option (music / effects), from the PS1 wishlist.

## Performance and diagnostics

- [x] **Pipeline compiling on the Pixel 8**: 31 shaders were translated as a `while (pc) switch (pc)` loop
      that Mali's compiler spent up to 50+ min on. All their jumps go forward, so the translator now writes
      them as plain ifs: the whole pre-compile takes 5 s (2026-10-30). A pipeline stalling the pre-compile
      for 60 s no longer holds the game. If it comes back: `VK_EXT_graphics_pipeline_library` on the Pixel.
- [x] V30 freeze at "Select Favorite Team" (2026-10-30): malformed indirect buffers, then WAIT_REG_MEM
      stalled. ARM memory ordering: sync/lwsync/eieio were no-ops; now fences (recompiler patch + CP).
- [x] Coach Now frame time (2026-10-30): the command-processor thread (the whole renderer) sat on a little
      core while the title's spinning job threads held the big ones, ~35-40 us per draw. It now keeps to the
      faster clusters (`HC09_CP_FAST_CORES=0` turns that off): Pixel 8 14 -> 19 fps, V30 12 -> 19 fps.
      `HC09_GPU_STATS` prints ms/frame per Draw phase, acquire/submit/present/fence waits and GPU time.
- [x] Job threads 0x150-0x15C spinning on NtYieldExecution / zero KeDelayExecutionThread (~95% of a big core
      each): on Android a thread yielding >16 times in 1 ms sleeps 100 us (`HC09_YIELD_SLEEP_US`). Per-draw
      cuts (texture/sampler memo per fetch slot, redundant dynamic state skipped, scratch vectors): Pixel
      Coach Now 18.8 -> 27-28 fps (2026-10-30).
- [x] Shaders missing from the APK are saved on the phone (`files/missing-shaders`, pull with
      `adb exec-out run-as com.hc09.recomp cat files/missing-shaders/<f>`), converted with XenosToHlsl +
      shaders_to_spirv.py: 152 shaders now; the Pixel draws a whole game with 0 skipped draws.
- [x] In a game the renderer redid the same work each frame (16 MB of texture hashing, ~550k indices and
      ~8.4 MB of vertices converted). Textures that never changed are checked every 4th frame; vertex and
      index data unchanged for 2 frames is converted once into kept slabs (hashed each frame to stay valid).
      Pixel: the same fps with ~2.5x the draws (20.5 fps at 1,414 draws/frame, was 19 at 596).
- [ ] Pixel in a game: ~33 ms renderer CPU (vertices ~8, mostly hashing 11.6 MB/frame to validate the kept
      copies; constants, descriptors, record, setup ~3.5-4 each), GPU ~22 ms. Next: skip re-hashing
      long-stable buffers (write-watch on guest pages would make this exact), fewer descriptor writes.
- [ ] V30 refuses pipelines: Adreno's log says "Bind group info mismatches the shader source for symbol
      texture2_2d" (also texture6, FloatConstants, ...). Bindings are now visible only to the stages that
      declare them (fixed C0DEA8AD11DC6C29 and 51ADA21A38FAF13D on Coach Now), but ~15 pairs still fail in a
      game; the fallback slots (t48+, `textureN_2d`) are the prime suspect.
- [ ] V30 GPU hang going from the menus into a game (frame ~3000-3300, as a burst of new pipelines first
      draws): every submit fails after it (IOCTL_KGSL_GPU_COMMAND "Resource deadlock"). Not ps
      E44B4D4FF40B0260 alone (skipping it still hangs). Bisect that scene's draws (HC09_SKIP_SHADERS / SKIP_DRAWS).
- [ ] V30 3D geometry explodes in a game (spikes and slabs) where the Pixel draws the same meshes fine:
      vertex fetch / skinning on Adreno (the Adreno 5xx SPIR-V rewrites? bone indexing into b0?). Turf shows a
      criss-cross pattern. Needs a frame dump.
- [ ] Pixel: flat black arms/hands and a grey head seen once on the sideline (texture or format mapping).
- [ ] Per-device profiles (V30 vs Pixel 8) and an FPS counter option.
- [ ] Logs and crash reports to `Android/data/<pkg>/files/logs` (stack traces via unwind).
- [ ] Keep HC09's debug switches usable on Android (`HC09_PRESS` scripted input, fence/stack tracing)
      through an env-style settings file.
- [ ] Battery/thermal: cap at the game's own 30 fps (it runs 30-35 fps on Windows) and don't spin.

## Not planned

- Xbox Live, voice chat, marketplace (stubbed on Windows too).
- Auto-update from GitHub (Windows rebuilds from source; doesn't apply on a phone).
