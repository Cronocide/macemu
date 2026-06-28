/*
 *  gl_gles_renderer.cpp - OpenGL ES 2.0/3.0 backend for OpenGL 1.2 FFP
 *
 *  Ported from PocketShaver gl_metal_renderer.mm / patterns from rave_gles_renderer.cpp
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "gl_engine.h"
#include "rave_metal_renderer.h"
#include "gles_compositor.h"
#include "accel_logging.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "gl_shaders_gles.h"
#include "video.h"

extern uint32 Mac_sysalloc(uint32 size);

#if ACCEL_LOGGING_ENABLED
#define GL_LOG(fmt, ...) do { \
	if (gl_logging_enabled) printf("GL_GLES: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#else
#define GL_LOG(fmt, ...) do {} while (0)
#endif

#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void video_request_gl_refresh(void) {}
#else
extern "C" void video_request_gl_refresh(void) {}
#endif

// Guest-side GL 1.2 constants (values that GLES headers may already provide
// are guarded; the rest are OpenGL 1.2-only and absent from GLES headers).
#ifndef GL_ZERO
#define GL_ZERO                    0x0000
#endif
#ifndef GL_ONE
#define GL_ONE                     0x0001
#endif
#ifndef GL_SRC_COLOR
#define GL_SRC_COLOR               0x0300
#endif
#ifndef GL_ONE_MINUS_SRC_COLOR
#define GL_ONE_MINUS_SRC_COLOR     0x0301
#endif
#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA               0x0302
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA     0x0303
#endif
#ifndef GL_DST_ALPHA
#define GL_DST_ALPHA               0x0304
#endif
#ifndef GL_ONE_MINUS_DST_ALPHA
#define GL_ONE_MINUS_DST_ALPHA     0x0305
#endif
#ifndef GL_DST_COLOR
#define GL_DST_COLOR               0x0306
#endif
#ifndef GL_ONE_MINUS_DST_COLOR
#define GL_ONE_MINUS_DST_COLOR     0x0307
#endif
#ifndef GL_SRC_ALPHA_SATURATE
#define GL_SRC_ALPHA_SATURATE      0x0308
#endif

// Primitive types (GLES defines 0-6, but not QUADS/QUAD_STRIP/POLYGON)
#ifndef GL_POINTS
#define GL_POINTS                  0x0000
#endif
#ifndef GL_LINES
#define GL_LINES                   0x0001
#endif
#ifndef GL_LINE_LOOP
#define GL_LINE_LOOP               0x0002
#endif
#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP              0x0003
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES               0x0004
#endif
#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP          0x0005
#endif
#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN            0x0006
#endif
#define GL_QUADS                   0x0007
#define GL_QUAD_STRIP              0x0008
#define GL_POLYGON                 0x0009

// Comparison functions
#ifndef GL_NEVER
#define GL_NEVER                   0x0200
#endif
#ifndef GL_LESS
#define GL_LESS                    0x0201
#endif
#ifndef GL_EQUAL
#define GL_EQUAL                   0x0202
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL                  0x0203
#endif
#ifndef GL_GREATER
#define GL_GREATER                 0x0204
#endif
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL                0x0205
#endif
#ifndef GL_GEQUAL
#define GL_GEQUAL                  0x0206
#endif
#ifndef GL_ALWAYS
#define GL_ALWAYS                  0x0207
#endif

// Stencil ops
#ifndef GL_KEEP
#define GL_KEEP                    0x1E00
#endif
#define GL_REPLACE_STENCIL         0x1E01
#ifndef GL_INCR
#define GL_INCR                    0x1E02
#endif
#ifndef GL_DECR
#define GL_DECR                    0x1E03
#endif
#define GL_INVERT_STENCILOP        0x150A
#ifndef GL_INCR_WRAP
#define GL_INCR_WRAP               0x8507
#endif
#ifndef GL_DECR_WRAP
#define GL_DECR_WRAP               0x8508
#endif

// Fog modes (guest GL 1.2 values)
#define GL_FOG_LINEAR              0x2601
#define GL_EXP                     0x0800
#define GL_EXP2                    0x0801

// Shade model
#define GL_FLAT                    0x1D00
#define GL_SMOOTH                  0x1D01

// Data types
#ifndef GL_BYTE
#define GL_BYTE                    0x1400
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE           0x1401
#endif
#ifndef GL_SHORT
#define GL_SHORT                   0x1402
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT          0x1403
#endif
#ifndef GL_INT
#define GL_INT                     0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT            0x1405
#endif
#ifndef GL_FLOAT
#define GL_FLOAT                   0x1406
#endif
#define GL_DOUBLE                  0x140A

// Client array types (GL 1.2 only)
#define GL_VERTEX_ARRAY            0x8074
#define GL_NORMAL_ARRAY            0x8075
#define GL_COLOR_ARRAY             0x8076
#define GL_TEXTURE_COORD_ARRAY     0x8078

// Interleaved vertex formats (GL 1.2 only)
#define GL_V2F                     0x2A20
#define GL_V3F                     0x2A21
#define GL_C4UB_V2F                0x2A22
#define GL_C4UB_V3F                0x2A23
#define GL_C3F_V3F                 0x2A24
#define GL_N3F_V3F                 0x2A25
#define GL_C4F_N3F_V3F             0x2A26
#define GL_T2F_V3F                 0x2A27
#define GL_T4F_V4F                 0x2A28
#define GL_T2F_C4UB_V3F            0x2A29
#define GL_T2F_C3F_V3F             0x2A2A
#define GL_T2F_N3F_V3F             0x2A2B
#define GL_T2F_C4F_N3F_V3F         0x2A2C
#define GL_T4F_C4F_N3F_V4F         0x2A2D

// Texture env modes (renamed to avoid GLES conflicts)
#define GL_MODULATE_TEX            0x2100
#define GL_DECAL_TEX               0x2101
#define GL_BLEND_TEXENV            0x0BE2
#define GL_REPLACE_TEX             0x1E01
#define GL_ADD_TEX                 0x0104

// Texture filter modes
#ifndef GL_NEAREST
#define GL_NEAREST                 0x2600
#endif
#define GL_LINEAR_FILTER           0x2601
#ifndef GL_NEAREST_MIPMAP_NEAREST
#define GL_NEAREST_MIPMAP_NEAREST  0x2700
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST   0x2701
#endif
#ifndef GL_NEAREST_MIPMAP_LINEAR
#define GL_NEAREST_MIPMAP_LINEAR   0x2702
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR    0x2703
#endif

// Texture wrap modes
#ifndef GL_REPEAT
#define GL_REPEAT                  0x2901
#endif
#define GL_CLAMP                   0x2900
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE           0x812F
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT         0x8370
#endif

#ifndef GL_RGBA
#define GL_RGBA                    0x1908
#endif

// Accumulation buffer ops (GL 1.2 only)
#define GL_ACCUM_OP                0x0100
#define GL_LOAD_OP                 0x0101
#define GL_RETURN_OP               0x0102
#define GL_MULT_OP                 0x0103
#define GL_ADD_OP                  0x0104

// ---- Saved GL state (RAVE pattern) ----
struct SavedGLState {
	GLint   fbo;
	GLint   viewport[4];
	GLboolean depthTest;
	GLboolean blend;
	GLboolean scissorTest;
	GLboolean stencilTest;
	GLboolean depthMask;
	GLint   blendSrcRGB, blendDstRGB;
	GLint   blendSrcAlpha, blendDstAlpha;
	GLint   activeProgram;
	GLint   boundVAO;
	GLint   boundVBO;
};

static void SaveGLState(SavedGLState *st)
{
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &st->fbo);
	glGetIntegerv(GL_VIEWPORT, st->viewport);
	st->depthTest   = glIsEnabled(GL_DEPTH_TEST);
	st->blend       = glIsEnabled(GL_BLEND);
	st->scissorTest = glIsEnabled(GL_SCISSOR_TEST);
	st->stencilTest = glIsEnabled(GL_STENCIL_TEST);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &st->depthMask);
	glGetIntegerv(GL_BLEND_SRC_RGB, &st->blendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &st->blendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &st->blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &st->blendDstAlpha);
	glGetIntegerv(GL_CURRENT_PROGRAM, &st->activeProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &st->boundVAO);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->boundVBO);
}

static void RestoreGLState(const SavedGLState *st)
{
	glBindFramebuffer(GL_FRAMEBUFFER, st->fbo);
	glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
	if (st->depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (st->scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (st->stencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
	glDepthMask(st->depthMask);
	glBlendFuncSeparate(st->blendSrcRGB, st->blendDstRGB, st->blendSrcAlpha, st->blendDstAlpha);
	glUseProgram(st->activeProgram);
	glBindVertexArray(st->boundVAO);
	glBindBuffer(GL_ARRAY_BUFFER, st->boundVBO);
}

// GL overlay compositing is now handled by the unified gles_compositor.
// GLMetalEndFrame registers the FBO texture via gles_compositor_set_overlay().

// ---- Vertex / GLES state ----
struct GLGLESVertex {
	float position[4];
	float color[4];
	float normal[3];
	float texcoord[2];
};

struct GLGLESState {
	GLuint program;
	bool initialized;
	bool renderPassActive;
	GLuint fbo, colorTexture, depthRenderbuffer;
	int fboWidth, fboHeight;
	GLuint vao, vbo;
	GLuint fallbackWhiteTexture;

	GLint loc_mvp_matrix, loc_modelview_matrix, loc_normal_matrix;
	GLint loc_lighting_enabled, loc_normalize_enabled;
	GLint loc_fog_enabled, loc_fog_mode, loc_fog_start, loc_fog_end, loc_fog_density;
	GLint loc_texenv_mode, loc_texenv_color, loc_fog_color;
	GLint loc_alpha_test_enabled, loc_alpha_func, loc_alpha_ref;
	GLint loc_has_texture, loc_texture0;

	struct LightLocs {
		GLint ambient, diffuse, specular, position;
		GLint spot_direction, spot_exponent, spot_cutoff;
		GLint constant_atten, linear_atten, quadratic_atten;
		GLint enabled;
	} light_locs[8];

	GLint loc_mat_ambient, loc_mat_diffuse, loc_mat_specular, loc_mat_emission, loc_mat_shininess;
	GLint loc_global_ambient;
};

static SavedGLState s_glPassSaved;

// ---- Matrix helpers ----
static void mat4_multiply(float *out, const float *a, const float *b)
{
	float tmp[16];
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			tmp[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
			                 a[1 * 4 + r] * b[c * 4 + 1] +
			                 a[2 * 4 + r] * b[c * 4 + 2] +
			                 a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
	memcpy(out, tmp, sizeof(float) * 16);
}

/* Normal matrix: 9 floats, column-major mat3 for GLES */
static void compute_normal_matrix(float *out9, const float *mv)
{
	float a = mv[0], b = mv[4], c = mv[8];
	float d = mv[1], e = mv[5], f = mv[9];
	float g = mv[2], h = mv[6], k = mv[10];

	float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
	if (fabsf(det) < 1e-12f) det = 1.0f;
	float inv_det = 1.0f / det;

	float inv[9];
	inv[0] = (e * k - f * h) * inv_det;
	inv[1] = (c * h - b * k) * inv_det;
	inv[2] = (b * f - c * e) * inv_det;
	inv[3] = (f * g - d * k) * inv_det;
	inv[4] = (a * k - c * g) * inv_det;
	inv[5] = (c * d - a * f) * inv_det;
	inv[6] = (d * h - e * g) * inv_det;
	inv[7] = (b * g - a * h) * inv_det;
	inv[8] = (a * e - b * d) * inv_det;

	out9[0] = inv[0]; out9[1] = inv[3]; out9[2] = inv[6];
	out9[3] = inv[1]; out9[4] = inv[4]; out9[5] = inv[7];
	out9[6] = inv[2]; out9[7] = inv[5]; out9[8] = inv[8];
}

