# RAVE + OpenGL 1.2 backend parity checklist

Reference: **PocketShaver** `PocketShaver/SheepShaver/src/gfxaccel/` (`rave_metal_renderer.mm`, `gl_metal_renderer.mm`, `gl_shaders.metal`, `rave_shaders.metal`, `metal_compositor.mm`).

Implementation: **SheepShaver (H700 / GLES)** `SheepShaver/src/gfxaccel/` (`rave_gles_renderer.cpp`, `gl_gles_renderer.cpp`, `rave_shaders_gles.h`, `gl_shaders_gles.h`, `gles_compositor.cpp`).

Use this when testing on device: tick **Verified** only after a real Mac OS 9 title exercises the path (logging or visual confirmation).

---

## 1. RAVE — native renderer entry points

These are the functions reached from `rave_dispatch.cpp` / engine after TVECT dispatch. Symbol-for-symbol parity with PocketShaver Metal is **present** in `rave_gles_renderer.cpp`.

| Area | Symbol | GLES (`rave_gles_renderer.cpp`) | Notes |
|------|--------|---------------------------------|--------|
| Overlay | `RaveCreateMetalOverlay` | Implemented | GLES: integrates with `video.h` / compositor; names kept for shared call sites |
| Overlay | `RaveDestroyMetalOverlay` | Implemented | |
| Overlay | `RaveClearOverlayToTransparent` | Implemented | |
| Overlay | `RaveScheduleDeferredOverlayDestroy` | Implemented | Matches deferred teardown pattern |
| Overlay | `RaveCancelDeferredOverlayDestroy` | Implemented | |
| Overlay | `RaveOverlayRetain` / `RaveOverlayRelease` | Implemented | Refcount path |
| Context GPU | `RaveInitMetalResources` | Implemented | FBO / depth / shader programs |
| Context GPU | `RaveReleaseMetalResources` | Implemented | |
| Frame | `NativeRenderStart` | Implemented | Must bind FBO, viewport, pipeline |
| Frame | `NativeRenderEnd` | Implemented | Readback + `gles_compositor_set_overlay(COMPOSITOR_OVERLAY_RAVE, …)` |
| Frame | `NativeRenderAbort` | Implemented | |
| Frame | `NativeFlush` / `NativeSync` | Implemented | |
| Draw | `NativeDrawTriGouraud` | Implemented | |
| Draw | `NativeDrawTriTexture` | Implemented | |
| Draw | `NativeDrawVGouraud` | Implemented | Fan/strip/etc. expansion |
| Draw | `NativeDrawVTexture` | Implemented | |
| Draw | `NativeSubmitVerticesGouraud` | Implemented | |
| Draw | `NativeSubmitVerticesTexture` | Implemented | |
| Draw | `NativeSubmitMultiTextureParams` | Implemented | Second texture unit path |
| Draw | `NativeDrawBitmap` | Implemented | |
| Draw | `NativeDrawTriMeshGouraud` | Implemented | |
| Draw | `NativeDrawTriMeshTexture` | Implemented | |
| Draw | `NativeDrawPoint` / `NativeDrawLine` | Implemented | |
| Callbacks | `NativeSetNoticeMethod` / `NativeGetNoticeMethod` | Implemented | Usually guest bookkeeping |
| Buffers | `NativeAccessDrawBuffer` / `NativeAccessDrawBufferEnd` | Implemented | CPU read/write staging |
| Buffers | `NativeAccessZBuffer` / `NativeAccessZBufferEnd` | Implemented | |
| Buffers | `NativeClearDrawBuffer` / `NativeClearZBuffer` | Implemented | |
| ATI | `NativeATIClearDrawBuffer` / `NativeATIClearZBuffer` | Implemented | Declared in `rave_engine.h` |
| Present | `NativeSwapBuffers` | Implemented | |
| Query | `NativeBusy` | Implemented | |
| Export | `NativeTextureNewFromDrawContext` / `NativeBitmapNewFromDrawContext` | Implemented | |
| Texture GPU | `RaveCreateMetalTexture` / `RaveUploadMipLevel` / `RaveGenerateMipmaps` / `RaveReleaseTexture` | Implemented | `void *` holds `GLuint` |
| Texture refresh | `RaveRefreshTextureFromPixmap` | Implemented | Declared in `rave_engine.h` |

**Dispatch coverage:** `rave_dispatch.cpp` draw method table (tags 0–34 + extensions) routes to the same handlers as PocketShaver; verify with `ACCEL_LOGGING` and a RAVE/QD3D title.

---

## 2. RAVE — shader & fixed-function parity (Metal 16 variants → GLES)

