/*
 *  gles_compositor.cpp - Unified GLES compositor for 3D overlay compositing
 *
 *  Ported from PocketShaver metal_compositor.mm
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *  GLES 2.0 translation for AArch64/H700 with Mali-G31 (via gl4es)
 *
 *  Replaces PocketShaver's CAMetalLayer overlay compositing with a single
 *  GLES blit pass that composites RAVE and/or GL FBO textures on top of
 *  the SDL2-rendered 2D Mac framebuffer.
 *
 *  Architecture:
 *    1. Renderers (rave_gles_renderer, gl_gles_renderer) draw to their FBOs
 *    2. At frame-end they register their FBO color texture + viewport rect
 *       via gles_compositor_set_overlay()
 *    3. present_sdl_video() in video_sdl2.cpp calls:
 *         SDL_RenderCopy(...)     -- 2D Mac desktop
 *         gles_compositor_composite_overlays(winW, winH)  -- 3D overlay(s)
 *         SDL_RenderPresent(...)  -- flip
 *
 *  The blit shader is a simple textured fullscreen quad (position + UV),
 *  mapped to the overlay's viewport rect within the Mac framebuffer.
 *  Overlays are drawn opaque (no alpha blending) since RAVE/GL apps
 *  typically output alpha=0 and the overlay viewport already covers
 *  exactly the 3D region. This matches PocketShaver's Metal approach.
 */

#include <cstdio>
#include <cstring>
#include <pthread.h>

#include "sysdeps.h"
#include "gles_compositor.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

// GL context mutex — serializes RAVE renderer and display/compositor threads
static pthread_mutex_t s_glMutex = PTHREAD_MUTEX_INITIALIZER;

void gles_gl_lock(void)   { pthread_mutex_lock(&s_glMutex); }
void gles_gl_unlock(void) { pthread_mutex_unlock(&s_glMutex); }
// Returns 0 if the lock was acquired, non-zero (EBUSY) if it is already held.
// Used by the present path to avoid blocking/self-deadlocking when a RAVE
// render is in progress (RAVE holds this lock across NativeRenderStart ->
// NativeRenderEnd, which spans guest execution and VBL interrupts).
int  gles_gl_trylock(void) { return pthread_mutex_trylock(&s_glMutex); }

#define COMPOSITOR_LOG(fmt, ...) \
    do { printf("[GLESCompositor] " fmt "\n", ##__VA_ARGS__); } while (0)

#define COMPOSITOR_ERR(fmt, ...) \
    do { printf("[GLESCompositor ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Overlay state per slot
// ---------------------------------------------------------------------------

struct OverlayState {
    bool     active;
    GLuint   texture;
    int      fboW, fboH;
    int      dstX, dstY;
    int      vpW, vpH;
    int      screenW, screenH;
};

static OverlayState s_overlays[COMPOSITOR_OVERLAY_COUNT] = {};

// ---------------------------------------------------------------------------
// Blit shader and geometry
// ---------------------------------------------------------------------------

static bool    s_initialized = false;
static GLuint  s_blitProgram = 0;
static GLuint  s_blitVBO     = 0;
static GLint   s_locTex      = -1;

static const char *s_blitVS =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char *s_blitFS =
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  fragColor = texture(u_tex, v_uv);\n"
    "}\n";

// ---------------------------------------------------------------------------
// GL state save/restore (minimal set that SDL_Renderer touches)
// ---------------------------------------------------------------------------

struct SavedState {
    GLint    program;
    GLint    texture;
    GLint    activeTexUnit;
    GLint    arrayBuffer;
    GLint    framebuffer;
    GLint    vao;
    GLint    viewport[4];
    GLint    blendSrc, blendDst;
    GLboolean blend;
    GLboolean depthTest;
    GLboolean scissorTest;
    GLboolean cullFace;
    GLboolean colorMask[4];
};

static void SaveState(SavedState *st)
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &st->program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &st->activeTexUnit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->arrayBuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &st->framebuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &st->vao);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &st->blendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &st->blendDst);
    st->blend     = glIsEnabled(GL_BLEND);
    st->depthTest = glIsEnabled(GL_DEPTH_TEST);
    st->scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    st->cullFace  = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_COLOR_WRITEMASK, st->colorMask);
}