static int32_t GLFogModeToShader(uint32_t gl_mode)
{
	switch (gl_mode) {
	case GL_FOG_LINEAR: return 1;
	case GL_EXP:        return 2;
	case GL_EXP2:       return 3;
	default:            return 0;
	}
}

static int32_t GLTexEnvModeToShader(uint32_t gl_mode)
{
	switch (gl_mode) {
	case GL_MODULATE_TEX:  return 0;
	case GL_DECAL_TEX:     return 1;
	case GL_BLEND_TEXENV:  return 2;
	case GL_REPLACE_TEX:   return 3;
	case GL_ADD_TEX:       return 4;
	default:               return 0;
	}
}

static int32_t GLAlphaFuncToShader(uint32_t gl_func)
{
	if (gl_func >= GL_NEVER && gl_func <= GL_ALWAYS)
		return (int32_t)(gl_func - GL_NEVER);
	return 7;
}

static GLenum GLBlendToGLES(uint32_t gl_blend)
{
	switch (gl_blend) {
	case GL_ZERO:                return GL_ZERO;
	case GL_ONE:                 return GL_ONE;
	case GL_SRC_COLOR:           return GL_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA:           return GL_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:           return GL_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
	case GL_DST_COLOR:           return GL_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA_SATURATE:  return GL_SRC_ALPHA_SATURATE;
	default:                     return GL_ONE;
	}
}

static GLenum GLDepthFuncToGLES(uint32_t gl_func)
{
	switch (gl_func) {
	case GL_NEVER:    return GL_NEVER;
	case GL_LESS:     return GL_LESS;
	case GL_EQUAL:    return GL_EQUAL;
	case GL_LEQUAL:   return GL_LEQUAL;
	case GL_GREATER:  return GL_GREATER;
	case GL_NOTEQUAL: return GL_NOTEQUAL;
	case GL_GEQUAL:   return GL_GEQUAL;
	case GL_ALWAYS:   return GL_ALWAYS;
	default:          return GL_LESS;
	}
}

static GLenum GLStencilOpToGLES(uint32_t gl_op)
{
	switch (gl_op) {
	case GL_KEEP:            return GL_KEEP;
	case 0x0000:             return GL_ZERO;
	case GL_REPLACE_STENCIL: return GL_REPLACE; /* same numeric value as GL_REPLACE texenv on desktop; ES stencil op */
	case GL_INCR:            return GL_INCR;
	case GL_DECR:            return GL_DECR;
	case GL_INVERT_STENCILOP: return GL_INVERT;
	case GL_INCR_WRAP:       return GL_INCR_WRAP;
	case GL_DECR_WRAP:       return GL_DECR_WRAP;
	default:                 return GL_KEEP;
	}
}

static void ConvertBGRA8ToRGBA8(const uint8_t *bgra, uint8_t *rgba, int numPixels)
{
	for (int i = 0; i < numPixels; i++) {
		const uint8_t *s = bgra + i * 4;
		uint8_t *d = rgba + i * 4;
		d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
	}
}

static GLenum GLMinFilterToGL(GLTextureObject *t)
{
	switch (t->min_filter) {
	case GL_NEAREST:
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return GL_NEAREST;
	default:
		return GL_LINEAR;
	}
}

static GLenum GLMagFilterToGL(GLTextureObject *t)
{
	return (t->mag_filter == GL_NEAREST) ? GL_NEAREST : GL_LINEAR;
}

static GLenum GLWrapToGL(GLuint wrap)
{
	switch (wrap) {
	case GL_CLAMP:
	case GL_CLAMP_TO_EDGE:
		return GL_CLAMP_TO_EDGE;
	case GL_MIRRORED_REPEAT:
		return GL_MIRRORED_REPEAT;
	case GL_REPEAT:
	default:
		return GL_REPEAT;
	}
}

static void ApplyTextureSamplerParams(GLTextureObject *texObj)
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GLMinFilterToGL(texObj));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GLMagFilterToGL(texObj));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GLWrapToGL(texObj->wrap_s));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GLWrapToGL(texObj->wrap_t));
}

static bool ExpandPrimitives(uint32_t gl_mode, const std::vector<GLVertex> &in,
                             std::vector<GLGLESVertex> &out, GLenum &gl_prim)
{
	auto copyVertex = [](GLGLESVertex &dst, const GLVertex &src) {
		memcpy(dst.position, src.position, sizeof(float) * 4);
		memcpy(dst.color, src.color, sizeof(float) * 4);
		memcpy(dst.normal, src.normal, sizeof(float) * 3);
		dst.texcoord[0] = src.texcoord[0][0];
		dst.texcoord[1] = src.texcoord[0][1];
	};

	size_t n = in.size();

	switch (gl_mode) {
	case GL_TRIANGLES:
		gl_prim = GL_TRIANGLES;
		return false;

	case GL_TRIANGLE_STRIP:
		gl_prim = GL_TRIANGLE_STRIP;
		return false;

	case GL_TRIANGLE_FAN: {
		gl_prim = GL_TRIANGLES;
		if (n < 3) return true;
		out.reserve((n - 2) * 3);
		for (size_t i = 1; i + 1 < n; i++) {
			GLGLESVertex v0, v1, v2;
			copyVertex(v0, in[0]);
			copyVertex(v1, in[i]);
			copyVertex(v2, in[i + 1]);
			out.push_back(v0);
			out.push_back(v1);
			out.push_back(v2);
		}
		return true;
	}

	case GL_QUADS: {
		gl_prim = GL_TRIANGLES;
		size_t numQuads = n / 4;
		out.reserve(numQuads * 6);
		for (size_t i = 0; i < numQuads; i++) {
			size_t base = i * 4;
			GLGLESVertex v0, v1, v2, v3;
			copyVertex(v0, in[base + 0]);
			copyVertex(v1, in[base + 1]);
			copyVertex(v2, in[base + 2]);
			copyVertex(v3, in[base + 3]);
			out.push_back(v0); out.push_back(v1); out.push_back(v2);
			out.push_back(v0); out.push_back(v2); out.push_back(v3);
		}
		return true;
	}

	case GL_QUAD_STRIP: {
		gl_prim = GL_TRIANGLES;
		if (n < 4) return true;
		size_t numQuads = (n - 2) / 2;
		out.reserve(numQuads * 6);
		for (size_t i = 0; i < numQuads; i++) {
			size_t base = i * 2;
			GLGLESVertex v0, v1, v2, v3;
			copyVertex(v0, in[base + 0]);
			copyVertex(v1, in[base + 1]);
			copyVertex(v2, in[base + 3]);
			copyVertex(v3, in[base + 2]);
			out.push_back(v0); out.push_back(v1); out.push_back(v2);
			out.push_back(v0); out.push_back(v2); out.push_back(v3);
		}
		return true;
	}

	case GL_POLYGON: {
		gl_prim = GL_TRIANGLES;
		if (n < 3) return true;
		out.reserve((n - 2) * 3);
		for (size_t i = 1; i + 1 < n; i++) {
			GLGLESVertex v0, v1, v2;
			copyVertex(v0, in[0]);
			copyVertex(v1, in[i]);
			copyVertex(v2, in[i + 1]);
			out.push_back(v0);
			out.push_back(v1);
			out.push_back(v2);
		}
		return true;
	}

	case GL_LINES:
		gl_prim = GL_LINES;
		return false;

	case GL_LINE_STRIP:
		gl_prim = GL_LINE_STRIP;
		return false;

	case GL_LINE_LOOP: {
		gl_prim = GL_LINES;
		if (n < 2) return true;
		out.reserve(n * 2);
		for (size_t i = 0; i + 1 < n; i++) {
			GLGLESVertex v0, v1;
			copyVertex(v0, in[i]);
			copyVertex(v1, in[i + 1]);
			out.push_back(v0);
			out.push_back(v1);
		}
		GLGLESVertex vLast, vFirst;
		copyVertex(vLast, in[n - 1]);
		copyVertex(vFirst, in[0]);
		out.push_back(vLast);
		out.push_back(vFirst);
		return true;
	}

	case GL_POINTS:
		gl_prim = GL_POINTS;
		return false;

	default:
		gl_prim = GL_TRIANGLES;
		return false;
	}
}

static void ConvertVertices(const std::vector<GLVertex> &in, std::vector<GLGLESVertex> &out)
{
	out.resize(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		memcpy(out[i].position, in[i].position, sizeof(float) * 4);
		memcpy(out[i].color, in[i].color, sizeof(float) * 4);
		memcpy(out[i].normal, in[i].normal, sizeof(float) * 3);
		out[i].texcoord[0] = in[i].texcoord[0][0];
		out[i].texcoord[1] = in[i].texcoord[0][1];
	}
}

static GLuint CompileShaderGL(GLenum type, const char *src)
{
	const char *version = "#version 300 es\n";
	const char *sources[2] = { version, src };
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 2, sources, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		GL_LOG("shader compile failed: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static bool CacheUniformLocations(GLGLESState *gs)
{
	GLuint p = gs->program;
	gs->loc_mvp_matrix = glGetUniformLocation(p, "u_mvp_matrix");
	gs->loc_modelview_matrix = glGetUniformLocation(p, "u_modelview_matrix");
	gs->loc_normal_matrix = glGetUniformLocation(p, "u_normal_matrix");
	gs->loc_lighting_enabled = glGetUniformLocation(p, "u_lighting_enabled");
	gs->loc_normalize_enabled = glGetUniformLocation(p, "u_normalize_enabled");
	gs->loc_fog_enabled = glGetUniformLocation(p, "u_fog_enabled");
	gs->loc_fog_mode = glGetUniformLocation(p, "u_fog_mode");
	gs->loc_fog_start = glGetUniformLocation(p, "u_fog_start");
	gs->loc_fog_end = glGetUniformLocation(p, "u_fog_end");
	gs->loc_fog_density = glGetUniformLocation(p, "u_fog_density");
	gs->loc_texenv_mode = glGetUniformLocation(p, "u_texenv_mode");
	gs->loc_texenv_color = glGetUniformLocation(p, "u_texenv_color");
	gs->loc_fog_color = glGetUniformLocation(p, "u_fog_color");
	gs->loc_alpha_test_enabled = glGetUniformLocation(p, "u_alpha_test_enabled");
	gs->loc_alpha_func = glGetUniformLocation(p, "u_alpha_func");
	gs->loc_alpha_ref = glGetUniformLocation(p, "u_alpha_ref");
	gs->loc_has_texture = glGetUniformLocation(p, "u_has_texture");
	gs->loc_texture0 = glGetUniformLocation(p, "u_texture0");
	gs->loc_mat_ambient = glGetUniformLocation(p, "u_mat_ambient");
	gs->loc_mat_diffuse = glGetUniformLocation(p, "u_mat_diffuse");
	gs->loc_mat_specular = glGetUniformLocation(p, "u_mat_specular");
	gs->loc_mat_emission = glGetUniformLocation(p, "u_mat_emission");
	gs->loc_mat_shininess = glGetUniformLocation(p, "u_mat_shininess");
	gs->loc_global_ambient = glGetUniformLocation(p, "u_global_ambient");

	char buf[64];
	for (int i = 0; i < 8; i++) {
		GLGLESState::LightLocs &L = gs->light_locs[i];
		snprintf(buf, sizeof(buf), "u_lights[%d].ambient", i);
		L.ambient = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].diffuse", i);
		L.diffuse = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].specular", i);
		L.specular = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].position", i);
		L.position = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].spot_direction", i);
		L.spot_direction = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].spot_exponent", i);
		L.spot_exponent = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].spot_cutoff", i);
		L.spot_cutoff = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].constant_atten", i);
		L.constant_atten = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].linear_atten", i);
		L.linear_atten = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].quadratic_atten", i);
		L.quadratic_atten = glGetUniformLocation(p, buf);
		snprintf(buf, sizeof(buf), "u_lights[%d].enabled", i);
		L.enabled = glGetUniformLocation(p, buf);
	}
	return gs->loc_mvp_matrix >= 0 && gs->loc_texture0 >= 0;
}

