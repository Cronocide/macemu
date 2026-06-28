/*
 *  rave_metal_renderer.h - RAVE rendering backend C interface
 *
 *  Ported from PocketShaver (https://github.com/carbjo/PocketShaver)
 *  Original branch: sierra760/gfxaccel
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *
 *  SheepShaver AArch64: implemented in rave_gles_renderer.cpp (OpenGL ES 3).
 *  Overlay/texture helpers keep historical "Metal" names for shared sources.
 *  ATI clear + pixmap refresh: see also prototypes in rave_engine.h.
 */

#ifndef RAVE_METAL_RENDERER_H
#define RAVE_METAL_RENDERER_H

#include <stdint.h>

struct RaveDrawPrivate;

// Overlay lifecycle (GLES + video/compositor integration)
void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width, int32_t height);
void RaveDestroyMetalOverlay(void);
void RaveClearOverlayToTransparent(void);
void RaveScheduleDeferredOverlayDestroy(void);
void RaveCancelDeferredOverlayDestroy(void);
void RaveOverlayRetain(void);
void RaveOverlayRelease(void);

// Per-context GPU resources (FBO, shaders, buffers)
void RaveInitMetalResources(struct RaveDrawPrivate *priv);
void RaveReleaseMetalResources(struct RaveDrawPrivate *priv);

// Render frame boundaries
int32_t NativeRenderStart(uint32_t drawContextAddr, uint32_t dirtyRectAddr, uint32_t initialContextAddr);
int32_t NativeRenderEnd(uint32_t drawContextAddr, uint32_t modifiedRectAddr);
int32_t NativeRenderAbort(uint32_t drawContextAddr);
int32_t NativeFlush(uint32_t drawContextAddr);
int32_t NativeSync(uint32_t drawContextAddr);

// Draw methods
int32_t NativeDrawTriGouraud(uint32_t drawContextAddr, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t flags);
int32_t NativeDrawVGouraud(uint32_t drawContextAddr, uint32_t nVertices, uint32_t vertexMode, uint32_t verticesAddr, uint32_t flagsAddr);
int32_t NativeSubmitVerticesGouraud(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr);
int32_t NativeDrawPoint(uint32_t drawContextAddr, uint32_t v0Addr);
int32_t NativeDrawLine(uint32_t drawContextAddr, uint32_t v0Addr, uint32_t v1Addr);
int32_t NativeDrawTriTexture(uint32_t drawContextAddr, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t flags);
int32_t NativeDrawVTexture(uint32_t drawContextAddr, uint32_t nVertices, uint32_t vertexMode, uint32_t verticesAddr, uint32_t flagsAddr);
int32_t NativeSubmitVerticesTexture(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr);
int32_t NativeSubmitMultiTextureParams(uint32_t drawContextAddr, uint32_t nVertices, uint32_t multiTexParamsAddr);
int32_t NativeDrawBitmap(uint32_t drawContextAddr, uint32_t vertexAddr, uint32_t bitmapMacAddr);
int32_t NativeDrawTriMeshGouraud(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr);
int32_t NativeDrawTriMeshTexture(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr);
int32_t NativeSetNoticeMethod(uint32_t drawContextAddr, uint32_t method, uint32_t callback, uint32_t refCon);
int32_t NativeGetNoticeMethod(uint32_t drawContextAddr, uint32_t method, uint32_t callbackOutPtr, uint32_t refConOutPtr);

// Buffer access / clear
int32_t NativeAccessDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t rowBytesPtr, uint32_t bufferPtrPtr);
int32_t NativeAccessDrawBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr);
int32_t NativeAccessZBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t rowBytesPtr, uint32_t bufferPtrPtr);
int32_t NativeAccessZBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr);
int32_t NativeClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr);
int32_t NativeClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr);
int32_t NativeSwapBuffers(uint32_t drawContextAddr);
int32_t NativeBusy(uint32_t drawContextAddr);
uint32_t NativeTextureNewFromDrawContext(uint32_t drawContextAddr);
uint32_t NativeBitmapNewFromDrawContext(uint32_t drawContextAddr);

// Texture objects (host GLuint stored in void *)
void *RaveCreateMetalTexture(uint32_t width, uint32_t height, uint32_t mipLevels, const uint8_t *pixelData, uint32_t bytesPerRow);
void RaveUploadMipLevel(void *metalTexture, uint32_t level, uint32_t width, uint32_t height, const uint8_t *pixelData, uint32_t bytesPerRow);
void RaveGenerateMipmaps(void *metalTexture);
void RaveReleaseTexture(void *metalTexture);

#endif /* RAVE_METAL_RENDERER_H */
