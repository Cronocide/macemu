/*
 *  compositor_stub.cpp - Compositor stubs for GLES port
 *
 *  Phase 4 will replace this with EGL/SDL2 compositor integration.
 */

#include "metal_compositor.h"

void compositor_set_3d_overlay_texture(void *, int, int) {}
void compositor_clear_3d_overlay(void) {}
int  compositor_has_3d_overlay(void) { return 0; }
void compositor_set_overlay_uv_scale(float, float) {}

int  MetalCompositorCreateOverlayTexture(int, int) { return 0; }
void MetalCompositorSetOverlayActive(int) {}
void MetalCompositorSetOverlayRect(int, int, int, int) {}