static void DestroyFBO(GLGLESState *gs)
{
	if (gs->depthRenderbuffer) {
		glDeleteRenderbuffers(1, &gs->depthRenderbuffer);
		gs->depthRenderbuffer = 0;
	}
	if (gs->colorTexture) {
		glDeleteTextures(1, &gs->colorTexture);
		gs->colorTexture = 0;
	}
	if (gs->fbo) {
		glDeleteFramebuffers(1, &gs->fbo);
		gs->fbo = 0;
	}
	gs->fboWidth = gs->fboHeight = 0;
}

static void ResizeFBO(GLGLESState *gs, int w, int h)
{
	if (w <= 0) w = 640;
	if (h <= 0) h = 480;
	if (gs->fbo && gs->fboWidth == w && gs->fboHeight == h)
		return;

	DestroyFBO(gs);

	glGenFramebuffers(1, &gs->fbo);
	glGenTextures(1, &gs->colorTexture);
	glBindTexture(GL_TEXTURE_2D, gs->colorTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glGenRenderbuffers(1, &gs->depthRenderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, gs->depthRenderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

	glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gs->colorTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gs->depthRenderbuffer);

	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		GL_LOG("FBO incomplete status=0x%x", st);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	gs->fboWidth = w;
	gs->fboHeight = h;
}

static void UploadUniforms(GLContext *ctx, GLGLESState *gs)
{
	const float *mv = ctx->modelview_stack[ctx->modelview_depth];
	const float *proj = ctx->projection_stack[ctx->projection_depth];
	float mvp[16];
	mat4_multiply(mvp, proj, mv);

	float nm[9];
	compute_normal_matrix(nm, mv);

	glUniformMatrix4fv(gs->loc_mvp_matrix, 1, GL_FALSE, mvp);
	glUniformMatrix4fv(gs->loc_modelview_matrix, 1, GL_FALSE, mv);
	glUniformMatrix3fv(gs->loc_normal_matrix, 1, GL_FALSE, nm);

	glUniform1i(gs->loc_lighting_enabled, ctx->lighting_enabled ? 1 : 0);
	glUniform1i(gs->loc_normalize_enabled, ctx->normalize ? 1 : 0);
	glUniform1i(gs->loc_fog_enabled, ctx->fog_enabled ? 1 : 0);
	glUniform1i(gs->loc_fog_mode, ctx->fog_enabled ? GLFogModeToShader(ctx->fog_mode) : 0);
	glUniform1f(gs->loc_fog_start, ctx->fog_start);
	glUniform1f(gs->loc_fog_end, ctx->fog_end);
	glUniform1f(gs->loc_fog_density, ctx->fog_density);

	int texUnit = ctx->active_texture;
	glUniform1i(gs->loc_texenv_mode, GLTexEnvModeToShader(ctx->tex_units[texUnit].env_mode));
	glUniform4fv(gs->loc_texenv_color, 1, ctx->tex_units[texUnit].env_color);

	if (ctx->fog_enabled) {
		glUniform4fv(gs->loc_fog_color, 1, ctx->fog_color);
	} else {
		float fc[4] = { 0, 0, 0, -1.0f };
		glUniform4fv(gs->loc_fog_color, 1, fc);
	}

	glUniform1i(gs->loc_alpha_test_enabled, ctx->alpha_test ? 1 : 0);
	glUniform1i(gs->loc_alpha_func, GLAlphaFuncToShader(ctx->alpha_func));
	glUniform1f(gs->loc_alpha_ref, ctx->alpha_ref);

	bool hasTex = ctx->tex_units[texUnit].enabled_2d && ctx->tex_units[texUnit].bound_texture_2d != 0;
	glUniform1i(gs->loc_has_texture, hasTex ? 1 : 0);

	for (int i = 0; i < 8; i++) {
		const GLLight &L = ctx->lights[i];
		GLGLESState::LightLocs &loc = gs->light_locs[i];
		if (loc.ambient >= 0) glUniform4fv(loc.ambient, 1, L.ambient);
		if (loc.diffuse >= 0) glUniform4fv(loc.diffuse, 1, L.diffuse);
		if (loc.specular >= 0) glUniform4fv(loc.specular, 1, L.specular);
		if (loc.position >= 0) glUniform4fv(loc.position, 1, L.position);
		if (loc.spot_direction >= 0) glUniform3fv(loc.spot_direction, 1, L.spot_direction);
		if (loc.spot_exponent >= 0) glUniform1f(loc.spot_exponent, L.spot_exponent);
		float sc = (L.spot_cutoff >= 180.0f) ? -1.0f : cosf(L.spot_cutoff * (float)M_PI / 180.0f);
		if (loc.spot_cutoff >= 0) glUniform1f(loc.spot_cutoff, sc);
		if (loc.constant_atten >= 0) glUniform1f(loc.constant_atten, L.constant_attenuation);
		if (loc.linear_atten >= 0) glUniform1f(loc.linear_atten, L.linear_attenuation);
		if (loc.quadratic_atten >= 0) glUniform1f(loc.quadratic_atten, L.quadratic_attenuation);
		if (loc.enabled >= 0) glUniform1i(loc.enabled, L.enabled ? 1 : 0);
	}

	const GLMaterial &M = ctx->materials[0];
	glUniform4fv(gs->loc_mat_ambient, 1, M.ambient);
	glUniform4fv(gs->loc_mat_diffuse, 1, M.diffuse);
	glUniform4fv(gs->loc_mat_specular, 1, M.specular);
	glUniform4fv(gs->loc_mat_emission, 1, M.emission);
	glUniform1f(gs->loc_mat_shininess, M.shininess);
	glUniform4fv(gs->loc_global_ambient, 1, ctx->light_model_ambient);
}

static void ApplyFixedFunctionGLState(GLContext *ctx)
{
	glColorMask(ctx->color_mask[0], ctx->color_mask[1], ctx->color_mask[2], ctx->color_mask[3]);

	if (ctx->depth_test) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GLDepthFuncToGLES(ctx->depth_func));
	} else {
		glDisable(GL_DEPTH_TEST);
	}
	glDepthMask(ctx->depth_mask ? GL_TRUE : GL_FALSE);

	if (ctx->blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GLBlendToGLES(ctx->blend_src), GLBlendToGLES(ctx->blend_dst));
	} else {
		glDisable(GL_BLEND);
	}

	if (ctx->cull_face_enabled) {
		glEnable(GL_CULL_FACE);
		glCullFace(ctx->cull_face_mode == 0x0405 ? GL_BACK : GL_FRONT);
		glFrontFace(ctx->front_face == 0x0901 ? GL_CCW : GL_CW);
	} else {
		glDisable(GL_CULL_FACE);
	}

	if (ctx->scissor_test) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(ctx->scissor_box[0], ctx->scissor_box[1], ctx->scissor_box[2], ctx->scissor_box[3]);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}

	if (ctx->stencil_test) {
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GLDepthFuncToGLES(ctx->stencil.func), ctx->stencil.ref & 0xFF, ctx->stencil.value_mask & 0xFF);
		glStencilMask(ctx->stencil.write_mask & 0xFF);
		glStencilOp(GLStencilOpToGLES(ctx->stencil.sfail),
		            GLStencilOpToGLES(ctx->stencil.dpfail),
		            GLStencilOpToGLES(ctx->stencil.dppass));
	} else {
		glDisable(GL_STENCIL_TEST);
	}
}

static void DrawPreparedVertices(GLContext *ctx, GLGLESState *gs,
                                 const GLGLESVertex *vertData, size_t vertCount, GLenum gl_prim)
{
	if (vertCount == 0) return;

	glUseProgram(gs->program);
	UploadUniforms(ctx, gs);
	ApplyFixedFunctionGLState(ctx);

	int texUnit = ctx->active_texture;
	bool hasTex = ctx->tex_units[texUnit].enabled_2d && ctx->tex_units[texUnit].bound_texture_2d != 0;

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gs->loc_texture0, 0);

	if (hasTex) {
		uint32_t texName = ctx->tex_units[texUnit].bound_texture_2d;
		auto it = ctx->texture_objects.find(texName);
		if (it != ctx->texture_objects.end() && it->second.metal_texture) {
			GLuint tid = (GLuint)(uintptr_t)it->second.metal_texture;
			glBindTexture(GL_TEXTURE_2D, tid);
			ApplyTextureSamplerParams(&it->second);
		} else {
			glBindTexture(GL_TEXTURE_2D, gs->fallbackWhiteTexture);
			GL_LOG("missing GPU texture for name %u, using white", texName);
		}
	} else {
		glBindTexture(GL_TEXTURE_2D, gs->fallbackWhiteTexture);
	}

	glBindVertexArray(gs->vao);
	glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertCount * sizeof(GLGLESVertex)), vertData, GL_STREAM_DRAW);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)16);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)32);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)44);

	glDrawArrays(gl_prim, 0, (GLsizei)vertCount);

	glDisableVertexAttribArray(3);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void GLMetalInit(GLContext *ctx)
{
	if (ctx->metal) return;

	GLGLESState *gs = new GLGLESState();
	memset(gs, 0, sizeof(*gs));

	GLuint vs = CompileShaderGL(GL_VERTEX_SHADER, gl_vertex_src);
	GLuint fs = CompileShaderGL(GL_FRAGMENT_SHADER, gl_fragment_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		delete gs;
		GL_LOG("GLMetalInit: shader compile failed");
		return;
	}

	gs->program = glCreateProgram();
	glAttachShader(gs->program, vs);
	glAttachShader(gs->program, fs);
	glBindAttribLocation(gs->program, 0, "a_position");
	glBindAttribLocation(gs->program, 1, "a_color");
	glBindAttribLocation(gs->program, 2, "a_normal");
	glBindAttribLocation(gs->program, 3, "a_texcoord");
	glLinkProgram(gs->program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = 0;
	glGetProgramiv(gs->program, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		glGetProgramInfoLog(gs->program, sizeof(log), NULL, log);
		GL_LOG("GLMetalInit: link failed: %s", log);
		glDeleteProgram(gs->program);
		delete gs;
		return;
	}

	if (!CacheUniformLocations(gs)) {
		GL_LOG("GLMetalInit: uniform location cache incomplete");
		glDeleteProgram(gs->program);
		delete gs;
		return;
	}

	glGenVertexArrays(1, &gs->vao);
	glGenBuffers(1, &gs->vbo);

	glGenTextures(1, &gs->fallbackWhiteTexture);
	glBindTexture(GL_TEXTURE_2D, gs->fallbackWhiteTexture);
	uint8_t white[4] = { 255, 255, 255, 255 };
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	int vw = ctx->viewport[2] > 0 ? ctx->viewport[2] : 640;
	int vh = ctx->viewport[3] > 0 ? ctx->viewport[3] : 480;
	ResizeFBO(gs, vw, vh);

	gs->initialized = true;
	ctx->metal = (void *)gs;
	GL_LOG("GLMetalInit: OK program=%u fbo=%dx%d", gs->program, gs->fboWidth, gs->fboHeight);
}

void GLMetalBeginFrame(GLContext *ctx)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized) return;
	if (gs->renderPassActive) return;

	int vw = ctx->viewport[2] > 0 ? ctx->viewport[2] : 640;
	int vh = ctx->viewport[3] > 0 ? ctx->viewport[3] : 480;
	if (vw > gs->fboWidth || vh > gs->fboHeight)
		ResizeFBO(gs, vw > gs->fboWidth ? vw : gs->fboWidth, vh > gs->fboHeight ? vh : gs->fboHeight);

	SaveGLState(&s_glPassSaved);
	glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
	glViewport(ctx->viewport[0], ctx->viewport[1], ctx->viewport[2], ctx->viewport[3]);

	glClearColor(ctx->clear_color[0], ctx->clear_color[1], ctx->clear_color[2], 1.0f);
	glClearDepthf(ctx->clear_depth);
	glClearStencil(ctx->clear_stencil);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	if (ctx->scissor_test) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(ctx->scissor_box[0], ctx->scissor_box[1], ctx->scissor_box[2], ctx->scissor_box[3]);
	}

	gs->renderPassActive = true;
}

