/*
 *  metal_compositor.h - Compositor interface (stub for GLES port)
 *
 *  Ported from PocketShaver (https://github.com/carbjo/PocketShaver)
 *  Original branch: sierra760/gfxaccel
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Stub: all functions are no-ops on the aarch64/GLES platform.
 *  Phase 4 will replace this with EGL/SDL2 compositor integration.
 */

#ifndef METAL_COMPOSITOR_H
#define METAL_COMPOSITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#endif /* METAL_COMPOSITOR_H */
