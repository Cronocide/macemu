/*
 *  rave_gles_renderer_stub.cpp - Stub RAVE renderer for build integration
 *
 *  Ported from PocketShaver (https://github.com/carbjo/PocketShaver)
 *  Original branch: sierra760/gfxaccel
 *
 *  This file provides no-op implementations of the RAVE renderer interface
 *  declared in rave_metal_renderer.h.  All draw/render methods return
 *  kQANotSupported (-27694) so the guest RAVE manager knows the engine
 *  cannot render yet.  Phase 2 will replace this with real GLES 2.0 code.
 */

#include "rave_metal_renderer.h"
#include <cstdio>

static const int32_t kQANotSupported = -27694;

// Overlay lifecycle
void RaveCreateMetalOverlay(int32_t, int32_t, int32_t, int32_t) {}
void RaveDestroyMetalOverlay(void) {}
void RaveClearOverlayToTransparent(void) {}
void RaveScheduleDeferredOverlayDestroy(void) {}
void RaveCancelDeferredOverlayDestroy(void) {}
void RaveOverlayRetain(void) {}
void RaveOverlayRelease(void) {}

// Per-context resources
void RaveInitMetalResources(struct RaveDrawPrivate *) {}
void RaveReleaseMetalResources(struct RaveDrawPrivate *) {}

// Render methods
int32_t NativeRenderStart(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeRenderEnd(uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeRenderAbort(uint32_t) { return kQANotSupported; }
int32_t NativeFlush(uint32_t) { return kQANotSupported; }
int32_t NativeSync(uint32_t) { return kQANotSupported; }

// Draw methods
int32_t NativeDrawTriGouraud(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawVGouraud(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeSubmitVerticesGouraud(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawPoint(uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawLine(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawTriTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawVTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeSubmitVerticesTexture(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeSubmitMultiTextureParams(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawBitmap(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawTriMeshGouraud(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeDrawTriMeshTexture(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeSetNoticeMethod(uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeGetNoticeMethod(uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }

// Buffer access
int32_t NativeAccessDrawBuffer(uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeAccessDrawBufferEnd(uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeAccessZBuffer(uint32_t, uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeAccessZBufferEnd(uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeClearDrawBuffer(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeClearZBuffer(uint32_t, uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeSwapBuffers(uint32_t) { return kQANotSupported; }
int32_t NativeBusy(uint32_t) { return kQANotSupported; }
uint32_t NativeTextureNewFromDrawContext(uint32_t) { return 0; }
uint32_t NativeBitmapNewFromDrawContext(uint32_t) { return 0; }

// ATI clear methods (called from rave_dispatch.cpp)
int32_t NativeATIClearDrawBuffer(uint32_t, uint32_t) { return kQANotSupported; }
int32_t NativeATIClearZBuffer(uint32_t, uint32_t) { return kQANotSupported; }

// Texture management
void *RaveCreateMetalTexture(uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t) { return nullptr; }
void RaveUploadMipLevel(void *, uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t) {}
void RaveGenerateMipmaps(void *) {}
void RaveReleaseTexture(void *) {}