void GLMetalFlushImmediateMode(GLContext *ctx)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized) return;
	if (ctx->im_vertices.empty()) return;

	if (!gs->renderPassActive) {
		GLMetalBeginFrame(ctx);
		if (!gs->renderPassActive) return;
	}

	GLenum gl_prim;
	std::vector<GLGLESVertex> expandedVerts;
	bool expanded = ExpandPrimitives(ctx->im_mode, ctx->im_vertices, expandedVerts, gl_prim);

	const GLGLESVertex *vertData;
	size_t vertCount;

	if (expanded) {
		vertData = expandedVerts.data();
		vertCount = expandedVerts.size();
	} else {
		ConvertVertices(ctx->im_vertices, expandedVerts);
		vertData = expandedVerts.data();
		vertCount = expandedVerts.size();
	}

	if (vertCount == 0) return;

	DrawPreparedVertices(ctx, gs, vertData, vertCount, gl_prim);
}

static void DrawTexturedNDCQuad(GLContext *ctx, GLGLESState *gs,
                                const GLGLESVertex *verts4, GLuint texId,
                                bool forceBlendSrcAlpha)
{
	if (!gs->renderPassActive) {
		GLMetalBeginFrame(ctx);
		if (!gs->renderPassActive) return;
	}

	static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	float nm[9] = { 1,0,0, 0,1,0, 0,0,1 };

	glUseProgram(gs->program);
	glUniformMatrix4fv(gs->loc_mvp_matrix, 1, GL_FALSE, identity);
	glUniformMatrix4fv(gs->loc_modelview_matrix, 1, GL_FALSE, identity);
	glUniformMatrix3fv(gs->loc_normal_matrix, 1, GL_FALSE, nm);

	glUniform1i(gs->loc_lighting_enabled, 0);
	glUniform1i(gs->loc_normalize_enabled, 0);
	glUniform1i(gs->loc_fog_enabled, 0);
	glUniform1i(gs->loc_fog_mode, 0);
	glUniform1f(gs->loc_fog_start, 0);
	glUniform1f(gs->loc_fog_end, 1);
	glUniform1f(gs->loc_fog_density, 0);

	glUniform1i(gs->loc_texenv_mode, 3);
	float one4[4] = { 1,1,1,1 };
	glUniform4fv(gs->loc_texenv_color, 1, one4);
	float fogOff[4] = { 0,0,0,-1 };
	glUniform4fv(gs->loc_fog_color, 1, fogOff);
	glUniform1i(gs->loc_alpha_test_enabled, 0);
	glUniform1i(gs->loc_alpha_func, 0);
	glUniform1f(gs->loc_alpha_ref, 0);
	glUniform1i(gs->loc_has_texture, 1);

	for (int i = 0; i < 8; i++) {
		GLGLESState::LightLocs &loc = gs->light_locs[i];
		if (loc.enabled >= 0) glUniform1i(loc.enabled, 0);
	}
	float zmat[4] = { 0,0,0,0 };
	glUniform4fv(gs->loc_mat_ambient, 1, zmat);
	glUniform4fv(gs->loc_mat_diffuse, 1, zmat);
	glUniform4fv(gs->loc_mat_specular, 1, zmat);
	glUniform4fv(gs->loc_mat_emission, 1, zmat);
	glUniform1f(gs->loc_mat_shininess, 0);
	glUniform4fv(gs->loc_global_ambient, 1, zmat);

	glColorMask(ctx->color_mask[0], ctx->color_mask[1], ctx->color_mask[2], ctx->color_mask[3]);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	if (forceBlendSrcAlpha) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else if (ctx->blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GLBlendToGLES(ctx->blend_src), GLBlendToGLES(ctx->blend_dst));
	} else {
		glDisable(GL_BLEND);
	}

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gs->loc_texture0, 0);
	glBindTexture(GL_TEXTURE_2D, texId);

	glBindVertexArray(gs->vao);
	glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLGLESVertex) * 4, verts4, GL_STREAM_DRAW);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)16);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)32);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(GLGLESVertex), (void *)44);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(3);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void GLMetalDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized || !bgra_data || width <= 0 || height <= 0) return;

	int npix = width * height;
	std::vector<uint8_t> rgba((size_t)npix * 4);
	ConvertBGRA8ToRGBA8(bgra_data, rgba.data(), npix);

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	float vp_x = (float)ctx->viewport[0];
	float vp_y = (float)ctx->viewport[1];
	float vp_w = (float)ctx->viewport[2];
	float vp_h = (float)ctx->viewport[3];
	if (vp_w <= 0 || vp_h <= 0) {
		glDeleteTextures(1, &tex);
		return;
	}

	float win_x = ctx->raster_pos[0];
	float win_y = ctx->raster_pos[1];
	float quad_w = (float)width * ctx->pixel_zoom_x;
	float quad_h = (float)height * ctx->pixel_zoom_y;

	float ndc_x0 = (win_x - vp_x) / vp_w * 2.0f - 1.0f;
	float ndc_y0 = (win_y - vp_y) / vp_h * 2.0f - 1.0f;
	float ndc_x1 = ndc_x0 + quad_w / vp_w * 2.0f;
	float ndc_y1 = ndc_y0 + quad_h / vp_h * 2.0f;

	GLGLESVertex verts[4];
	memset(verts, 0, sizeof(verts));
	verts[0].position[0] = ndc_x0; verts[0].position[1] = ndc_y0; verts[0].position[2] = 0; verts[0].position[3] = 1;
	verts[0].color[0] = verts[0].color[1] = verts[0].color[2] = verts[0].color[3] = 1;
	verts[0].texcoord[0] = 0; verts[0].texcoord[1] = 1;

	verts[1].position[0] = ndc_x1; verts[1].position[1] = ndc_y0; verts[1].position[2] = 0; verts[1].position[3] = 1;
	verts[1].color[0] = verts[1].color[1] = verts[1].color[2] = verts[1].color[3] = 1;
	verts[1].texcoord[0] = 1; verts[1].texcoord[1] = 1;

	verts[2].position[0] = ndc_x0; verts[2].position[1] = ndc_y1; verts[2].position[2] = 0; verts[2].position[3] = 1;
	verts[2].color[0] = verts[2].color[1] = verts[2].color[2] = verts[2].color[3] = 1;
	verts[2].texcoord[0] = 0; verts[2].texcoord[1] = 0;

	verts[3].position[0] = ndc_x1; verts[3].position[1] = ndc_y1; verts[3].position[2] = 0; verts[3].position[3] = 1;
	verts[3].color[0] = verts[3].color[1] = verts[3].color[2] = verts[3].color[3] = 1;
	verts[3].texcoord[0] = 1; verts[3].texcoord[1] = 0;

	DrawTexturedNDCQuad(ctx, gs, verts, tex, false);
	glDeleteTextures(1, &tex);
	(void)data_len;
}

void GLMetalBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized || !bgra_data || width <= 0 || height <= 0) return;

	int npix = width * height;
	std::vector<uint8_t> rgba((size_t)npix * 4);
	ConvertBGRA8ToRGBA8(bgra_data, rgba.data(), npix);

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	float vp_x = (float)ctx->viewport[0];
	float vp_y = (float)ctx->viewport[1];
	float vp_w = (float)ctx->viewport[2];
	float vp_h = (float)ctx->viewport[3];
	if (vp_w <= 0 || vp_h <= 0) {
		glDeleteTextures(1, &tex);
		return;
	}

	float win_x = ctx->raster_pos[0];
	float win_y = ctx->raster_pos[1];
	float quad_w = (float)width * ctx->pixel_zoom_x;
	float quad_h = (float)height * ctx->pixel_zoom_y;

	float ndc_x0 = (win_x - vp_x) / vp_w * 2.0f - 1.0f;
	float ndc_y0 = (win_y - vp_y) / vp_h * 2.0f - 1.0f;
	float ndc_x1 = ndc_x0 + quad_w / vp_w * 2.0f;
	float ndc_y1 = ndc_y0 + quad_h / vp_h * 2.0f;

	GLGLESVertex verts[4];
	memset(verts, 0, sizeof(verts));
	verts[0].position[0] = ndc_x0; verts[0].position[1] = ndc_y0; verts[0].position[2] = 0; verts[0].position[3] = 1;
	verts[0].color[0] = verts[0].color[1] = verts[0].color[2] = verts[0].color[3] = 1;
	verts[0].texcoord[0] = 0; verts[0].texcoord[1] = 1;
	verts[1].position[0] = ndc_x1; verts[1].position[1] = ndc_y0; verts[1].position[2] = 0; verts[1].position[3] = 1;
	verts[1].color[0] = verts[1].color[1] = verts[1].color[2] = verts[1].color[3] = 1;
	verts[1].texcoord[0] = 1; verts[1].texcoord[1] = 1;
	verts[2].position[0] = ndc_x0; verts[2].position[1] = ndc_y1; verts[2].position[2] = 0; verts[2].position[3] = 1;
	verts[2].color[0] = verts[2].color[1] = verts[2].color[2] = verts[2].color[3] = 1;
	verts[2].texcoord[0] = 0; verts[2].texcoord[1] = 0;
	verts[3].position[0] = ndc_x1; verts[3].position[1] = ndc_y1; verts[3].position[2] = 0; verts[3].position[3] = 1;
	verts[3].color[0] = verts[3].color[1] = verts[3].color[2] = verts[3].color[3] = 1;
	verts[3].texcoord[0] = 1; verts[3].texcoord[1] = 0;

	DrawTexturedNDCQuad(ctx, gs, verts, tex, true);
	glDeleteTextures(1, &tex);
	(void)data_len;
}

void GLMetalEndFrame(GLContext *ctx)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized || !gs->renderPassActive) return;

	glFlush();

	// Read FBO pixels and blit to the_buffer + the_buffer_copy for proper
	// QuickDraw z-order integration via VOSF (same approach as RAVE renderer).
	{
		int w = gs->fboWidth;
		int h = gs->fboHeight;
		int dstLeft = ctx->viewport[0];
		int dstTop  = ctx->viewport[1];

		static uint8_t *tmpBuf = nullptr;
		static int tmpBufSize = 0;
		int needed = w * h * 4;
		if (!tmpBuf || tmpBufSize < needed) {
			delete[] tmpBuf;
			tmpBuf = new uint8_t[needed];
			tmpBufSize = needed;
		}
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmpBuf);
		video_blit_rave_fbo(tmpBuf, w, h, dstLeft, dstTop);
	}

	gles_compositor_set_overlay(COMPOSITOR_OVERLAY_GL,
	                           (uint32_t)gs->colorTexture,
	                           gs->fboWidth, gs->fboHeight,
	                           ctx->viewport[0], ctx->viewport[1],
	                           gs->fboWidth, gs->fboHeight,
	                           VModes[cur_mode].viXsize,
	                           VModes[cur_mode].viYsize);

	gs->renderPassActive = false;
	RestoreGLState(&s_glPassSaved);
	video_request_gl_refresh();
	GL_LOG("GLMetalEndFrame");
}

void NativeGLFinish(GLContext *ctx)
{
	(void)ctx;
	glFinish();
}

void NativeGLFlush(GLContext *ctx)
{
	(void)ctx;
	glFlush();
}

