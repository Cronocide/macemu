/*
 *  gl_metal_renderer.h - Metal / GLES stub backend: public entry points from gl_metal_renderer
 *
 *  Declarations match PocketShaver/SheepShaver/src/gfxaccel/gl_metal_renderer.mm.
 *  Forward declarations only — include after GLContext / GLTextureObject are defined,
 *  or ensure this header is included from gl_engine.h below those structs.
 */

#ifndef GL_METAL_RENDERER_H
#define GL_METAL_RENDERER_H

#include <cstdint>

struct GLContext;
struct GLTextureObject;

// ---- Frame / lifecycle (GLMetal*) ----
void GLMetalInit(GLContext *ctx);
void GLMetalBeginFrame(GLContext *ctx);
void GLMetalFlushImmediateMode(GLContext *ctx);
void GLMetalDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
void GLMetalBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
void GLMetalEndFrame(GLContext *ctx);
void GLMetalRelease(GLContext *ctx);

void GLMetalUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                          int width, int height, const uint8_t *data, int dataLen);
void GLMetalUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                             int xoff, int yoff, int w, int h,
                             const uint8_t *data, int bytesPerRow);
void GLMetalDestroyTexture(GLTextureObject *texObj);
void GLMetalUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                            int width, int height, int depth,
                            const uint8_t *data, int dataLen);
void GLMetalUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level,
                               int xoff, int yoff, int zoff,
                               int w, int h, int d,
                               const uint8_t *data, int bytesPerRow, int bytesPerImage);

// ---- Native GL hooks implemented in the metal renderer TU ----
void NativeGLFinish(GLContext *ctx);
void NativeGLFlush(GLContext *ctx);
void NativeGLReadPixels(GLContext *ctx, int32_t x, int32_t y, int32_t width, int32_t height,
                        uint32_t format, uint32_t type, uint32_t mac_pixels);
void NativeGLAccum(GLContext *ctx, uint32_t op, float value);

void NativeGLBegin(GLContext *ctx, uint32_t mode);
void NativeGLEnd(GLContext *ctx);

void NativeGLVertex2f(GLContext *ctx, float x, float y);
void NativeGLVertex3f(GLContext *ctx, float x, float y, float z);
void NativeGLVertex4f(GLContext *ctx, float x, float y, float z, float w);
void NativeGLVertex2d(GLContext *ctx, double x, double y);
void NativeGLVertex3d(GLContext *ctx, double x, double y, double z);
void NativeGLVertex4d(GLContext *ctx, double x, double y, double z, double w);
void NativeGLVertex2i(GLContext *ctx, int32_t x, int32_t y);
void NativeGLVertex3i(GLContext *ctx, int32_t x, int32_t y, int32_t z);
void NativeGLVertex4i(GLContext *ctx, int32_t x, int32_t y, int32_t z, int32_t w);
void NativeGLVertex2s(GLContext *ctx, int16_t x, int16_t y);
void NativeGLVertex3s(GLContext *ctx, int16_t x, int16_t y, int16_t z);
void NativeGLVertex4s(GLContext *ctx, int16_t x, int16_t y, int16_t z, int16_t w);
void NativeGLVertex2fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex3fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex4fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex2dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex3dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex4dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex2iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex3iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex4iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex2sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex3sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLVertex4sv(GLContext *ctx, uint32_t mac_ptr);

void NativeGLColor3f(GLContext *ctx, float r, float g, float b);
void NativeGLColor4f(GLContext *ctx, float r, float g, float b, float a);
void NativeGLColor3d(GLContext *ctx, double r, double g, double b);
void NativeGLColor4d(GLContext *ctx, double r, double g, double b, double a);
void NativeGLColor3b(GLContext *ctx, int8_t r, int8_t g, int8_t b);
void NativeGLColor4b(GLContext *ctx, int8_t r, int8_t g, int8_t b, int8_t a);
void NativeGLColor3ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b);
void NativeGLColor4ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void NativeGLColor3i(GLContext *ctx, int32_t r, int32_t g, int32_t b);
void NativeGLColor4i(GLContext *ctx, int32_t r, int32_t g, int32_t b, int32_t a);
void NativeGLColor3s(GLContext *ctx, int16_t r, int16_t g, int16_t b);
void NativeGLColor4s(GLContext *ctx, int16_t r, int16_t g, int16_t b, int16_t a);
void NativeGLColor3ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b);
void NativeGLColor4ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
void NativeGLColor3us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b);
void NativeGLColor4us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b, uint16_t a);
void NativeGLColor3fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3bv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4bv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3ubv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4ubv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3uiv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4uiv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor3usv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLColor4usv(GLContext *ctx, uint32_t mac_ptr);

void NativeGLNormal3f(GLContext *ctx, float x, float y, float z);
void NativeGLNormal3d(GLContext *ctx, double x, double y, double z);
void NativeGLNormal3b(GLContext *ctx, int8_t x, int8_t y, int8_t z);
void NativeGLNormal3i(GLContext *ctx, int32_t x, int32_t y, int32_t z);
void NativeGLNormal3s(GLContext *ctx, int16_t x, int16_t y, int16_t z);
void NativeGLNormal3fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLNormal3dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLNormal3bv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLNormal3iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLNormal3sv(GLContext *ctx, uint32_t mac_ptr);

void NativeGLTexCoord1f(GLContext *ctx, float s);
void NativeGLTexCoord2f(GLContext *ctx, float s, float t);
void NativeGLTexCoord3f(GLContext *ctx, float s, float t, float r);
void NativeGLTexCoord4f(GLContext *ctx, float s, float t, float r, float q);
void NativeGLTexCoord1d(GLContext *ctx, double s);
void NativeGLTexCoord2d(GLContext *ctx, double s, double t);
void NativeGLTexCoord3d(GLContext *ctx, double s, double t, double r);
void NativeGLTexCoord4d(GLContext *ctx, double s, double t, double r, double q);
void NativeGLTexCoord1i(GLContext *ctx, int32_t s);
void NativeGLTexCoord2i(GLContext *ctx, int32_t s, int32_t t);
void NativeGLTexCoord3i(GLContext *ctx, int32_t s, int32_t t, int32_t r);
void NativeGLTexCoord4i(GLContext *ctx, int32_t s, int32_t t, int32_t r, int32_t q);
void NativeGLTexCoord1s(GLContext *ctx, int16_t s);
void NativeGLTexCoord2s(GLContext *ctx, int16_t s, int16_t t);
void NativeGLTexCoord3s(GLContext *ctx, int16_t s, int16_t t, int16_t r);
void NativeGLTexCoord4s(GLContext *ctx, int16_t s, int16_t t, int16_t r, int16_t q);
void NativeGLTexCoord1fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord2fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord3fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord4fv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord1dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord2dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord3dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord4dv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord1iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord2iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord3iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord4iv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord1sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord2sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord3sv(GLContext *ctx, uint32_t mac_ptr);
void NativeGLTexCoord4sv(GLContext *ctx, uint32_t mac_ptr);

void NativeGLDrawArrays(GLContext *ctx, uint32_t mode, int32_t first, int32_t count);
void NativeGLDrawElements(GLContext *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t indices_ptr);
void NativeGLDrawRangeElements(GLContext *ctx, uint32_t mode, uint32_t start, uint32_t end,
                             int32_t count, uint32_t type, uint32_t indices_ptr);
void NativeGLArrayElement(GLContext *ctx, int32_t i);
void NativeGLInterleavedArrays(GLContext *ctx, uint32_t format, int32_t stride, uint32_t pointer);

#endif /* GL_METAL_RENDERER_H */
