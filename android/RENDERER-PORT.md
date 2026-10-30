# Porting HC09's D3D12 renderer to Vulkan (Android)

Status (2026-10-30, branch `feature/android`): the game runs inside the Android app on the Pixel 8 and
LG V30 at 35 frames a second, with `src/gpu/null_renderer.cpp` dropping draws and the render thread
(`src/android/jni_main.cpp`) clearing the window to a colour that follows `gpu::FrameNumber()`.
This file is the plan for replacing the null renderer with a real one. Background: `docs/` on the USB
drive (`F:\xbox-36-recomp\docs\PHASE1-AUDIT.md`, `PHASE6-RENDERER-PLAN.md`, `PHASE6A-SHADER-RESULTS.md`).

## How the D3D12 renderer works (src/gpu)

- Entry points (`gpu/draw.h`): `ExecuteDraw(const DrawInfo&)`, `ExecuteSwap(frontBuffer, w, h)`,
  `FrameNumber()`, `IsDumpFrame()`, all called on the **command-processor thread**
  (`command_processor.cpp`). `renderer.cpp` (2,731 lines) implements them at the end of the file.
- `Renderer::Draw` (renderer.cpp ~1123-1806): mode control (copy mode means `Resolve()`), shaders from
  `ActiveShader()` -> `GetShader()` (HLSL translate + compile, cached), `PrepareIndices` (topology,
  rectangle lists, index conversion), `GetRenderTarget`/`GetDepthTarget` (one host target per EDRAM
  base + format + samples, 1280x720 x `HC09_RESOLUTION_SCALE`), `GetPipeline` (state hashed into a key),
  float constants (2048 big-endian dwords, swapped and uploaded when changed), `DrawState` constant
  buffer (viewport transform, alpha test, bool/loop constants, texture sizes, vertex stream info),
  vertex streams (`gpuMemory` path = views of a GPU copy of guest memory; **CPU path** = byte-swap the
  reachable dwords into the upload ring), textures via `TextureCache::Get` + samplers from fetch
  constants, viewport/scissor, then `DrawIndexedInstanced`/`DrawInstanced`. Lots of game-specific
  handling: tile-sized clears stretched to full width, fast clears through multisampled EDRAM aliases,
  predicated tiling (first tile pass renders the whole frame), scene camera capture for SSAO/reflections.
- `Renderer::Resolve` (~1808-1980): copies an EDRAM render target rectangle into a resolve target
  texture keyed by destination address; `TextureCache::SetResolved` makes later fetches of that
  address sample it. Depth resolves in `ResolveDepth`.
- `Renderer::Swap` (~2173): presents the **resolve target** whose address is the front buffer (a GPU
  texture, not guest memory), drawn to the back buffer keeping 16:9.
- `texture_cache.cpp` (1,448): untiling (compute shader when guest memory is on the GPU, **CPU otherwise**),
  formats, mips, cube/3D, resolved-texture lookup, replacement images (HC27). `shared_memory.cpp`: the
  GPU copy of guest memory with write-watch (VEH + VirtualProtect).
- `HC09_GPU_MEMORY=0` selects the CPU paths for both vertices and textures (README).

## Decisions for the Vulkan port

1. **New files, D3D12 code untouched**: `src/gpu/vulkan_renderer.cpp` follows `renderer.cpp` section by
   section with D3D12 calls turned into Vulkan; a Vulkan texture cache follows `texture_cache.cpp`.
   `android/native/CMakeLists.txt` swaps `null_renderer.cpp` for them in the app build. Keep his
   comments and debug switches where they still make sense; drop D3D-only debugging (readbacks to BMP)
   at first.
2. **CPU paths first** (no shared-memory buffer, no compute untiling): fewer moving parts and no
   SIGSEGV write-watch yet. Move to GPU paths later for speed.
3. **Shaders**: no compiler on the phone. Ship the SPIR-V built by `tools/shaders_to_spirv.py` in the APK
   assets, named `<microcode hash>.vs.spv` / `.ps.spv` (the same hash `HC09_DUMP_SHADERS` uses), and
   load by hash in `GetShader`; log misses (`[shader] missing <hash>`) and skip those draws.
   Bindings (descriptor set 0): b0/b1 -> 0/1 (uniform buffers), t0-t15 -> 2-17 (vertex streams,
   storage buffers), t16-t79 -> 18-81 (sampled images; t48+ are the 2D fallback views of cube/3D
   fetches), s0-s31 -> 100-131 (samplers). Build each draw's set layout from the translator's info
   (`vertexFetchConstants`, `textureFetchMask`, cube/volume masks) or from the SPIR-V's own bindings.
   The translator info (`TranslatedShader`) is needed at run time too: run `TranslateShader` on the
   phone (it is cheap; only compiling was expensive) and use its fields, loading SPIR-V instead of HLSL.
4. **Descriptors**: `VK_KHR_push_descriptor` exists on both phones: push each draw's descriptors
   instead of managing pools/tables (replaces `GetViewTable`/sampler tables). Samplers: cache
   `VkSampler` objects by their description.
5. **Vulkan 1.0 floor** (LG V30): render passes (cache by target formats + load/store), no dynamic
   rendering, no timeline semaphores. Depth D24S8. MSAA off on the V30 at first.
6. **Rectangle lists** (D3D12 uses a geometry shader, `kRectangleGs`): the Pixel 8 has geometry shaders;
   the V30 does not. Start with the GS on devices that have it (compile `kRectangleGs` to SPIR-V too);
   for the V30, find which draws use rectangles (clears and 2D quads?) and handle them another way
   (e.g. a translator option that emits a vertex shader expanding 3 vertices to 4 by `SV_VertexID`).
7. **Threads**: rendering happens on the command-processor thread (as on Windows). The `VulkanContext`
   moves from the render thread to the renderer; `jni_main.cpp` only hands it the `ANativeWindow`
   (mutex-protected) and stops its colour loop once the renderer is up.
8. **Known shader gaps**: 138 of ~148 shaders captured; on the V30 three fail with `VK_INCOMPLETE`
   (`6A15D0F32F902994.ps`, `51ADA21A38FAF13D.ps`, `DA7401D8BD2E9FA2.vs`). Skip those draws for now.

## Milestones (check both phones each time)

1. Title screen: draws into render targets, a resolve, and `Swap` presenting the resolve target.
2. Menus with the 3D background (depth, blending, textures incl. DXT decode, resolves used as textures).
3. A game on the field (predicated tiling, MSAA off, many draws: watch frame time on the V30).

Test with `HC09_PRESS` scripts (see `docs` and the headless runs: Start at frame 1100, then A every 200
frames reaches the 3D menus) and `HC09_DUMP_FRAME` frame dumps; compare against the Windows build's
screenshots where possible. Build the APK as in `android/build.gradle`'s header (Gradle through
`java -classpath gradle/wrapper/gradle-wrapper.jar`, no `.bat` scripts).