void GLMetalRelease(GLContext *ctx)
{
	if (!ctx->metal) return;
	GLGLESState *gs = (GLGLESState *)ctx->metal;

	gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_GL);

	for (auto &pair : ctx->texture_objects) {
		if (pair.second.metal_texture) {
			GLuint tid = (GLuint)(uintptr_t)pair.second.metal_texture;
			glDeleteTextures(1, &tid);
			pair.second.metal_texture = nullptr;
		}
	}
	ctx->texture_objects.clear();

	if (gs->program) glDeleteProgram(gs->program);
	if (gs->vao) glDeleteVertexArrays(1, &gs->vao);
	if (gs->vbo) glDeleteBuffers(1, &gs->vbo);
	if (gs->fallbackWhiteTexture) glDeleteTextures(1, &gs->fallbackWhiteTexture);
	DestroyFBO(gs);

	delete gs;
	ctx->metal = nullptr;
	GL_LOG("GLMetalRelease");
}

void GLMetalUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                          int width, int height, const uint8_t *data, int dataLen)
{
	(void)ctx;
	if (!data || width <= 0 || height <= 0) return;

	int bpp = 4;
	int needBytes = width * height * bpp;
	if (dataLen > 0 && dataLen < needBytes)
		return;

	std::vector<uint8_t> rgba((size_t)width * height * 4);
	ConvertBGRA8ToRGBA8(data, rgba.data(), width * height);

	GLuint tid = texObj->metal_texture ? (GLuint)(uintptr_t)texObj->metal_texture : 0;

	if (tid == 0) {
		glGenTextures(1, &tid);
		texObj->metal_texture = (void *)(uintptr_t)tid;
	}

	if (level == 0 && (texObj->width != width || texObj->height != height)) {
		glDeleteTextures(1, &tid);
		glGenTextures(1, &tid);
		texObj->metal_texture = (void *)(uintptr_t)tid;
		texObj->width = width;
		texObj->height = height;
	}

	glBindTexture(GL_TEXTURE_2D, tid);
	ApplyTextureSamplerParams(texObj);
	glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	if (level == 0) {
		texObj->width = width;
		texObj->height = height;
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	(void)dataLen;
}

void GLMetalUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                             int xoff, int yoff, int w, int h,
                             const uint8_t *data, int bytesPerRow)
{
	(void)ctx;
	if (!texObj->metal_texture || !data || w <= 0 || h <= 0) return;

	GLuint tid = (GLuint)(uintptr_t)texObj->metal_texture;
	std::vector<uint8_t> rgba((size_t)w * h * 4);
	for (int row = 0; row < h; row++) {
		const uint8_t *src = data + row * bytesPerRow;
		uint8_t *dst = rgba.data() + row * w * 4;
		ConvertBGRA8ToRGBA8(src, dst, w);
	}

	glBindTexture(GL_TEXTURE_2D, tid);
	ApplyTextureSamplerParams(texObj);
	glTexSubImage2D(GL_TEXTURE_2D, level, xoff, yoff, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_2D, 0);
}

void GLMetalDestroyTexture(GLTextureObject *texObj)
{
	if (texObj->metal_texture) {
		GLuint tid = (GLuint)(uintptr_t)texObj->metal_texture;
		glDeleteTextures(1, &tid);
		texObj->metal_texture = nullptr;
	}
	texObj->width = 0;
	texObj->height = 0;
	texObj->depth = 0;
}

void GLMetalUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level,
                            int width, int height, int depth,
                            const uint8_t *data, int dataLen)
{
	(void)ctx;
#if defined(GL_ES_VERSION_3_0)
	if (!data || width <= 0 || height <= 0 || depth <= 0) return;
	int sliceSize = width * height * 4;
	int total = sliceSize * depth;
	if (dataLen > 0 && dataLen < total)
		return;

	std::vector<uint8_t> rgba((size_t)total);
	ConvertBGRA8ToRGBA8(data, rgba.data(), width * height * depth);

	GLuint tid = texObj->metal_texture ? (GLuint)(uintptr_t)texObj->metal_texture : 0;
	bool needNew = (tid == 0) || (level == 0 && (texObj->width != width || texObj->height != height || texObj->depth != depth));

	if (needNew && level == 0) {
		if (tid) {
			glDeleteTextures(1, &tid);
			tid = 0;
		}
		glGenTextures(1, &tid);
		texObj->metal_texture = (void *)(uintptr_t)tid;
		texObj->width = width;
		texObj->height = height;
		texObj->depth = depth;
	}

	if (tid == 0) return;

	glBindTexture(GL_TEXTURE_3D, tid);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexImage3D(GL_TEXTURE_3D, level, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_3D, 0);
#else
	(void)texObj; (void)level; (void)width; (void)height; (void)depth; (void)data; (void)dataLen;
	GL_LOG("GLMetalUpload3DTexture: GLES3 required, stubbed");
#endif
}

void GLMetalUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level,
                               int xoff, int yoff, int zoff,
                               int w, int h, int d,
                               const uint8_t *data, int bytesPerRow, int bytesPerImage)
{
	(void)ctx;
#if defined(GL_ES_VERSION_3_0)
	if (!texObj->metal_texture || !data || w <= 0 || h <= 0 || d <= 0) return;

	GLuint tid = (GLuint)(uintptr_t)texObj->metal_texture;
	std::vector<uint8_t> rgba((size_t)w * h * d * 4);
	for (int z = 0; z < d; z++) {
		for (int row = 0; row < h; row++) {
			const uint8_t *src = data + z * bytesPerImage + row * bytesPerRow;
			uint8_t *dst = rgba.data() + (z * w * h + row * w) * 4;
			ConvertBGRA8ToRGBA8(src, dst, w);
		}
	}

	glBindTexture(GL_TEXTURE_3D, tid);
	glTexSubImage3D(GL_TEXTURE_3D, level, xoff, yoff, zoff, w, h, d, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_3D, 0);
#else
	(void)texObj; (void)level; (void)xoff; (void)yoff; (void)zoff;
	(void)w; (void)h; (void)d; (void)data; (void)bytesPerRow; (void)bytesPerImage;
	GL_LOG("GLMetalUploadSubTexture3D: GLES3 required, stubbed");
#endif
}

void NativeGLReadPixels(GLContext *ctx, int32_t x, int32_t y, int32_t width, int32_t height,
                        uint32_t format, uint32_t type, uint32_t mac_pixels)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized || mac_pixels == 0 || width <= 0 || height <= 0) return;

	SavedGLState st;
	SaveGLState(&st);
	glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, ctx->pixel_store.pack_alignment > 0 ? ctx->pixel_store.pack_alignment : 4);

	std::vector<uint8_t> rgba((size_t)width * height * 4);
	glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

	int packAlign = ctx->pixel_store.pack_alignment;
	if (packAlign < 1) packAlign = 4;

	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			const uint8_t *src = rgba.data() + (row * width + col) * 4;
			uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
			if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
				uint32_t dstAddr = mac_pixels + (row * width + col) * 4;
				WriteMacInt8(dstAddr + 0, r);
				WriteMacInt8(dstAddr + 1, g);
				WriteMacInt8(dstAddr + 2, b);
				WriteMacInt8(dstAddr + 3, a);
			} else {
				uint32_t dstAddr = mac_pixels + (row * width + col) * 4;
				WriteMacInt8(dstAddr + 0, r);
				WriteMacInt8(dstAddr + 1, g);
				WriteMacInt8(dstAddr + 2, b);
				WriteMacInt8(dstAddr + 3, a);
			}
		}
	}
	(void)packAlign;

	RestoreGLState(&st);
}

static void gl_accum_ensure_allocated(GLContext *ctx, int width, int height)
{
	if (ctx->accum_allocated && ctx->accum_width == width && ctx->accum_height == height)
		return;

	if (ctx->accum_buffer) {
		free(ctx->accum_buffer);
		ctx->accum_buffer = nullptr;
	}

	ctx->accum_buffer = (float *)calloc((size_t)width * height * 4, sizeof(float));
	if (ctx->accum_buffer) {
		ctx->accum_width = width;
		ctx->accum_height = height;
		ctx->accum_allocated = true;
	} else {
		ctx->accum_allocated = false;
	}
}

static float *gl_accum_read_framebuffer(GLContext *ctx, int *out_w, int *out_h)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized) return nullptr;

	SavedGLState st;
	SaveGLState(&st);
	glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);

	int width = gs->fboWidth;
	int height = gs->fboHeight;
	std::vector<uint8_t> rgba((size_t)width * height * 4);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

	float *result = (float *)malloc((size_t)width * height * 4 * sizeof(float));
	if (!result) {
		RestoreGLState(&st);
		return nullptr;
	}
	for (int i = 0; i < width * height; i++) {
		result[i * 4 + 0] = rgba[i * 4 + 0] / 255.0f;
		result[i * 4 + 1] = rgba[i * 4 + 1] / 255.0f;
		result[i * 4 + 2] = rgba[i * 4 + 2] / 255.0f;
		result[i * 4 + 3] = rgba[i * 4 + 3] / 255.0f;
	}
	*out_w = width;
	*out_h = height;
	RestoreGLState(&st);
	return result;
}

