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

- [ ] **Faster pipeline compiling on the Pixel 8** (the first "Preparing shaders" pass took 20+ min: a few
      pixel shaders take Mali 25-45 s each, e.g. C0DEA8AD11DC6C29.ps and 51ADA21A38FAF13D.ps, and are
      compiled again for every vertex shader / render state they pair with):
      1. Quick: more pre-compile workers in `StartPrecompile` (4 -> 8; the game is paused meanwhile, and the
         Pixel has 9 cores).
      2. Big win, Pixel only: `VK_EXT_graphics_pipeline_library` (compile each shader stage once, link
         pipelines cheaply) and/or extended dynamic state (blend, depth/stencil, cull dynamic), so those
         shaders compile once instead of per pipeline. The V30 (Vulkan 1.0) keeps the current path.
      3. Long term: smaller translated pixel shaders from the translator (unrolling the register init,
         spirv-opt -O and dropping the Adreno 5xx rewrites were measured on 2026-10-30: no gain).
- [x] V30 freeze at "Select Favorite Team" (2026-10-30): malformed indirect buffers, then WAIT_REG_MEM
      stalled. ARM memory ordering: sync/lwsync/eieio were no-ops; now fences (recompiler patch + CP).
- [ ] V30 driver refuses 9+ pipelines with VK_INCOMPLETE on the menus (Coach Now: sky and ground black;
      up to ~700 draws/frame skipped). Not size-related (7 KB shaders fail, 85 KB ones build), e.g.
      ps C6100068E4666E5A, 05477D5BC59B1C6F, C0DEA8AD11DC6C29. Find which construct the driver dislikes.
- [ ] Per-device profiles (V30 vs Pixel 8) and an FPS counter option.
- [ ] Logs and crash reports to `Android/data/<pkg>/files/logs` (stack traces via unwind).
- [ ] Keep HC09's debug switches usable on Android (`HC09_PRESS` scripted input, fence/stack tracing)
      through an env-style settings file.
- [ ] Battery/thermal: cap at the game's own 30 fps (it runs 30-35 fps on Windows) and don't spin.

## Not planned

- Xbox Live, voice chat, marketplace (stubbed on Windows too).
- Auto-update from GitHub (Windows rebuilds from source; doesn't apply on a phone).