static void RestoreState(const SavedState *st)
{
    glUseProgram(st->program);
    glActiveTexture(st->activeTexUnit);
    glBindTexture(GL_TEXTURE_2D, st->texture);
    glBindBuffer(GL_ARRAY_BUFFER, st->arrayBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, st->framebuffer);
    glBindVertexArray(st->vao);
    glViewport(st->viewport[0], st->viewport[1],
               st->viewport[2], st->viewport[3]);
    glBlendFunc(st->blendSrc, st->blendDst);
    if (st->blend)     glEnable(GL_BLEND);     else glDisable(GL_BLEND);
    if (st->depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st->cullFace)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    glColorMask(st->colorMask[0], st->colorMask[1],
                st->colorMask[2], st->colorMask[3]);
}

// ---------------------------------------------------------------------------
// Aspect-preserving letterbox (defined below, near the present-state statics)
// ---------------------------------------------------------------------------
//
// Computes the destination pixel rect (ox, oy, dw, dh) where a content region
// of size content_w x content_h is drawn, centered inside an out_w x out_h
// window while preserving aspect ratio.  This matches SDL_RenderSetLogicalSize
// behaviour so the 2D framebuffer present and the 3D overlay compositor land on
// exactly the same pixels (otherwise the overlay stretches full-width over the
// letterbox bars, leaving ghosted 3D content in the side margins).
static void ComputeLetterbox(int out_w, int out_h, int content_w, int content_h,
    double *ox, double *oy, double *dw, double *dh);
// ---------------------------------------------------------------------------
// Shader compilation helper
// ---------------------------------------------------------------------------