static void gl_accum_write_framebuffer(GLContext *ctx, float scale)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized || !ctx->accum_allocated) return;

	int width = ctx->accum_width;
	int height = ctx->accum_height;
	int dstW = gs->fboWidth;
	int dstH = gs->fboHeight;
	int w = (width < dstW) ? width : dstW;
	int h = (height < dstH) ? height : dstH;

	std::vector<uint8_t> pixels((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) {
		float r = ctx->accum_buffer[i * 4 + 0] * scale;
		float g = ctx->accum_buffer[i * 4 + 1] * scale;
		float b = ctx->accum_buffer[i * 4 + 2] * scale;
		float a = ctx->accum_buffer[i * 4 + 3] * scale;
		r = r < 0 ? 0 : (r > 1 ? 1 : r);
		g = g < 0 ? 0 : (g > 1 ? 1 : g);
		b = b < 0 ? 0 : (b > 1 ? 1 : b);
		a = a < 0 ? 0 : (a > 1 ? 1 : a);
		pixels[i * 4 + 0] = (uint8_t)(r * 255.0f + 0.5f);
		pixels[i * 4 + 1] = (uint8_t)(g * 255.0f + 0.5f);
		pixels[i * 4 + 2] = (uint8_t)(b * 255.0f + 0.5f);
		pixels[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
	}

	glBindTexture(GL_TEXTURE_2D, gs->colorTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindTexture(GL_TEXTURE_2D, 0);
}

void NativeGLAccum(GLContext *ctx, uint32_t op, float value)
{
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized) return;

	int fb_w = 0, fb_h = 0;

	switch (op) {
	case GL_ACCUM_OP: {
		float *fb = gl_accum_read_framebuffer(ctx, &fb_w, &fb_h);
		if (!fb) return;
		gl_accum_ensure_allocated(ctx, fb_w, fb_h);
		if (!ctx->accum_allocated) { free(fb); return; }
		int n = fb_w * fb_h * 4;
		for (int i = 0; i < n; i++)
			ctx->accum_buffer[i] += fb[i] * value;
		free(fb);
		break;
	}
	case GL_LOAD_OP: {
		float *fb = gl_accum_read_framebuffer(ctx, &fb_w, &fb_h);
		if (!fb) return;
		gl_accum_ensure_allocated(ctx, fb_w, fb_h);
		if (!ctx->accum_allocated) { free(fb); return; }
		int n = fb_w * fb_h * 4;
		for (int i = 0; i < n; i++)
			ctx->accum_buffer[i] = fb[i] * value;
		free(fb);
		break;
	}
	case GL_MULT_OP: {
		if (!ctx->accum_allocated) return;
		int n = ctx->accum_width * ctx->accum_height * 4;
		for (int i = 0; i < n; i++)
			ctx->accum_buffer[i] *= value;
		break;
	}
	case GL_ADD_OP: {
		if (!ctx->accum_allocated) return;
		int n = ctx->accum_width * ctx->accum_height * 4;
		for (int i = 0; i < n; i++)
			ctx->accum_buffer[i] += value;
		break;
	}
	case GL_RETURN_OP:
		gl_accum_write_framebuffer(ctx, value);
		break;
	default:
		GL_LOG("NativeGLAccum: unknown op 0x%04x", op);
		break;
	}
}

void NativeGLBegin(GLContext *ctx, uint32_t mode)
{
	ctx->in_begin = true;
	ctx->im_mode = mode;
	ctx->im_vertices.clear();
}

void NativeGLEnd(GLContext *ctx)
{
	ctx->in_begin = false;
	GLMetalFlushImmediateMode(ctx);
}

static inline void PushVertex(GLContext *ctx, float x, float y, float z, float w)
{
	GLVertex v;
	v.position[0] = x; v.position[1] = y; v.position[2] = z; v.position[3] = w;
	memcpy(v.color, ctx->current_color, sizeof(float) * 4);
	memcpy(v.normal, ctx->current_normal, sizeof(float) * 3);
	for (int u = 0; u < 4; u++)
		memcpy(v.texcoord[u], ctx->current_texcoord[u], sizeof(float) * 4);
	memcpy(v.secondary_color, ctx->current_secondary_color, sizeof(float) * 3);
	v.fog_coord = ctx->current_fog_coord;
	ctx->im_vertices.push_back(v);
}

void NativeGLVertex2f(GLContext *ctx, float x, float y) { PushVertex(ctx, x, y, 0.0f, 1.0f); }
void NativeGLVertex3f(GLContext *ctx, float x, float y, float z) { PushVertex(ctx, x, y, z, 1.0f); }
void NativeGLVertex4f(GLContext *ctx, float x, float y, float z, float w) { PushVertex(ctx, x, y, z, w); }
void NativeGLVertex2d(GLContext *ctx, double x, double y) { PushVertex(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLVertex3d(GLContext *ctx, double x, double y, double z) { PushVertex(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLVertex4d(GLContext *ctx, double x, double y, double z, double w) { PushVertex(ctx, (float)x, (float)y, (float)z, (float)w); }
void NativeGLVertex2i(GLContext *ctx, int32_t x, int32_t y) { PushVertex(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLVertex3i(GLContext *ctx, int32_t x, int32_t y, int32_t z) { PushVertex(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLVertex4i(GLContext *ctx, int32_t x, int32_t y, int32_t z, int32_t w) { PushVertex(ctx, (float)x, (float)y, (float)z, (float)w); }
void NativeGLVertex2s(GLContext *ctx, int16_t x, int16_t y) { PushVertex(ctx, (float)x, (float)y, 0.0f, 1.0f); }
void NativeGLVertex3s(GLContext *ctx, int16_t x, int16_t y, int16_t z) { PushVertex(ctx, (float)x, (float)y, (float)z, 1.0f); }
void NativeGLVertex4s(GLContext *ctx, int16_t x, int16_t y, int16_t z, int16_t w) { PushVertex(ctx, (float)x, (float)y, (float)z, (float)w); }

void NativeGLVertex2fv(GLContext *ctx, uint32_t mac_ptr) {
	float v[2];
	uint32_t tmp;
	tmp = ReadMacInt32(mac_ptr); memcpy(&v[0], &tmp, 4);
	tmp = ReadMacInt32(mac_ptr + 4); memcpy(&v[1], &tmp, 4);
	PushVertex(ctx, v[0], v[1], 0.0f, 1.0f);
}
void NativeGLVertex3fv(GLContext *ctx, uint32_t mac_ptr) {
	float v[3];
	for (int i = 0; i < 3; i++) { uint32_t tmp = ReadMacInt32(mac_ptr + i * 4); memcpy(&v[i], &tmp, 4); }
	PushVertex(ctx, v[0], v[1], v[2], 1.0f);
}
void NativeGLVertex4fv(GLContext *ctx, uint32_t mac_ptr) {
	float v[4];
	for (int i = 0; i < 4; i++) { uint32_t tmp = ReadMacInt32(mac_ptr + i * 4); memcpy(&v[i], &tmp, 4); }
	PushVertex(ctx, v[0], v[1], v[2], v[3]);
}
void NativeGLVertex2dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[2];
	for (int i = 0; i < 2; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i * 8) << 32) | ReadMacInt32(mac_ptr + i * 8 + 4);
		memcpy(&v[i], &b, 8);
	}
	PushVertex(ctx, (float)v[0], (float)v[1], 0.0f, 1.0f);
}
void NativeGLVertex3dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[3];
	for (int i = 0; i < 3; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i * 8) << 32) | ReadMacInt32(mac_ptr + i * 8 + 4);
		memcpy(&v[i], &b, 8);
	}
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], 1.0f);
}
void NativeGLVertex4dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[4];
	for (int i = 0; i < 4; i++) {
		uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i * 8) << 32) | ReadMacInt32(mac_ptr + i * 8 + 4);
		memcpy(&v[i], &b, 8);
	}
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}
void NativeGLVertex2iv(GLContext *ctx, uint32_t mac_ptr) {
	int32_t v[2];
	v[0] = (int32_t)ReadMacInt32(mac_ptr); v[1] = (int32_t)ReadMacInt32(mac_ptr + 4);
	PushVertex(ctx, (float)v[0], (float)v[1], 0.0f, 1.0f);
}
void NativeGLVertex3iv(GLContext *ctx, uint32_t mac_ptr) {
	int32_t v[3];
	for (int i = 0; i < 3; i++) v[i] = (int32_t)ReadMacInt32(mac_ptr + i * 4);
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], 1.0f);
}
void NativeGLVertex4iv(GLContext *ctx, uint32_t mac_ptr) {
	int32_t v[4];
	for (int i = 0; i < 4; i++) v[i] = (int32_t)ReadMacInt32(mac_ptr + i * 4);
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}
void NativeGLVertex2sv(GLContext *ctx, uint32_t mac_ptr) {
	int16_t x = (int16_t)ReadMacInt16(mac_ptr), y = (int16_t)ReadMacInt16(mac_ptr + 2);
	PushVertex(ctx, (float)x, (float)y, 0.0f, 1.0f);
}
void NativeGLVertex3sv(GLContext *ctx, uint32_t mac_ptr) {
	int16_t v[3];
	for (int i = 0; i < 3; i++) v[i] = (int16_t)ReadMacInt16(mac_ptr + i * 2);
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], 1.0f);
}
void NativeGLVertex4sv(GLContext *ctx, uint32_t mac_ptr) {
	int16_t v[4];
	for (int i = 0; i < 4; i++) v[i] = (int16_t)ReadMacInt16(mac_ptr + i * 2);
	PushVertex(ctx, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}

void NativeGLColor3f(GLContext *ctx, float r, float g, float b) {
	ctx->current_color[0] = r; ctx->current_color[1] = g;
	ctx->current_color[2] = b; ctx->current_color[3] = 1.0f;
}
void NativeGLColor4f(GLContext *ctx, float r, float g, float b, float a) {
	ctx->current_color[0] = r; ctx->current_color[1] = g;
	ctx->current_color[2] = b; ctx->current_color[3] = a;
}
void NativeGLColor3d(GLContext *ctx, double r, double g, double b) {
	NativeGLColor3f(ctx, (float)r, (float)g, (float)b);
}
void NativeGLColor4d(GLContext *ctx, double r, double g, double b, double a) {
	NativeGLColor4f(ctx, (float)r, (float)g, (float)b, (float)a);
}
void NativeGLColor3b(GLContext *ctx, int8_t r, int8_t g, int8_t b) {
	NativeGLColor3f(ctx, (r + 128) / 255.0f, (g + 128) / 255.0f, (b + 128) / 255.0f);
}
void NativeGLColor4b(GLContext *ctx, int8_t r, int8_t g, int8_t b, int8_t a) {
	NativeGLColor4f(ctx, (r + 128) / 255.0f, (g + 128) / 255.0f, (b + 128) / 255.0f, (a + 128) / 255.0f);
}
void NativeGLColor3ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b) {
	NativeGLColor3f(ctx, r / 255.0f, g / 255.0f, b / 255.0f);
}
void NativeGLColor4ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	NativeGLColor4f(ctx, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
void NativeGLColor3i(GLContext *ctx, int32_t r, int32_t g, int32_t b) {
	NativeGLColor3f(ctx, (float)((double)(r - INT32_MIN) / (double)UINT32_MAX),
	                     (float)((double)(g - INT32_MIN) / (double)UINT32_MAX),
	                     (float)((double)(b - INT32_MIN) / (double)UINT32_MAX));
}
void NativeGLColor4i(GLContext *ctx, int32_t r, int32_t g, int32_t b, int32_t a) {
	NativeGLColor4f(ctx, (float)((double)(r - INT32_MIN) / (double)UINT32_MAX),
	                     (float)((double)(g - INT32_MIN) / (double)UINT32_MAX),
	                     (float)((double)(b - INT32_MIN) / (double)UINT32_MAX),
	                     (float)((double)(a - INT32_MIN) / (double)UINT32_MAX));
}
void NativeGLColor3s(GLContext *ctx, int16_t r, int16_t g, int16_t b) {
	NativeGLColor3f(ctx, (r + 32768) / 65535.0f, (g + 32768) / 65535.0f, (b + 32768) / 65535.0f);
}
void NativeGLColor4s(GLContext *ctx, int16_t r, int16_t g, int16_t b, int16_t a) {
	NativeGLColor4f(ctx, (r + 32768) / 65535.0f, (g + 32768) / 65535.0f, (b + 32768) / 65535.0f, (a + 32768) / 65535.0f);
}
void NativeGLColor3ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b) {
	NativeGLColor3f(ctx, r / (float)UINT32_MAX, g / (float)UINT32_MAX, b / (float)UINT32_MAX);
}
void NativeGLColor4ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
	NativeGLColor4f(ctx, r / (float)UINT32_MAX, g / (float)UINT32_MAX, b / (float)UINT32_MAX, a / (float)UINT32_MAX);
}
void NativeGLColor3us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b) {
	NativeGLColor3f(ctx, r / 65535.0f, g / 65535.0f, b / 65535.0f);
}
void NativeGLColor4us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
	NativeGLColor4f(ctx, r / 65535.0f, g / 65535.0f, b / 65535.0f, a / 65535.0f);
}

static inline float ReadMacFloat(uint32_t addr) {
	uint32_t bits = ReadMacInt32(addr);
	float f; memcpy(&f, &bits, 4);
	return f;
}

void NativeGLColor3fv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4), ReadMacFloat(mac_ptr + 8));
}
void NativeGLColor4fv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4),
	                     ReadMacFloat(mac_ptr + 8), ReadMacFloat(mac_ptr + 12));
}
void NativeGLColor3bv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3b(ctx, (int8_t)ReadMacInt8(mac_ptr), (int8_t)ReadMacInt8(mac_ptr + 1), (int8_t)ReadMacInt8(mac_ptr + 2));
}
void NativeGLColor4bv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4b(ctx, (int8_t)ReadMacInt8(mac_ptr), (int8_t)ReadMacInt8(mac_ptr + 1),
	                     (int8_t)ReadMacInt8(mac_ptr + 2), (int8_t)ReadMacInt8(mac_ptr + 3));
}
void NativeGLColor3ubv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3ub(ctx, ReadMacInt8(mac_ptr), ReadMacInt8(mac_ptr + 1), ReadMacInt8(mac_ptr + 2));
}
void NativeGLColor4ubv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4ub(ctx, ReadMacInt8(mac_ptr), ReadMacInt8(mac_ptr + 1),
	                      ReadMacInt8(mac_ptr + 2), ReadMacInt8(mac_ptr + 3));
}
void NativeGLColor3dv(GLContext *ctx, uint32_t mac_ptr) {
	double r, g, b;
	uint64_t bits;
	bits = ((uint64_t)ReadMacInt32(mac_ptr) << 32) | ReadMacInt32(mac_ptr + 4); memcpy(&r, &bits, 8);
	bits = ((uint64_t)ReadMacInt32(mac_ptr + 8) << 32) | ReadMacInt32(mac_ptr + 12); memcpy(&g, &bits, 8);
	bits = ((uint64_t)ReadMacInt32(mac_ptr + 16) << 32) | ReadMacInt32(mac_ptr + 20); memcpy(&b, &bits, 8);
	NativeGLColor3f(ctx, (float)r, (float)g, (float)b);
}
void NativeGLColor4dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[4];
	for (int i = 0; i < 4; i++) {
		uint64_t bits = ((uint64_t)ReadMacInt32(mac_ptr + i * 8) << 32) | ReadMacInt32(mac_ptr + i * 8 + 4);
		memcpy(&v[i], &bits, 8);
	}
	NativeGLColor4f(ctx, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}
