/*
 *  gl_gles_renderer_stub.cpp - No-op backend matching gl_metal_renderer.mm entry points
 *
 *  Satisfies link symbols for GLMetal* and NativeGL* functions that on macOS / PocketShaver
 *  live in gl_metal_renderer.mm. Replace with a real GLES implementation when ready.
 */

#include "gl_metal_renderer.h"

void GLMetalInit(GLContext *) {}
void GLMetalBeginFrame(GLContext *) {}
void GLMetalFlushImmediateMode(GLContext *) {}
void GLMetalDrawPixels(GLContext *, int, int, const uint8_t *, int) {}
void GLMetalBitmap(GLContext *, int, int, const uint8_t *, int) {}
void GLMetalEndFrame(GLContext *) {}
void GLMetalRelease(GLContext *) {}

void GLMetalUploadTexture(GLContext *, GLTextureObject *, int, int, int, const uint8_t *, int) {}
void GLMetalUploadSubTexture(GLContext *, GLTextureObject *, int, int, int, int, int,
                             const uint8_t *, int) {}
void GLMetalDestroyTexture(GLTextureObject *) {}
void GLMetalUpload3DTexture(GLContext *, GLTextureObject *, int, int, int, int,
                            const uint8_t *, int) {}
void GLMetalUploadSubTexture3D(GLContext *, GLTextureObject *, int, int, int, int,
                               int, int, int, const uint8_t *, int, int) {}

void NativeGLFinish(GLContext *) {}
void NativeGLFlush(GLContext *) {}
void NativeGLReadPixels(GLContext *, int32_t, int32_t, int32_t, int32_t,
                        uint32_t, uint32_t, uint32_t) {}
void NativeGLAccum(GLContext *, uint32_t, float) {}

void NativeGLBegin(GLContext *, uint32_t) {}
void NativeGLEnd(GLContext *) {}

void NativeGLVertex2f(GLContext *, float, float) {}
void NativeGLVertex3f(GLContext *, float, float, float) {}
void NativeGLVertex4f(GLContext *, float, float, float, float) {}
void NativeGLVertex2d(GLContext *, double, double) {}
void NativeGLVertex3d(GLContext *, double, double, double) {}
void NativeGLVertex4d(GLContext *, double, double, double, double) {}
void NativeGLVertex2i(GLContext *, int32_t, int32_t) {}
void NativeGLVertex3i(GLContext *, int32_t, int32_t, int32_t) {}
void NativeGLVertex4i(GLContext *, int32_t, int32_t, int32_t, int32_t) {}
void NativeGLVertex2s(GLContext *, int16_t, int16_t) {}
void NativeGLVertex3s(GLContext *, int16_t, int16_t, int16_t) {}
void NativeGLVertex4s(GLContext *, int16_t, int16_t, int16_t, int16_t) {}
void NativeGLVertex2fv(GLContext *, uint32_t) {}
void NativeGLVertex3fv(GLContext *, uint32_t) {}
void NativeGLVertex4fv(GLContext *, uint32_t) {}
void NativeGLVertex2dv(GLContext *, uint32_t) {}
void NativeGLVertex3dv(GLContext *, uint32_t) {}
void NativeGLVertex4dv(GLContext *, uint32_t) {}
void NativeGLVertex2iv(GLContext *, uint32_t) {}
void NativeGLVertex3iv(GLContext *, uint32_t) {}
void NativeGLVertex4iv(GLContext *, uint32_t) {}
void NativeGLVertex2sv(GLContext *, uint32_t) {}
void NativeGLVertex3sv(GLContext *, uint32_t) {}
void NativeGLVertex4sv(GLContext *, uint32_t) {}

void NativeGLColor3f(GLContext *, float, float, float) {}
void NativeGLColor4f(GLContext *, float, float, float, float) {}
void NativeGLColor3d(GLContext *, double, double, double) {}
void NativeGLColor4d(GLContext *, double, double, double, double) {}
void NativeGLColor3b(GLContext *, int8_t, int8_t, int8_t) {}
void NativeGLColor4b(GLContext *, int8_t, int8_t, int8_t, int8_t) {}
void NativeGLColor3ub(GLContext *, uint8_t, uint8_t, uint8_t) {}
void NativeGLColor4ub(GLContext *, uint8_t, uint8_t, uint8_t, uint8_t) {}
void NativeGLColor3i(GLContext *, int32_t, int32_t, int32_t) {}
void NativeGLColor4i(GLContext *, int32_t, int32_t, int32_t, int32_t) {}
void NativeGLColor3s(GLContext *, int16_t, int16_t, int16_t) {}
void NativeGLColor4s(GLContext *, int16_t, int16_t, int16_t, int16_t) {}
void NativeGLColor3ui(GLContext *, uint32_t, uint32_t, uint32_t) {}
void NativeGLColor4ui(GLContext *, uint32_t, uint32_t, uint32_t, uint32_t) {}
void NativeGLColor3us(GLContext *, uint16_t, uint16_t, uint16_t) {}
void NativeGLColor4us(GLContext *, uint16_t, uint16_t, uint16_t, uint16_t) {}
void NativeGLColor3fv(GLContext *, uint32_t) {}
void NativeGLColor4fv(GLContext *, uint32_t) {}
void NativeGLColor3bv(GLContext *, uint32_t) {}
void NativeGLColor4bv(GLContext *, uint32_t) {}
void NativeGLColor3ubv(GLContext *, uint32_t) {}
void NativeGLColor4ubv(GLContext *, uint32_t) {}
void NativeGLColor3dv(GLContext *, uint32_t) {}
void NativeGLColor4dv(GLContext *, uint32_t) {}
void NativeGLColor3iv(GLContext *, uint32_t) {}
void NativeGLColor4iv(GLContext *, uint32_t) {}
void NativeGLColor3sv(GLContext *, uint32_t) {}
void NativeGLColor4sv(GLContext *, uint32_t) {}
void NativeGLColor3uiv(GLContext *, uint32_t) {}
void NativeGLColor4uiv(GLContext *, uint32_t) {}
void NativeGLColor3usv(GLContext *, uint32_t) {}
void NativeGLColor4usv(GLContext *, uint32_t) {}