static GLuint CompileShader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    const char *version = "#version 300 es\n";
    const char *parts[2] = { version, src };
    glShaderSource(shader, 2, parts, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        COMPOSITOR_ERR("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ---------------------------------------------------------------------------
// gles_compositor_init
// ---------------------------------------------------------------------------

int gles_compositor_init(void)
{
    if (s_initialized) return 0;

    GLuint vs = CompileShader(GL_VERTEX_SHADER, s_blitVS);
    if (!vs) return -1;

    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, s_blitFS);
    if (!fs) {
        glDeleteShader(vs);
        return -1;
    }

    s_blitProgram = glCreateProgram();
    glAttachShader(s_blitProgram, vs);
    glAttachShader(s_blitProgram, fs);
    glBindAttribLocation(s_blitProgram, 0, "a_pos");
    glBindAttribLocation(s_blitProgram, 1, "a_uv");
    glLinkProgram(s_blitProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(s_blitProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(s_blitProgram, sizeof(log), NULL, log);
        COMPOSITOR_ERR("Program link failed: %s", log);
        glDeleteProgram(s_blitProgram);
        s_blitProgram = 0;
        return -1;
    }

    s_locTex = glGetUniformLocation(s_blitProgram, "u_tex");

    // VBO is updated per-overlay with computed NDC quad vertices,
    // so allocate with GL_DYNAMIC_DRAW.
    glGenBuffers(1, &s_blitVBO);

    s_initialized = true;
    COMPOSITOR_LOG("gles_compositor_init: success (program=%u vbo=%u)",
                   s_blitProgram, s_blitVBO);
    return 0;
}

// ---------------------------------------------------------------------------
// gles_compositor_set_overlay / clear / query
// ---------------------------------------------------------------------------

void gles_compositor_set_overlay(int slot, uint32_t gl_texture,
                                 int fbo_w, int fbo_h,
                                 int dst_x, int dst_y,
                                 int vp_w, int vp_h,
                                 int screen_w, int screen_h)
{
    if (slot < 0 || slot >= COMPOSITOR_OVERLAY_COUNT) return;

    OverlayState *ov = &s_overlays[slot];
    ov->active  = true;
    ov->texture = (GLuint)gl_texture;
    ov->fboW    = fbo_w;
    ov->fboH    = fbo_h;
    ov->dstX    = dst_x;
    ov->dstY    = dst_y;
    ov->vpW     = (vp_w > 0) ? vp_w : fbo_w;
    ov->vpH     = (vp_h > 0) ? vp_h : fbo_h;
    ov->screenW = (screen_w > 0) ? screen_w : 640;
    ov->screenH = (screen_h > 0) ? screen_h : 480;
}

void gles_compositor_clear_overlay(int slot)
{
    if (slot < 0 || slot >= COMPOSITOR_OVERLAY_COUNT) return;
    s_overlays[slot].active = false;
    s_overlays[slot].texture = 0;
}

int gles_compositor_has_overlay(void)
{
    for (int i = 0; i < COMPOSITOR_OVERLAY_COUNT; i++) {
        if (s_overlays[i].active && s_overlays[i].texture)
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// gles_compositor_composite_overlays
// ---------------------------------------------------------------------------
//
// For each active overlay, compute an NDC quad that maps the overlay's
// Mac-framebuffer rect (dstX, dstY, vpW, vpH) to the output window.
// The UV coordinates map [0..uvU] x [0..uvV] where uvU/uvV account
// for the viewport being smaller than the FBO texture.
//
// Coordinate system:
//   Mac framebuffer: origin top-left, Y-down
//   NDC:             origin center, Y-up, range [-1,+1]
//   FBO texture:     origin bottom-left (GL convention), rendered upside-down
//                    relative to Mac coords, so we flip V: top of quad gets
//                    v=1 (top of FBO = bottom of Mac), bottom gets v=0.

void gles_compositor_composite_overlays(int out_w, int out_h)
{
    // #region agent log
    {
        static int _comp_log = 0;
        if (_comp_log < 30) {
            int hasOv = gles_compositor_has_overlay();
            FILE *_f = fopen("/tmp/rave_debug_f04964.ndjson", "a");
            if (_f) {
                fprintf(_f, "{\"sessionId\":\"f04964\",\"hypothesisId\":\"Z18a,Z18e\","
                    "\"location\":\"gles_compositor.cpp:composite_overlays\","
                    "\"message\":\"compositor entry\","
                    "\"data\":{\"hasOverlay\":%d,\"initialized\":%d,\"prog\":%u,\"outW\":%d,\"outH\":%d,"
                    "\"slot0_active\":%d,\"slot0_tex\":%u,\"slot1_active\":%d,\"slot1_tex\":%u}}\n",
                    hasOv, (int)s_initialized, s_blitProgram, out_w, out_h,
                    (int)s_overlays[0].active, s_overlays[0].texture,
                    (int)s_overlays[1].active, s_overlays[1].texture);
                fclose(_f);
            }
            _comp_log++;
        }
    }
    // #endregion

    if (!gles_compositor_has_overlay()) return;
    if (!s_initialized && gles_compositor_init() != 0) return;
    if (!s_blitProgram) return;

    SavedState saved;
    SaveState(&saved);

    // Always target FBO 0 (the screen backbuffer).  The RAVE renderer
    // runs in the PPC emulation thread and may have changed the current
    // FBO binding to its render FBO between SDL_RenderCopy and this call.
    // Using saved.framebuffer would race with that thread and draw into
    // the wrong FBO roughly half the time (confirmed by Z18e logs).
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, out_w, out_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);

    glUseProgram(s_blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_locTex, 0);

    for (int i = 0; i < COMPOSITOR_OVERLAY_COUNT; i++) {
        OverlayState *ov = &s_overlays[i];
        if (!ov->active || !ov->texture) continue;

        float macW = (float)ov->screenW;
        float macH = (float)ov->screenH;

        // Scale factor from Mac framebuffer coords to output window pixels
        float scaleX = (float)out_w / macW;
        float scaleY = (float)out_h / macH;

        // Pixel rect in output window
        float px0 = ov->dstX * scaleX;
        float py0 = ov->dstY * scaleY;
        float px1 = px0 + ov->vpW * scaleX;
        float py1 = py0 + ov->vpH * scaleY;

        // Convert to NDC (Y-up: top of window = +1, bottom = -1)
        float ndcL = (px0 / out_w) * 2.0f - 1.0f;
        float ndcR = (px1 / out_w) * 2.0f - 1.0f;
        float ndcT = 1.0f - (py0 / out_h) * 2.0f;
        float ndcB = 1.0f - (py1 / out_h) * 2.0f;

        // UV scale: viewport may be smaller than FBO texture
        float uvU = (float)ov->vpW / (float)ov->fboW;
        float uvV = (float)ov->vpH / (float)ov->fboH;

        // Quad vertices: pos(x,y) + uv(u,v)
        // FBO origin is bottom-left, Mac origin is top-left, so the top
        // edge of the Mac viewport (ndcT) samples from v=uvV (top of FBO
        // content) and the bottom edge (ndcB) from v=0.
        float verts[] = {
            ndcL, ndcT,  0.0f, uvV,   // top-left
            ndcR, ndcT,  uvU,  uvV,    // top-right
            ndcL, ndcB,  0.0f, 0.0f,   // bottom-left
            ndcR, ndcB,  uvU,  0.0f,   // bottom-right
        };

        glBindTexture(GL_TEXTURE_2D, ov->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindBuffer(GL_ARRAY_BUFFER, s_blitVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // #region agent log
        {
            static int _draw_log = 0;
            if (_draw_log < 20) {
                GLenum err = glGetError();
                FILE *_f = fopen("/tmp/rave_debug_f04964.ndjson", "a");
                if (_f) {
                    fprintf(_f, "{\"sessionId\":\"f04964\",\"hypothesisId\":\"Z18a,Z18d\","
                        "\"location\":\"gles_compositor.cpp:after_draw\","
                        "\"message\":\"compositor draw done\","
                        "\"data\":{\"slot\":%d,\"tex\":%u,\"glError\":%u,"
                        "\"ndcL\":%.3f,\"ndcR\":%.3f,\"ndcT\":%.3f,\"ndcB\":%.3f,"
                        "\"uvU\":%.3f,\"uvV\":%.3f,\"fboBinding\":%d}}\n",
                        i, ov->texture, (unsigned)err,
                        ndcL, ndcR, ndcT, ndcB, uvU, uvV, saved.framebuffer);
                    fclose(_f);
                }
                _draw_log++;
            }
        }
        // #endregion

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
    }

    RestoreState(&saved);
}

// ---------------------------------------------------------------------------
// 2D framebuffer GPU present
// ---------------------------------------------------------------------------
//
// Uploads the raw guest framebuffer into a GL texture and converts it in a
// fragment shader, replacing the CPU SDL_BlitSurface + SDL_RenderCopy path for
// 8-bit (palette) and 16-bit (RGB565) modes.  Shares the GL context (and the
// SaveState/RestoreState dance) with the overlay compositor.

static GLuint s_presentProg8  = 0;	// 8-bit indexed -> palette lookup
static GLuint s_presentProg16 = 0;	// 16-bit RGB565 passthrough
static GLint  s_presentLocFb8  = -1, s_presentLocPal8 = -1;
static GLint  s_presentLocFb16 = -1;
static GLuint s_fbTex  = 0;			// framebuffer texture
static GLuint s_palTex = 0;			// 256x1 palette texture (8-bit only)
static int    s_presentDepth = 0;	// bits
static int    s_presentW = 0, s_presentH = 0;
static int    s_presentIntegerScale = 0;
static uint32_t s_palCPU[256] = {};
static bool   s_palDirty = false;

static const char *s_present8FS =
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_fb;\n"
    "uniform sampler2D u_pal;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  float idx = texture(u_fb, v_uv).r;\n"
    "  float u = (idx * 255.0 + 0.5) / 256.0;\n"
    "  fragColor = vec4(texture(u_pal, vec2(u, 0.5)).rgb, 1.0);\n"
    "}\n";

// Mac 16-bit is byte-swapped (big-endian) RGB555.  We upload the raw bytes as a
// two-channel GL_RG8 texture (.r = byte at addr0 = high byte, .g = byte at
// addr1 = low byte) and decode the 555 components here:
//   byte_hi = 0 RRRRR GG   (bit7=0, bits6-2=R, bits1-0=G high 2)
//   byte_lo = GGG BBBBB     (bits7-5=G low 3, bits4-0=B)
static const char *s_present16FS =
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_fb;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec2 px = texture(u_fb, v_uv).rg;\n"
    "  float hi = floor(px.r * 255.0 + 0.5);\n"
    "  float lo = floor(px.g * 255.0 + 0.5);\n"
    "  float R = mod(floor(hi / 4.0), 32.0);\n"
    "  float G = mod(hi, 4.0) * 8.0 + floor(lo / 32.0);\n"
    "  float B = mod(lo, 32.0);\n"
    "  fragColor = vec4(R / 31.0, G / 31.0, B / 31.0, 1.0);\n"
    "}\n";

static GLuint BuildPresentProgram(const char *fsSrc)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, s_blitVS);
    if (!vs) return 0;
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        COMPOSITOR_ERR("Present program link failed: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

int gles_present_supported(int depth_bits)
{
    // 8-bit  -> indexed, decoded via a 256-entry palette LUT.
    // 16-bit -> byte-swapped RGB555, decoded from raw bytes (see s_present16FS).
    return (depth_bits == 8 || depth_bits == 16) ? 1 : 0;
}

int gles_present_init(int depth_bits, int width, int height, int integer_scale)
{
    if (!gles_present_supported(depth_bits)) return -1;
    if (s_blitVBO == 0 && gles_compositor_init() != 0) return -1;

    s_presentDepth = depth_bits;
    s_presentW = width;
    s_presentH = height;
    s_presentIntegerScale = integer_scale;

    // (Re)create the framebuffer texture in the appropriate internal format.
    if (s_fbTex) { glDeleteTextures(1, &s_fbTex); s_fbTex = 0; }
    glGenTextures(1, &s_fbTex);
    glBindTexture(GL_TEXTURE_2D, s_fbTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (depth_bits == 8) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, NULL);
        if (!s_presentProg8) {
            s_presentProg8 = BuildPresentProgram(s_present8FS);
            if (!s_presentProg8) return -1;
            s_presentLocFb8  = glGetUniformLocation(s_presentProg8, "u_fb");
            s_presentLocPal8 = glGetUniformLocation(s_presentProg8, "u_pal");
        }
        if (!s_palTex) {
            glGenTextures(1, &s_palTex);
            glBindTexture(GL_TEXTURE_2D, s_palTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
        s_palDirty = true;
    } else { // 16-bit (byte-swapped RGB555, decoded in shader from raw bytes)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width, height, 0,
                     GL_RG, GL_UNSIGNED_BYTE, NULL);
        if (!s_presentProg16) {
            s_presentProg16 = BuildPresentProgram(s_present16FS);
            if (!s_presentProg16) return -1;
            s_presentLocFb16 = glGetUniformLocation(s_presentProg16, "u_fb");
        }
    }
    COMPOSITOR_LOG("gles_present_init: depth=%d %dx%d intscale=%d fbTex=%u",
                   depth_bits, width, height, integer_scale, s_fbTex);
    return 0;
}

void gles_present_set_palette(const uint32_t *argb256)
{
    if (!argb256) return;
    memcpy(s_palCPU, argb256, sizeof(s_palCPU));
    s_palDirty = true;
}

void gles_present_upload(const void *pixels, int pitch_bytes,
                         int x, int y, int w, int h)
{
    if (!s_fbTex || !pixels || w <= 0 || h <= 0) return;
    const int bpp = (s_presentDepth == 8) ? 1 : 2;
    const uint8_t *base = (const uint8_t *)pixels + (size_t)y * pitch_bytes + (size_t)x * bpp;

    glBindTexture(GL_TEXTURE_2D, s_fbTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch_bytes / bpp);
    if (s_presentDepth == 8)
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, base);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RG, GL_UNSIGNED_BYTE, base);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void gles_present_draw(int out_w, int out_h, int clear)
{
    if (!s_fbTex || s_presentW <= 0 || s_presentH <= 0) return;
    const GLuint prog = (s_presentDepth == 8) ? s_presentProg8 : s_presentProg16;
    if (!prog) return;

    SavedState saved;
    SaveState(&saved);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, out_w, out_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);

    if (clear) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Compute the letterboxed destination rect (aspect-preserving), matching
    // SDL_RenderSetLogicalSize behaviour so the GPU path looks identical.
    double scale = (double)out_w / s_presentW;
    const double sy = (double)out_h / s_presentH;
    if (sy < scale) scale = sy;
    if (s_presentIntegerScale) scale = (scale < 1.0) ? 1.0 : (double)(int)scale;
    const double dw = scale * s_presentW;
    const double dh = scale * s_presentH;
    const double ox = (out_w - dw) * 0.5;
    const double oy = (out_h - dh) * 0.5;
    const float ndcL = (float)((ox / out_w) * 2.0 - 1.0);
    const float ndcR = (float)(((ox + dw) / out_w) * 2.0 - 1.0);
    const float ndcT = (float)(1.0 - (oy / out_h) * 2.0);
    const float ndcB = (float)(1.0 - ((oy + dh) / out_h) * 2.0);

    // Mac framebuffer row 0 (top) is uploaded to texture row 0, so the top of
    // the quad samples v=0 (no vertical flip, unlike the FBO overlay path).
    const float verts[] = {
        ndcL, ndcT,  0.0f, 0.0f,   // top-left
        ndcR, ndcT,  1.0f, 0.0f,   // top-right
        ndcL, ndcB,  0.0f, 1.0f,   // bottom-left
        ndcR, ndcB,  1.0f, 1.0f,   // bottom-right
    };

    glUseProgram(prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fbTex);
    // Both paths sample encoded data (8-bit palette indices, 16-bit packed
    // RGB555 bytes), which must not be interpolated -> nearest filtering.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (s_presentDepth == 8) {
        glUniform1i(s_presentLocFb8, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        if (s_palDirty) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, s_palCPU);
            s_palDirty = false;
        }
        glUniform1i(s_presentLocPal8, 1);
    } else {
        glUniform1i(s_presentLocFb16, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_blitVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    // Leave texture unit 1 clean before restoring unit 0's binding.
    if (s_presentDepth == 8) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    RestoreState(&saved);
}

void gles_present_shutdown(void)
{
    if (s_fbTex)  { glDeleteTextures(1, &s_fbTex);  s_fbTex = 0; }
    if (s_palTex) { glDeleteTextures(1, &s_palTex); s_palTex = 0; }
    if (s_presentProg8)  { glDeleteProgram(s_presentProg8);  s_presentProg8 = 0; }
    if (s_presentProg16) { glDeleteProgram(s_presentProg16); s_presentProg16 = 0; }
    s_presentDepth = s_presentW = s_presentH = 0;
    s_palDirty = false;
}

// ---------------------------------------------------------------------------
// gles_compositor_shutdown
// ---------------------------------------------------------------------------

void gles_compositor_shutdown(void)
{
    if (s_blitProgram) {
        glDeleteProgram(s_blitProgram);
        s_blitProgram = 0;
    }
    if (s_blitVBO) {
        glDeleteBuffers(1, &s_blitVBO);
        s_blitVBO = 0;
    }
    for (int i = 0; i < COMPOSITOR_OVERLAY_COUNT; i++) {
        s_overlays[i].active = false;
        s_overlays[i].texture = 0;
    }
    s_initialized = false;
    s_locTex = -1;
    COMPOSITOR_LOG("gles_compositor_shutdown: done");
}

// ---------------------------------------------------------------------------
// Legacy API shims (for callers still using metal_compositor.h names)
// ---------------------------------------------------------------------------
// These map the old PocketShaver-derived stub API to the new compositor.
// They operate on the RAVE overlay slot (slot 0) since the old API was
// designed for a single overlay.

static int   s_legacy_width  = 0;
static int   s_legacy_height = 0;
static float s_legacy_uv_u   = 1.0f;
static float s_legacy_uv_v   = 1.0f;

void compositor_set_3d_overlay_texture(void *texture, int width, int height)
{
    // texture pointer is actually a GLuint cast to void*
    uint32_t tex = (uint32_t)(uintptr_t)texture;
    s_legacy_width = width;
    s_legacy_height = height;
    if (tex) {
        gles_compositor_set_overlay(COMPOSITOR_OVERLAY_RAVE, tex,
                                    width, height, 0, 0,
                                    width, height, 640, 480);
    }
}

void compositor_clear_3d_overlay(void)
{
    gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_RAVE);
}

int compositor_has_3d_overlay(void)
{
    return s_overlays[COMPOSITOR_OVERLAY_RAVE].active ? 1 : 0;
}

void compositor_set_overlay_uv_scale(float u_scale, float v_scale)
{
    s_legacy_uv_u = u_scale;
    s_legacy_uv_v = v_scale;
}

int MetalCompositorCreateOverlayTexture(int w, int h)
{
    s_legacy_width = w;
    s_legacy_height = h;
    return 0;
}

void MetalCompositorSetOverlayActive(int active)
{
    if (!active) {
        gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_RAVE);
    }
}

void MetalCompositorSetOverlayRect(int x, int y, int w, int h)
{
    OverlayState *ov = &s_overlays[COMPOSITOR_OVERLAY_RAVE];
    if (ov->active) {
        ov->dstX = x;
        ov->dstY = y;
        if (w > 0 && h > 0) {
            ov->vpW = w;
            ov->vpH = h;
        }
    }
}