void NativeGLColor3iv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4), (int32_t)ReadMacInt32(mac_ptr + 8));
}
void NativeGLColor4iv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4),
	                     (int32_t)ReadMacInt32(mac_ptr + 8), (int32_t)ReadMacInt32(mac_ptr + 12));
}
void NativeGLColor3sv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2), (int16_t)ReadMacInt16(mac_ptr + 4));
}
void NativeGLColor4sv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2),
	                     (int16_t)ReadMacInt16(mac_ptr + 4), (int16_t)ReadMacInt16(mac_ptr + 6));
}
void NativeGLColor3uiv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3ui(ctx, ReadMacInt32(mac_ptr), ReadMacInt32(mac_ptr + 4), ReadMacInt32(mac_ptr + 8));
}
void NativeGLColor4uiv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4ui(ctx, ReadMacInt32(mac_ptr), ReadMacInt32(mac_ptr + 4),
	                      ReadMacInt32(mac_ptr + 8), ReadMacInt32(mac_ptr + 12));
}
void NativeGLColor3usv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor3us(ctx, ReadMacInt16(mac_ptr), ReadMacInt16(mac_ptr + 2), ReadMacInt16(mac_ptr + 4));
}
void NativeGLColor4usv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLColor4us(ctx, ReadMacInt16(mac_ptr), ReadMacInt16(mac_ptr + 2),
	                      ReadMacInt16(mac_ptr + 4), ReadMacInt16(mac_ptr + 6));
}

void NativeGLNormal3f(GLContext *ctx, float x, float y, float z) {
	ctx->current_normal[0] = x; ctx->current_normal[1] = y; ctx->current_normal[2] = z;
}
void NativeGLNormal3d(GLContext *ctx, double x, double y, double z) {
	NativeGLNormal3f(ctx, (float)x, (float)y, (float)z);
}
void NativeGLNormal3b(GLContext *ctx, int8_t x, int8_t y, int8_t z) {
	NativeGLNormal3f(ctx, x / 127.0f, y / 127.0f, z / 127.0f);
}
void NativeGLNormal3i(GLContext *ctx, int32_t x, int32_t y, int32_t z) {
	NativeGLNormal3f(ctx, (float)((double)x / (double)INT32_MAX),
	                      (float)((double)y / (double)INT32_MAX),
	                      (float)((double)z / (double)INT32_MAX));
}
void NativeGLNormal3s(GLContext *ctx, int16_t x, int16_t y, int16_t z) {
	NativeGLNormal3f(ctx, x / 32767.0f, y / 32767.0f, z / 32767.0f);
}
void NativeGLNormal3fv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLNormal3f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4), ReadMacFloat(mac_ptr + 8));
}
void NativeGLNormal3dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[3];
	for (int i = 0; i < 3; i++) {
		uint64_t bits = ((uint64_t)ReadMacInt32(mac_ptr + i * 8) << 32) | ReadMacInt32(mac_ptr + i * 8 + 4);
		memcpy(&v[i], &bits, 8);
	}
	NativeGLNormal3f(ctx, (float)v[0], (float)v[1], (float)v[2]);
}
void NativeGLNormal3bv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLNormal3b(ctx, (int8_t)ReadMacInt8(mac_ptr), (int8_t)ReadMacInt8(mac_ptr + 1), (int8_t)ReadMacInt8(mac_ptr + 2));
}
void NativeGLNormal3iv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLNormal3i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4), (int32_t)ReadMacInt32(mac_ptr + 8));
}
void NativeGLNormal3sv(GLContext *ctx, uint32_t mac_ptr) {
	NativeGLNormal3s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2), (int16_t)ReadMacInt16(mac_ptr + 4));
}

void NativeGLTexCoord1f(GLContext *ctx, float s) {
	int u = ctx->active_texture;
	ctx->current_texcoord[u][0] = s;
	ctx->current_texcoord[u][1] = 0.0f;
	ctx->current_texcoord[u][2] = 0.0f;
	ctx->current_texcoord[u][3] = 1.0f;
}
void NativeGLTexCoord2f(GLContext *ctx, float s, float t) {
	int u = ctx->active_texture;
	ctx->current_texcoord[u][0] = s;
	ctx->current_texcoord[u][1] = t;
	ctx->current_texcoord[u][2] = 0.0f;
	ctx->current_texcoord[u][3] = 1.0f;
}
void NativeGLTexCoord3f(GLContext *ctx, float s, float t, float r) {
	int u = ctx->active_texture;
	ctx->current_texcoord[u][0] = s;
	ctx->current_texcoord[u][1] = t;
	ctx->current_texcoord[u][2] = r;
	ctx->current_texcoord[u][3] = 1.0f;
}
void NativeGLTexCoord4f(GLContext *ctx, float s, float t, float r, float q) {
	int u = ctx->active_texture;
	ctx->current_texcoord[u][0] = s;
	ctx->current_texcoord[u][1] = t;
	ctx->current_texcoord[u][2] = r;
	ctx->current_texcoord[u][3] = q;
}
void NativeGLTexCoord1d(GLContext *ctx, double s) { NativeGLTexCoord1f(ctx, (float)s); }
void NativeGLTexCoord2d(GLContext *ctx, double s, double t) { NativeGLTexCoord2f(ctx, (float)s, (float)t); }
void NativeGLTexCoord3d(GLContext *ctx, double s, double t, double r) { NativeGLTexCoord3f(ctx, (float)s, (float)t, (float)r); }
void NativeGLTexCoord4d(GLContext *ctx, double s, double t, double r, double q) { NativeGLTexCoord4f(ctx, (float)s, (float)t, (float)r, (float)q); }
void NativeGLTexCoord1i(GLContext *ctx, int32_t s) { NativeGLTexCoord1f(ctx, (float)s); }
void NativeGLTexCoord2i(GLContext *ctx, int32_t s, int32_t t) { NativeGLTexCoord2f(ctx, (float)s, (float)t); }
void NativeGLTexCoord3i(GLContext *ctx, int32_t s, int32_t t, int32_t r) { NativeGLTexCoord3f(ctx, (float)s, (float)t, (float)r); }
void NativeGLTexCoord4i(GLContext *ctx, int32_t s, int32_t t, int32_t r, int32_t q) { NativeGLTexCoord4f(ctx, (float)s, (float)t, (float)r, (float)q); }
void NativeGLTexCoord1s(GLContext *ctx, int16_t s) { NativeGLTexCoord1f(ctx, (float)s); }
void NativeGLTexCoord2s(GLContext *ctx, int16_t s, int16_t t) { NativeGLTexCoord2f(ctx, (float)s, (float)t); }
void NativeGLTexCoord3s(GLContext *ctx, int16_t s, int16_t t, int16_t r) { NativeGLTexCoord3f(ctx, (float)s, (float)t, (float)r); }
void NativeGLTexCoord4s(GLContext *ctx, int16_t s, int16_t t, int16_t r, int16_t q) { NativeGLTexCoord4f(ctx, (float)s, (float)t, (float)r, (float)q); }

void NativeGLTexCoord1fv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord1f(ctx, ReadMacFloat(mac_ptr)); }
void NativeGLTexCoord2fv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord2f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4)); }
void NativeGLTexCoord3fv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord3f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4), ReadMacFloat(mac_ptr + 8)); }
void NativeGLTexCoord4fv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord4f(ctx, ReadMacFloat(mac_ptr), ReadMacFloat(mac_ptr + 4), ReadMacFloat(mac_ptr + 8), ReadMacFloat(mac_ptr + 12)); }
void NativeGLTexCoord1dv(GLContext *ctx, uint32_t mac_ptr) {
	double v; uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr) << 32) | ReadMacInt32(mac_ptr + 4); memcpy(&v, &b, 8);
	NativeGLTexCoord1f(ctx, (float)v);
}
void NativeGLTexCoord2dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[2];
	for (int i = 0; i < 2; i++) { uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i*8) << 32) | ReadMacInt32(mac_ptr + i*8 + 4); memcpy(&v[i], &b, 8); }
	NativeGLTexCoord2f(ctx, (float)v[0], (float)v[1]);
}
void NativeGLTexCoord3dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[3];
	for (int i = 0; i < 3; i++) { uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i*8) << 32) | ReadMacInt32(mac_ptr + i*8 + 4); memcpy(&v[i], &b, 8); }
	NativeGLTexCoord3f(ctx, (float)v[0], (float)v[1], (float)v[2]);
}
void NativeGLTexCoord4dv(GLContext *ctx, uint32_t mac_ptr) {
	double v[4];
	for (int i = 0; i < 4; i++) { uint64_t b = ((uint64_t)ReadMacInt32(mac_ptr + i*8) << 32) | ReadMacInt32(mac_ptr + i*8 + 4); memcpy(&v[i], &b, 8); }
	NativeGLTexCoord4f(ctx, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
}
void NativeGLTexCoord1iv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord1i(ctx, (int32_t)ReadMacInt32(mac_ptr)); }
void NativeGLTexCoord2iv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord2i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4)); }
void NativeGLTexCoord3iv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord3i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4), (int32_t)ReadMacInt32(mac_ptr + 8)); }
void NativeGLTexCoord4iv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord4i(ctx, (int32_t)ReadMacInt32(mac_ptr), (int32_t)ReadMacInt32(mac_ptr + 4), (int32_t)ReadMacInt32(mac_ptr + 8), (int32_t)ReadMacInt32(mac_ptr + 12)); }
void NativeGLTexCoord1sv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord1s(ctx, (int16_t)ReadMacInt16(mac_ptr)); }
void NativeGLTexCoord2sv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord2s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2)); }
void NativeGLTexCoord3sv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord3s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2), (int16_t)ReadMacInt16(mac_ptr + 4)); }
void NativeGLTexCoord4sv(GLContext *ctx, uint32_t mac_ptr) { NativeGLTexCoord4s(ctx, (int16_t)ReadMacInt16(mac_ptr), (int16_t)ReadMacInt16(mac_ptr + 2), (int16_t)ReadMacInt16(mac_ptr + 4), (int16_t)ReadMacInt16(mac_ptr + 6)); }

static inline float ReadArrayComponent(uint32_t addr, uint32_t type) {
	switch (type) {
	case GL_FLOAT: {
		uint32_t bits = ReadMacInt32(addr);
		float f; memcpy(&f, &bits, 4);
		return f;
	}
	case GL_DOUBLE: {
		uint64_t bits = ((uint64_t)ReadMacInt32(addr) << 32) | ReadMacInt32(addr + 4);
		double d; memcpy(&d, &bits, 8);
		return (float)d;
	}
	case GL_INT:
		return (float)(int32_t)ReadMacInt32(addr);
	case GL_UNSIGNED_INT:
		return (float)ReadMacInt32(addr);
	case GL_SHORT:
		return (float)(int16_t)ReadMacInt16(addr);
	case GL_UNSIGNED_SHORT:
		return (float)ReadMacInt16(addr);
	case GL_BYTE:
		return (float)(int8_t)ReadMacInt8(addr);
	case GL_UNSIGNED_BYTE:
		return (float)ReadMacInt8(addr);
	default:
		return 0.0f;
	}
}