void NativeGLNormal3f(GLContext *, float, float, float) {}
void NativeGLNormal3d(GLContext *, double, double, double) {}
void NativeGLNormal3b(GLContext *, int8_t, int8_t, int8_t) {}
void NativeGLNormal3i(GLContext *, int32_t, int32_t, int32_t) {}
void NativeGLNormal3s(GLContext *, int16_t, int16_t, int16_t) {}
void NativeGLNormal3fv(GLContext *, uint32_t) {}
void NativeGLNormal3dv(GLContext *, uint32_t) {}
void NativeGLNormal3bv(GLContext *, uint32_t) {}
void NativeGLNormal3iv(GLContext *, uint32_t) {}
void NativeGLNormal3sv(GLContext *, uint32_t) {}

void NativeGLTexCoord1f(GLContext *, float) {}
void NativeGLTexCoord2f(GLContext *, float, float) {}
void NativeGLTexCoord3f(GLContext *, float, float, float) {}
void NativeGLTexCoord4f(GLContext *, float, float, float, float) {}
void NativeGLTexCoord1d(GLContext *, double) {}
void NativeGLTexCoord2d(GLContext *, double, double) {}
void NativeGLTexCoord3d(GLContext *, double, double, double) {}
void NativeGLTexCoord4d(GLContext *, double, double, double, double) {}
void NativeGLTexCoord1i(GLContext *, int32_t) {}
void NativeGLTexCoord2i(GLContext *, int32_t, int32_t) {}
void NativeGLTexCoord3i(GLContext *, int32_t, int32_t, int32_t) {}
void NativeGLTexCoord4i(GLContext *, int32_t, int32_t, int32_t, int32_t) {}
void NativeGLTexCoord1s(GLContext *, int16_t) {}
void NativeGLTexCoord2s(GLContext *, int16_t, int16_t) {}
void NativeGLTexCoord3s(GLContext *, int16_t, int16_t, int16_t) {}
void NativeGLTexCoord4s(GLContext *, int16_t, int16_t, int16_t, int16_t) {}
void NativeGLTexCoord1fv(GLContext *, uint32_t) {}
void NativeGLTexCoord2fv(GLContext *, uint32_t) {}
void NativeGLTexCoord3fv(GLContext *, uint32_t) {}
void NativeGLTexCoord4fv(GLContext *, uint32_t) {}
void NativeGLTexCoord1dv(GLContext *, uint32_t) {}
void NativeGLTexCoord2dv(GLContext *, uint32_t) {}
void NativeGLTexCoord3dv(GLContext *, uint32_t) {}
void NativeGLTexCoord4dv(GLContext *, uint32_t) {}
void NativeGLTexCoord1iv(GLContext *, uint32_t) {}
void NativeGLTexCoord2iv(GLContext *, uint32_t) {}
void NativeGLTexCoord3iv(GLContext *, uint32_t) {}
void NativeGLTexCoord4iv(GLContext *, uint32_t) {}
void NativeGLTexCoord1sv(GLContext *, uint32_t) {}
void NativeGLTexCoord2sv(GLContext *, uint32_t) {}
void NativeGLTexCoord3sv(GLContext *, uint32_t) {}
void NativeGLTexCoord4sv(GLContext *, uint32_t) {}

void NativeGLDrawArrays(GLContext *, uint32_t, int32_t, int32_t) {}
void NativeGLDrawElements(GLContext *, uint32_t, int32_t, uint32_t, uint32_t) {}
void NativeGLDrawRangeElements(GLContext *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t, uint32_t) {}
void NativeGLArrayElement(GLContext *, int32_t) {}
void NativeGLInterleavedArrays(GLContext *, uint32_t, int32_t, uint32_t) {}
