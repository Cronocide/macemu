/*
 *  gles_compositor.h - Unified GLES compositor for 3D overlay compositing
 *
 *  Replaces metal_compositor.h for the AArch64/H700 GLES port.
 *  Provides a single compositing path for both RAVE and OpenGL 1.2 FBO
 *  overlays, drawn on top of the 2D Mac framebuffer during present.
 *
 *  Design (ported from PocketShaver's metal_compositor.mm):
 *    - Renderers (RAVE/GL) render into their own FBOs as before
 *    - At present time, the compositor draws the 2D framebuffer (via SDL),
 *      then blits each active overlay FBO on top using a fullscreen quad
 *    - Overlay position/size maps to the Mac framebuffer coordinate space
 *    - Overlays are opaque within their viewport rect (no alpha blending,
 *      matching PocketShaver's CAMetalLayer approach)
 *
 *  The readback-to-the_buffer path is preserved alongside GPU compositing
 *  for QuickDraw z-order correctness (menus, dialogs drawn over 3D content).
 */

#ifndef GLES_COMPOSITOR_H
#define GLES_COMPOSITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Overlay slot identifiers.
 *  RAVE and GL each get one slot. Both can be active simultaneously
 *  (e.g. a RAVE game with an OpenGL HUD), composited in slot order.
 */
enum {
	COMPOSITOR_OVERLAY_RAVE = 0,
	COMPOSITOR_OVERLAY_GL   = 1,
	COMPOSITOR_OVERLAY_COUNT = 2
};

/*
 *  Initialize the compositor's blit shader and VBO.
 *  Called lazily on first overlay registration. Safe to call multiple times.
 *  Requires a current GL context (SDL_GL_CreateContext must have been called).
 *  Returns 0 on success, -1 on failure.
 */
int gles_compositor_init(void);

/*
 *  Register an overlay FBO's color texture for compositing.
 *
 *  slot:       COMPOSITOR_OVERLAY_RAVE or COMPOSITOR_OVERLAY_GL
 *  gl_texture: The GL texture ID of the FBO's color attachment
 *  fbo_w/h:    Size of the FBO texture in pixels
 *  dst_x/y:    Position in Mac framebuffer coordinates (top-left of overlay)
 *  vp_w/h:     Viewport size within the FBO (may be smaller than fbo_w/h)
 *  screen_w/h: Mac framebuffer dimensions (for NDC mapping)
 */
void gles_compositor_set_overlay(int slot, uint32_t gl_texture,
                                 int fbo_w, int fbo_h,
                                 int dst_x, int dst_y,
                                 int vp_w, int vp_h,
                                 int screen_w, int screen_h);

/*
 *  Deactivate an overlay slot. The texture is NOT deleted -- the renderer
 *  owns the FBO and its attachments.
 */
void gles_compositor_clear_overlay(int slot);

/*
 *  Query whether any overlay is active.
 */
int gles_compositor_has_overlay(void);

/*
 *  Draw all active overlays on top of the current framebuffer (FBO 0).
 *  Called from present_sdl_video() after SDL_RenderCopy.
 *
 *  out_w/out_h: Actual window dimensions (for glViewport).
 *
 *  Saves and restores all GL state that SDL_Renderer depends on.
 */
void gles_compositor_composite_overlays(int out_w, int out_h);

/*
 *  Tear down compositor resources (shader, VBO).
 *  Called during video shutdown.
 */
void gles_compositor_shutdown(void);

/* ---- 2D framebuffer GPU present (optional, "video_gpu_present") ---- */
/*
 *  An alternative present path that uploads the raw guest framebuffer to a GL
 *  texture and converts it in a fragment shader (palette lookup for 8-bit,
 *  RGB565 passthrough for 16-bit), avoiding the CPU pixel-conversion cost of the
 *  SDL_BlitSurface path.  All of these must be called on the GL/renderer thread,
 *  except gles_present_set_palette which only stages CPU data.
 */

/* 1 if this depth (in bits: 8 or 16) is handled by the GPU present path. */
int  gles_present_supported(int depth_bits);

/* (Re)create GL resources for the given mode.  integer_scale matches the
 * "scale_integer" pref.  Returns 0 on success, -1 on failure. */
int  gles_present_init(int depth_bits, int width, int height, int integer_scale);

/* Stage a 256-entry palette (each entry 0xAARRGGBB) for 8-bit modes.  CPU-only;
 * the texture upload happens lazily on the next gles_present_draw. */
void gles_present_set_palette(const uint32_t *argb256);

/* Upload a dirty sub-rectangle of the framebuffer.  pitch_bytes is the source
 * row stride; x/y/w/h are in pixels. */
void gles_present_upload(const void *pixels, int pitch_bytes,
                         int x, int y, int w, int h);

/* Draw the framebuffer to FBO 0, letterboxed into out_w x out_h.  When clear is
 * non-zero the borders are cleared to black first. */
void gles_present_draw(int out_w, int out_h, int clear);

/* Release GPU present resources. */
void gles_present_shutdown(void);

/*
 *  GL context mutex — serializes access between the RAVE renderer thread
 *  (NativeRenderStart → NativeRenderEnd) and the display/compositor thread
 *  (SDL_RenderCopy → compositor → SDL_RenderPresent).
 *
 *  Both threads share the same EGL/GLES context on this platform, so
 *  concurrent GL calls corrupt each other's state.
 */
void gles_gl_lock(void);
void gles_gl_unlock(void);
int  gles_gl_trylock(void);   /* 0 = acquired, non-zero = already held */


/* ---- Legacy API shims (keep metal_compositor.h callers compiling) ---- */

void compositor_set_3d_overlay_texture(void *texture, int width, int height);
void compositor_clear_3d_overlay(void);
int  compositor_has_3d_overlay(void);
void compositor_set_overlay_uv_scale(float u_scale, float v_scale);

int  MetalCompositorCreateOverlayTexture(int w, int h);
void MetalCompositorSetOverlayActive(int active);
void MetalCompositorSetOverlayRect(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* GLES_COMPOSITOR_H */