static inline int TypeSize(uint32_t type) {
	switch (type) {
	case GL_FLOAT:          return 4;
	case GL_DOUBLE:         return 8;
	case GL_INT:            return 4;
	case GL_UNSIGNED_INT:   return 4;
	case GL_SHORT:          return 2;
	case GL_UNSIGNED_SHORT: return 2;
	case GL_BYTE:           return 1;
	case GL_UNSIGNED_BYTE:  return 1;
	default:                return 4;
	}
}

static inline int EffectiveStride(const GLVertexArrayPointer &arr) {
	if (arr.stride > 0) return arr.stride;
	return arr.size * TypeSize(arr.type);
}

static void FetchArrayVertex(GLContext *ctx, int32_t i, GLVertex &v)
{
	if (ctx->vertex_array.enabled && ctx->vertex_array.pointer) {
		int es = EffectiveStride(ctx->vertex_array);
		uint32_t base = ctx->vertex_array.pointer + i * es;
		int sz = ctx->vertex_array.size;
		v.position[0] = (sz >= 1) ? ReadArrayComponent(base + 0 * TypeSize(ctx->vertex_array.type), ctx->vertex_array.type) : 0.0f;
		v.position[1] = (sz >= 2) ? ReadArrayComponent(base + 1 * TypeSize(ctx->vertex_array.type), ctx->vertex_array.type) : 0.0f;
		v.position[2] = (sz >= 3) ? ReadArrayComponent(base + 2 * TypeSize(ctx->vertex_array.type), ctx->vertex_array.type) : 0.0f;
		v.position[3] = (sz >= 4) ? ReadArrayComponent(base + 3 * TypeSize(ctx->vertex_array.type), ctx->vertex_array.type) : 1.0f;
	} else {
		v.position[0] = v.position[1] = v.position[2] = 0.0f; v.position[3] = 1.0f;
	}

	if (ctx->color_array.enabled && ctx->color_array.pointer) {
		int es = EffectiveStride(ctx->color_array);
		uint32_t base = ctx->color_array.pointer + i * es;
		int sz = ctx->color_array.size;
		uint32_t ct = ctx->color_array.type;
		if (ct == GL_UNSIGNED_BYTE) {
			v.color[0] = (sz >= 1) ? ReadMacInt8(base) / 255.0f : 0.0f;
			v.color[1] = (sz >= 2) ? ReadMacInt8(base + 1) / 255.0f : 0.0f;
			v.color[2] = (sz >= 3) ? ReadMacInt8(base + 2) / 255.0f : 0.0f;
			v.color[3] = (sz >= 4) ? ReadMacInt8(base + 3) / 255.0f : 1.0f;
		} else {
			v.color[0] = (sz >= 1) ? ReadArrayComponent(base + 0 * TypeSize(ct), ct) : 0.0f;
			v.color[1] = (sz >= 2) ? ReadArrayComponent(base + 1 * TypeSize(ct), ct) : 0.0f;
			v.color[2] = (sz >= 3) ? ReadArrayComponent(base + 2 * TypeSize(ct), ct) : 0.0f;
			v.color[3] = (sz >= 4) ? ReadArrayComponent(base + 3 * TypeSize(ct), ct) : 1.0f;
		}
	} else {
		memcpy(v.color, ctx->current_color, sizeof(float) * 4);
	}

	if (ctx->normal_array.enabled && ctx->normal_array.pointer) {
		int es = EffectiveStride(ctx->normal_array);
		uint32_t base = ctx->normal_array.pointer + i * es;
		for (int c = 0; c < 3; c++)
			v.normal[c] = ReadArrayComponent(base + c * TypeSize(ctx->normal_array.type), ctx->normal_array.type);
	} else {
		memcpy(v.normal, ctx->current_normal, sizeof(float) * 3);
	}

	if (ctx->texcoord_array[0].enabled && ctx->texcoord_array[0].pointer) {
		int es = EffectiveStride(ctx->texcoord_array[0]);
		uint32_t base = ctx->texcoord_array[0].pointer + i * es;
		int sz = ctx->texcoord_array[0].size;
		uint32_t ct = ctx->texcoord_array[0].type;
		v.texcoord[0][0] = (sz >= 1) ? ReadArrayComponent(base + 0 * TypeSize(ct), ct) : 0.0f;
		v.texcoord[0][1] = (sz >= 2) ? ReadArrayComponent(base + 1 * TypeSize(ct), ct) : 0.0f;
		v.texcoord[0][2] = (sz >= 3) ? ReadArrayComponent(base + 2 * TypeSize(ct), ct) : 0.0f;
		v.texcoord[0][3] = (sz >= 4) ? ReadArrayComponent(base + 3 * TypeSize(ct), ct) : 1.0f;
	} else {
		memcpy(v.texcoord[0], ctx->current_texcoord[0], sizeof(float) * 4);
	}

	for (int u = 1; u < 4; u++)
		memcpy(v.texcoord[u], ctx->current_texcoord[u], sizeof(float) * 4);

	memcpy(v.secondary_color, ctx->current_secondary_color, sizeof(float) * 3);
	v.fog_coord = ctx->current_fog_coord;
}

static void GLMetalDrawVertexArray(GLContext *ctx, uint32_t mode, const int32_t *indices, int32_t count)
{
	if (count <= 0) return;
	GLGLESState *gs = (GLGLESState *)ctx->metal;
	if (!gs || !gs->initialized) return;

	ctx->in_begin = true;
	ctx->im_mode = mode;
	ctx->im_vertices.clear();
	ctx->im_vertices.reserve((size_t)count);

	for (int32_t j = 0; j < count; j++) {
		GLVertex v;
		FetchArrayVertex(ctx, indices[j], v);
		ctx->im_vertices.push_back(v);
	}

	ctx->in_begin = false;
	GLMetalFlushImmediateMode(ctx);
}

void NativeGLDrawArrays(GLContext *ctx, uint32_t mode, int32_t first, int32_t count)
{
	if (count <= 0) return;
	std::vector<int32_t> indices((size_t)count);
	for (int32_t i = 0; i < count; i++) indices[i] = first + i;
	GLMetalDrawVertexArray(ctx, mode, indices.data(), count);
}

void NativeGLDrawElements(GLContext *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t indices_ptr)
{
	if (count <= 0) return;
	std::vector<int32_t> indices((size_t)count);
	for (int32_t i = 0; i < count; i++) {
		switch (type) {
		case GL_UNSIGNED_INT:
			indices[i] = (int32_t)ReadMacInt32(indices_ptr + i * 4);
			break;
		case GL_UNSIGNED_SHORT:
			indices[i] = (int32_t)ReadMacInt16(indices_ptr + i * 2);
			break;
		case GL_UNSIGNED_BYTE:
			indices[i] = (int32_t)ReadMacInt8(indices_ptr + i);
			break;
		default:
			indices[i] = i;
			break;
		}
	}
	GLMetalDrawVertexArray(ctx, mode, indices.data(), count);
}

void NativeGLDrawRangeElements(GLContext *ctx, uint32_t mode, uint32_t start, uint32_t end,
                             int32_t count, uint32_t type, uint32_t indices_ptr)
{
	(void)start; (void)end;
	NativeGLDrawElements(ctx, mode, count, type, indices_ptr);
}

void NativeGLArrayElement(GLContext *ctx, int32_t i)
{
	GLVertex v;
	FetchArrayVertex(ctx, i, v);
	ctx->im_vertices.push_back(v);
}

void NativeGLInterleavedArrays(GLContext *ctx, uint32_t format, int32_t stride, uint32_t pointer)
{
	ctx->color_array.enabled = false;
	ctx->normal_array.enabled = false;
	ctx->texcoord_array[ctx->client_active_texture].enabled = false;
	ctx->edge_flag_array.enabled = false;
	ctx->index_array.enabled = false;

	int totalStride = 0;

	switch (format) {
	case GL_V2F:
		totalStride = stride ? stride : 8;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 2;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer;
		break;
	case GL_V3F:
		totalStride = stride ? stride : 12;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer;
		break;
	case GL_C4UB_V2F:
		totalStride = stride ? stride : 12;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_UNSIGNED_BYTE; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 2;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 4;
		break;
	case GL_C4UB_V3F:
		totalStride = stride ? stride : 16;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_UNSIGNED_BYTE; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 4;
		break;
	case GL_C3F_V3F:
		totalStride = stride ? stride : 24;
		ctx->color_array.enabled = true; ctx->color_array.size = 3;
		ctx->color_array.type = GL_FLOAT; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 12;
		break;
	case GL_N3F_V3F:
		totalStride = stride ? stride : 24;
		ctx->normal_array.enabled = true; ctx->normal_array.size = 3;
		ctx->normal_array.type = GL_FLOAT; ctx->normal_array.stride = totalStride;
		ctx->normal_array.pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 12;
		break;
	case GL_C4F_N3F_V3F:
		totalStride = stride ? stride : 40;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_FLOAT; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer;
		ctx->normal_array.enabled = true; ctx->normal_array.size = 3;
		ctx->normal_array.type = GL_FLOAT; ctx->normal_array.stride = totalStride;
		ctx->normal_array.pointer = pointer + 16;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 28;
		break;
	case GL_T2F_V3F:
		totalStride = stride ? stride : 20;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 2;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 8;
		break;
	case GL_T4F_V4F:
		totalStride = stride ? stride : 32;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 4;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 4;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 16;
		break;
	case GL_T2F_C4UB_V3F:
		totalStride = stride ? stride : 24;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 2;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_UNSIGNED_BYTE; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer + 8;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 12;
		break;
	case GL_T2F_C3F_V3F:
		totalStride = stride ? stride : 32;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 2;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->color_array.enabled = true; ctx->color_array.size = 3;
		ctx->color_array.type = GL_FLOAT; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer + 8;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 20;
		break;
	case GL_T2F_N3F_V3F:
		totalStride = stride ? stride : 32;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 2;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->normal_array.enabled = true; ctx->normal_array.size = 3;
		ctx->normal_array.type = GL_FLOAT; ctx->normal_array.stride = totalStride;
		ctx->normal_array.pointer = pointer + 8;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 20;
		break;
	case GL_T2F_C4F_N3F_V3F:
		totalStride = stride ? stride : 48;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 2;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_FLOAT; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer + 8;
		ctx->normal_array.enabled = true; ctx->normal_array.size = 3;
		ctx->normal_array.type = GL_FLOAT; ctx->normal_array.stride = totalStride;
		ctx->normal_array.pointer = pointer + 24;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 3;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 36;
		break;
	case GL_T4F_C4F_N3F_V4F:
		totalStride = stride ? stride : 60;
		ctx->texcoord_array[ctx->client_active_texture].enabled = true;
		ctx->texcoord_array[ctx->client_active_texture].size = 4;
		ctx->texcoord_array[ctx->client_active_texture].type = GL_FLOAT;
		ctx->texcoord_array[ctx->client_active_texture].stride = totalStride;
		ctx->texcoord_array[ctx->client_active_texture].pointer = pointer;
		ctx->color_array.enabled = true; ctx->color_array.size = 4;
		ctx->color_array.type = GL_FLOAT; ctx->color_array.stride = totalStride;
		ctx->color_array.pointer = pointer + 16;
		ctx->normal_array.enabled = true; ctx->normal_array.size = 3;
		ctx->normal_array.type = GL_FLOAT; ctx->normal_array.stride = totalStride;
		ctx->normal_array.pointer = pointer + 32;
		ctx->vertex_array.enabled = true; ctx->vertex_array.size = 4;
		ctx->vertex_array.type = GL_FLOAT; ctx->vertex_array.stride = totalStride;
		ctx->vertex_array.pointer = pointer + 44;
		break;
	default:
		GL_LOG("glInterleavedArrays: unknown format 0x%04x", format);
		break;
	}
	(void)totalStride;
}