| Item | PocketShaver | SheepShaver GLES | Verified |
|------|----------------|------------------|----------|
| Program variants | 16 (`HAS_TEXTURE`, `HAS_FOG`, `HAS_ALPHA_TEST`, `HAS_MULTI_TEXTURE`) | Same bitmask → `BuildShaderVariant` | ☐ |
| Vertex layout | Position, color, texcoord (+ texcoord2 multi) | `rave_shaders_gles.h` + attrib locations 0–3 | ☐ |
| Perspective-correct UV | `u/v/w` in texcoord | Shader divides by `z` | ☐ |
| Texture env / op bits | `bitSet` on `u_texture_op` | Ported in fragment | ☐ |
| Multi-texture combine | `u_multi_texture_op`, factor | Ported | ☐ |
| Fog modes | Multiple `u_fog_mode` values | Ported | ☐ |
| Alpha test | `u_alpha_test_func` / ref | Ported | ☐ |
| Depth test / write | From draw context | GLES `glDepthFunc` / mask | ☐ |
| Blending | From context | GLES blend func | ☐ |
| GLSL version | Metal MSL | `#version 300 es` + `in`/`out`/`texture()` | ☐ |

---

## 3. RAVE — integration & compositor

| Item | Verified |
|------|----------|
| `NativeRenderEnd` → `video_blit_rave_fbo` for VOSF / QuickDraw z-order | ☐ |
| `gles_compositor_set_overlay(COMPOSITOR_OVERLAY_RAVE, …)` with correct dst rect / FBO size | ☐ |
| `present_sdl_video()` calls `gles_compositor_composite_overlays` when overlays active | ☐ |
| `gles_gl_lock` / `gles_gl_unlock` around emulator vs present thread (no concurrent GL) | ☐ |

---

## 4. OpenGL 1.2 (`gl_gles_renderer.cpp`) — entry point parity vs PocketShaver Metal

All symbols declared in `include/gl_metal_renderer.h` that are implemented in `gl_metal_renderer.mm` have matching implementations in `gl_gles_renderer.cpp` (frame, immediate mode, arrays, texture upload, read pixels, accum).

| Category | Parity | Notes |
|----------|--------|--------|
| `GLMetalInit` / `BeginFrame` / `FlushImmediateMode` / `EndFrame` / `Release` | Yes | GLES FBO + VAO/VBO |
| `GLMetalDrawPixels` / `GLMetalBitmap` | Yes | Textured NDC quad |
| `GLMetalUploadTexture` / `SubTexture` / `DestroyTexture` | Yes | BGRA → RGBA upload |
| `GLMetalUpload3DTexture` / `SubTexture3D` | **Conditional** | Full path only if `GL_ES_VERSION_3_0`; else no-op + log |
| `NativeGLReadPixels` | **Narrow** | Always reads RGBA8 from FBO; guest `format`/`type` only partially honored (same limitation pattern as Metal’s minimal fallback) |
| `NativeGLAccum` | Yes (CPU float buffer + readback) | Heavy; rarely used by games |
| Immediate mode vertex / color / normal / texcoord | Yes | All scalar + `v` + `dv` variants aligned with Metal |
| `NativeGLDrawArrays` / `DrawElements` / `DrawRangeElements` | Yes | Range hint ignored (same as Metal) |
| `NativeGLArrayElement` / `InterleavedArrays` | Yes | Same format set as Metal |
| Uber-shader | Yes | Lighting, fog, texenv (incl. `GL_ADD`), alpha test |
| Single texture unit in shader | Yes | Matches Metal `texture(0)` — multitexture not in uber-shader |

---

## 5. OpenGL — known gaps vs ideal GL 1.2 (Metal reference included)

| Topic | PocketShaver Metal | SheepShaver GLES | Risk |
|-------|--------------------|------------------|------|
| `glShadeModel(GL_FLAT)` | Uniform `shade_model` is set in C++ but **not used** in `gl_shaders.metal` fragment | No `u_shade_model`; smooth interpolation only | Low unless a game relies on flat shading |
| `NativeGLReadPixels` formats | Minimal RGBA8 path | Same class of limitation | Medium for tools; low for games |
| 3D textures | Always (Metal) | Needs GLES 3 | Medium if a game uses 3D tex |
| Stencil | As implemented in Metal | As implemented in GLES | Verify per-title |

---

## 6. Suggested test matrix (Mac OS 9)

| Bucket | Example titles / apps | Exercises |
|--------|------------------------|-----------|
| RAVE / QD3D | RAVE-focused demos, games using ATI RAVE | All draw paths, texture bind, fog, alpha |
| GL immediate | Older GL games (immediate mode) | `glBegin`, quads, fan |
| GL arrays | Nanosaur-era titles | `DrawArrays`, `InterleavedArrays` |
| Compositing | Any 3D + desktop UI | Overlay + QuickDraw menus |

---

## 7. Files to diff when debugging a mismatch

1. Draw path: `PocketShaver/.../rave_metal_renderer.mm` ↔ `rave_gles_renderer.cpp` (same function name).
2. GL draw: `gl_metal_renderer.mm` ↔ `gl_gles_renderer.cpp` (`GLMetalFlushImmediateMode`, `ExpandPrimitives`, `UploadUniforms`).
3. Shaders: `rave_shaders.metal` ↔ `rave_shaders_gles.h`; `gl_shaders.metal` ↔ `gl_shaders_gles.h`.

---

*Last audited against repository layout: SheepShaver `gfxaccel` + PocketShaver reference tree (function-level grep parity).*
