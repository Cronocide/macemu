/*
 *  rave_gles_renderer.cpp - RAVE rendering backend for OpenGL ES 2.0
 *
 *  Ported from PocketShaver rave_metal_renderer.mm
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *  GLES 2.0 translation for AArch64/H700 with Mali-G31 (via gl4es)
 *
 *  Replaces all Metal API calls with GLES 2.0 equivalents:
 *    MTLDevice/CommandQueue      -> implicit (GLES has no explicit device)
 *    MTLRenderPipelineState      -> GLuint shader programs (16 variants)
 *    MTLRenderCommandEncoder     -> glUseProgram + glDraw*
 *    MTLBuffer                   -> glVertexAttribPointer (client-side arrays)
 *    MTLTexture                  -> glGenTextures / glTexImage2D
 *    MTLDepthStencilState        -> glDepthFunc / glDepthMask / glEnable
 *    MTLSampler                  -> glTexParameter on each texture
 *    FBO render target           -> compositor (Phase 4, stubbed here)
 */

#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "rave_engine.h"
#include "rave_metal_renderer.h"
#include "gles_compositor.h"
#include "gles_compositor.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "rave_shaders_gles.h"

extern uint32 Mac_sysalloc(uint32 size);

#include "video.h"
#include "prefs.h"

// RAVE per-frame timing instrumentation, gated by the "perf_profile" pref.
// Separates guest-PPC geometry time from RAVE renderer/GPU time per frame, and
// splits NativeRenderEnd into flush / glReadPixels / CPU-blit phases.
bool          rave_perf = false;					// shared with rave_dispatch.cpp
int64         g_rave_dispatch_us = 0;				// time inside RaveDispatch this frame
static bool   rave_perf_init = false;
static uint64 rave_perf_flush_us = 0;
static uint64 rave_perf_readback_us = 0;
static uint64 rave_perf_blit_us = 0;
static uint64 rave_perf_dispatch_us = 0;			// accumulated draw/setup dispatch
static uint64 rave_perf_guest_us = 0;				// accumulated guest geometry time
static uint64 rave_perf_frames = 0;
static uint64 rave_perf_window_start = 0;

// Texture-draw diagnostics ("perf_profile"): for textured draws, capture the
// vertex-color and UV ranges plus the texture-op and bound-texture state, so we
// can tell white-vs-black models apart (untextured variant / zero vertex color /
// empty texture / texture-op misread).  Set per-draw in ApplyDirtyState and
// accumulated in SubmitRawVertices; printed ~every 2s.
static uint64 rtex_window_start = 0;
static uint32 rtex_textured_draws = 0;
static uint32 rtex_untextured_draws = 0;
static float  rtex_colMin[4] = {1e9f,1e9f,1e9f,1e9f};
static float  rtex_colMax[4] = {-1e9f,-1e9f,-1e9f,-1e9f};
static float  rtex_ucolMin[4] = {1e9f,1e9f,1e9f,1e9f};	// untextured draw colors
static float  rtex_ucolMax[4] = {-1e9f,-1e9f,-1e9f,-1e9f};
static float  rtex_uvMin[2] = {1e9f,1e9f};
static float  rtex_uvMax[2] = {-1e9f,-1e9f};
static uint32 rtex_nonmodulate_draws = 0;				// textured draws with texOp != 1
static int    rtex_cur_texop_i = 0;		// raw state[12].i for the current draw
static float  rtex_cur_texop_f = 0.0f;	// raw state[12].f for the current draw
static int    rtex_cur_texstate = 0;	// 0=no addr, 1=addr-but-empty, 2=bound-with-data
static uint32 rtex_texstate_count[3] = {0,0,0};

static void rtex_report(void)
{
	if (!rave_perf) return;
	const uint64 now = GetTicks_usec();
	if (rtex_window_start == 0) { rtex_window_start = now; return; }
	if (now - rtex_window_start < 2000000) return;
	printf("[perf:tex] textured %u (nonmod %u) untextured %u | texstate noaddr=%u empty=%u ok=%u | "
	       "texColor rgb[%.2f,%.2f][%.2f,%.2f][%.2f,%.2f] | "
	       "untexColor rgb[%.2f,%.2f][%.2f,%.2f][%.2f,%.2f] a[%.2f,%.2f] | "
	       "uv u[%.2f,%.2f] v[%.2f,%.2f]\n",
	       rtex_textured_draws, rtex_nonmodulate_draws, rtex_untextured_draws,
	       rtex_texstate_count[0], rtex_texstate_count[1], rtex_texstate_count[2],
	       rtex_colMin[0], rtex_colMax[0], rtex_colMin[1], rtex_colMax[1],
	       rtex_colMin[2], rtex_colMax[2],
	       rtex_ucolMin[0], rtex_ucolMax[0], rtex_ucolMin[1], rtex_ucolMax[1],
	       rtex_ucolMin[2], rtex_ucolMax[2], rtex_ucolMin[3], rtex_ucolMax[3],
	       rtex_uvMin[0], rtex_uvMax[0], rtex_uvMin[1], rtex_uvMax[1]);
	fflush(stdout);
	rtex_textured_draws = rtex_untextured_draws = rtex_nonmodulate_draws = 0;
	rtex_texstate_count[0] = rtex_texstate_count[1] = rtex_texstate_count[2] = 0;
	for (int i = 0; i < 4; i++) { rtex_colMin[i] = 1e9f; rtex_colMax[i] = -1e9f; }
	for (int i = 0; i < 4; i++) { rtex_ucolMin[i] = 1e9f; rtex_ucolMax[i] = -1e9f; }
	for (int i = 0; i < 2; i++) { rtex_uvMin[i] = 1e9f; rtex_uvMax[i] = -1e9f; }
	rtex_window_start = now;
}

static void rave_perf_report(void)
{
	if (!rave_perf) return;
	const uint64 now = GetTicks_usec();
	if (rave_perf_window_start == 0) { rave_perf_window_start = now; return; }
	const uint64 elapsed = now - rave_perf_window_start;
	if (elapsed < 2000000) return;
	const double secs = elapsed / 1000000.0;
	const double f = rave_perf_frames ? (double)rave_perf_frames : 1.0;
	printf("[perf:rave] %.2fs | %llu frames (%.1f fps) | per-frame avg ms: "
	       "guest %.1f | rave-draws %.1f | flush %.2f readback %.2f blit %.2f\n",
	       secs, (unsigned long long)rave_perf_frames, rave_perf_frames / secs,
	       rave_perf_guest_us / 1000.0 / f,
	       rave_perf_dispatch_us / 1000.0 / f,
	       rave_perf_flush_us / 1000.0 / f,
	       rave_perf_readback_us / 1000.0 / f,
	       rave_perf_blit_us / 1000.0 / f);
	fflush(stdout);
	rave_perf_flush_us = rave_perf_readback_us = rave_perf_blit_us = 0;
	rave_perf_dispatch_us = rave_perf_guest_us = 0;
	rave_perf_frames = 0;
	rave_perf_window_start = now;
}

// #region agent log
#include <sys/time.h>
#define RAVE_DBG_LOG "/tmp/rave_debug_f04964.ndjson"
static void rave_dbg(const char *hyp, const char *loc, const char *msg,
                     const char *extra = nullptr) {
	static int enabled = -1;
	if (enabled < 0) {
		const char *env = getenv("RAVE_DEBUG_NDJSON");
		enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
	}
	if (!enabled) return;
	FILE *f = fopen(RAVE_DBG_LOG, "a");
	if (!f) return;
	struct timeval tv; gettimeofday(&tv, nullptr);
	long long ts = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
	if (extra)
		fprintf(f, "{\"sessionId\":\"f04964\",\"hypothesisId\":\"%s\",\"location\":\"%s\","
		           "\"message\":\"%s\",\"data\":{%s},\"timestamp\":%lld}\n",
		        hyp, loc, msg, extra, ts);
	else
		fprintf(f, "{\"sessionId\":\"f04964\",\"hypothesisId\":\"%s\",\"location\":\"%s\","
		           "\"message\":\"%s\",\"data\":{},\"timestamp\":%lld}\n",
		        hyp, loc, msg, ts);
	fclose(f);
}
// #endregion

// GL state save/restore (shared by RAVE render pass and GPU compositor)
struct SavedGLState {
	GLint   fbo;
	GLint   viewport[4];
	GLboolean depthTest;
	GLboolean blend;
	GLboolean scissorTest;
	GLboolean depthMask;
	GLboolean colorMask[4];
	GLint   blendSrcRGB, blendDstRGB;
	GLint   blendSrcAlpha, blendDstAlpha;
	GLint   blendEqRGB, blendEqAlpha;
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
	glGetBooleanv(GL_DEPTH_WRITEMASK, &st->depthMask);
	glGetBooleanv(GL_COLOR_WRITEMASK, st->colorMask);
	glGetIntegerv(GL_BLEND_SRC_RGB, &st->blendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &st->blendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &st->blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &st->blendDstAlpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &st->blendEqRGB);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &st->blendEqAlpha);
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
	glDepthMask(st->depthMask);
	glColorMask(st->colorMask[0], st->colorMask[1], st->colorMask[2], st->colorMask[3]);
	glBlendEquationSeparate(st->blendEqRGB, st->blendEqAlpha);
	glBlendFuncSeparate(st->blendSrcRGB, st->blendDstRGB,
	                    st->blendSrcAlpha, st->blendDstAlpha);
	glUseProgram(st->activeProgram);
	glBindVertexArray(st->boundVAO);
	glBindBuffer(GL_ARRAY_BUFFER, st->boundVBO);
}

// (GPU compositor removed — using the_buffer approach for QuickDraw z-order integration)

#define kQANoErr  0
#define kQAError -2
#define kQAContext_NoZBuffer (1 << 0)

// Vertex layout (48 bytes, must match Metal RaveVertex and shader attributes)
struct RaveVertex {
	float pos[4];    // x, y, z, w
	float color[4];  // r, g, b, a
	float uv[4];     // uOverW, vOverW, invW, 0
};

// Fragment uniforms (mirrors Metal FragmentUniforms)
struct FragmentUniforms {
	int32_t  texture_op;
	int32_t  fog_mode;
	float    fog_color_a, fog_color_r, fog_color_g, fog_color_b;
	float    fog_start, fog_end, fog_density, fog_max_depth;
	int32_t  alpha_test_func;
	float    alpha_test_ref;
	uint32_t multi_texture_op;
	float    multi_texture_factor;
	float    mipmap_bias;
	float    multi_texture_mipmap_bias;
	float    env_color_r, env_color_g, env_color_b, env_color_a;
};


/*
 *  Shader program variant management
 *
 *  16 variants from 4 boolean #defines x 3 blend modes = 48 programs.
 *  Simplification for GLES: we use 16 programs and switch blend state
 *  via glBlendFunc (cheaper than 48 separate programs on Mali-G31).
 */

struct RaveShaderProgram {
	GLuint program;
	// Uniform locations (cached at link time)
	GLint loc_viewport;
	GLint loc_point_width;
	GLint loc_texture_op;
	GLint loc_texture0;
	GLint loc_texture1;
	GLint loc_fog_mode;
	GLint loc_fog_color;
	GLint loc_fog_start;
	GLint loc_fog_end;
	GLint loc_fog_density;
	GLint loc_fog_max_depth;
	GLint loc_alpha_test_func;
	GLint loc_alpha_test_ref;
	GLint loc_multi_texture_op;
	GLint loc_multi_texture_factor;
	GLint loc_env_color;
};

#define RAVE_NUM_SHADER_VARIANTS 16

struct RaveGLESState {
	RaveShaderProgram programs[RAVE_NUM_SHADER_VARIANTS];
	int               currentProgramIdx;
	bool              renderPassActive;

	GLuint            fbo;
	GLuint            colorTexture;
	GLuint            depthRenderbuffer;
	int               fboWidth, fboHeight;

	GLuint            vao;
	GLuint            vbo;

	uint32_t          drawCallCount;
	uint32_t          triangleCount;

	// Per-frame vertex coordinate diagnostics
	float             vtxMinX, vtxMaxX;
	float             vtxMinY, vtxMaxY;
	float             vtxMinZ, vtxMaxZ;
	float             vtxMinW, vtxMaxW; // invW from uv[2]
	float             vtxMinA, vtxMaxA; // vertex alpha
	float             vtxMinR, vtxMaxR; // vertex color red
	uint32_t          vtxOutsideCount;  // verts outside [0,w]x[0,h]
	uint32_t          vtxDiagFrames;    // frames logged so far

	// Buffer access staging
	uint8_t          *drawBufferCPU;
	uint32_t          drawBufferCPUMac;
	uint32_t          drawBufferCPUSize;
	uint8_t          *readbackCPU;
	uint32_t          readbackCPUSize;
	uint8_t          *zBufferCPU;
	uint32_t          zBufferCPUMac;
	uint32_t          zBufferCPUSize;
	bool              drawBufferAccessed;
	bool              zBufferAccessed;

	std::vector<uint32_t> rttTextureHandles;
};


static int accel_overlay_refcount = 0;
static bool pending_overlay_destroy_scheduled = false;

/*
 *  Shader compilation helpers
 */

static GLuint CompileShader(GLenum type, const char *defines, const char *source)
{
	GLuint shader = glCreateShader(type);
	const char *version = "#version 300 es\n";
	const char *sources[3] = { version, defines, source };
	glShaderSource(shader, 3, sources, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		RAVE_LOG("Shader compile error: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint LinkProgram(GLuint vert, GLuint frag)
{
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);

	glBindAttribLocation(prog, 0, "a_position");
	glBindAttribLocation(prog, 1, "a_color");
	glBindAttribLocation(prog, 2, "a_texcoord");
	glBindAttribLocation(prog, 3, "a_texcoord2");

	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		RAVE_LOG("Program link error: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static void BuildShaderVariant(RaveShaderProgram *out, int bits)
{
	char defines[256];
	int len = 0;
	if (bits & 1) len += snprintf(defines + len, sizeof(defines) - len, "#define HAS_TEXTURE\n");
	if (bits & 2) len += snprintf(defines + len, sizeof(defines) - len, "#define HAS_FOG\n");
	if (bits & 4) len += snprintf(defines + len, sizeof(defines) - len, "#define HAS_ALPHA_TEST\n");
	if (bits & 8) len += snprintf(defines + len, sizeof(defines) - len, "#define HAS_MULTI_TEXTURE\n");
	if (len == 0) defines[0] = '\0';

	// #region agent log
	{
		GLenum pre_err = glGetError();
		char buf[128];
		snprintf(buf, sizeof(buf), "\"bits\":%d,\"glGetError\":%u", bits, (unsigned)pre_err);
		rave_dbg("C", "rave_gles_renderer.cpp:BuildShaderVariant", "pre-compile GL state", buf);
	}
	// #endregion

	GLuint vs = CompileShader(GL_VERTEX_SHADER, defines, rave_vertex_src);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, defines, rave_fragment_src);
	if (!vs || !fs) {
		// #region agent log
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "\"bits\":%d,\"vs\":%u,\"fs\":%u", bits, vs, fs);
			rave_dbg("C", "rave_gles_renderer.cpp:BuildShaderVariant", "shader compile FAILED", buf);
		}
		// #endregion
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		out->program = 0;
		return;
	}

	out->program = LinkProgram(vs, fs);
	glDeleteShader(vs);
	glDeleteShader(fs);

	// #region agent log
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "\"bits\":%d,\"program\":%u", bits, out->program);
		rave_dbg("C", "rave_gles_renderer.cpp:BuildShaderVariant", "shader build result", buf);
	}
	// #endregion

	if (!out->program) return;

	out->loc_viewport          = glGetUniformLocation(out->program, "u_viewport");
	out->loc_point_width       = glGetUniformLocation(out->program, "u_point_width");
	out->loc_texture_op        = glGetUniformLocation(out->program, "u_texture_op");
	out->loc_texture0          = glGetUniformLocation(out->program, "u_texture0");
	out->loc_texture1          = glGetUniformLocation(out->program, "u_texture1");
	out->loc_fog_mode          = glGetUniformLocation(out->program, "u_fog_mode");
	out->loc_fog_color         = glGetUniformLocation(out->program, "u_fog_color");
	out->loc_fog_start         = glGetUniformLocation(out->program, "u_fog_start");
	out->loc_fog_end           = glGetUniformLocation(out->program, "u_fog_end");
	out->loc_fog_density       = glGetUniformLocation(out->program, "u_fog_density");
	out->loc_fog_max_depth     = glGetUniformLocation(out->program, "u_fog_max_depth");
	out->loc_alpha_test_func   = glGetUniformLocation(out->program, "u_alpha_test_func");
	out->loc_alpha_test_ref    = glGetUniformLocation(out->program, "u_alpha_test_ref");
	out->loc_multi_texture_op  = glGetUniformLocation(out->program, "u_multi_texture_op");
	out->loc_multi_texture_factor = glGetUniformLocation(out->program, "u_multi_texture_factor");
	out->loc_env_color         = glGetUniformLocation(out->program, "u_env_color");
}


/*
 *  GLES depth function mapping (RAVE ZFunction -> GL)
 */
static GLenum ZFunctionToGL(int zfunc)
{
	switch (zfunc) {
		case 0: return GL_ALWAYS;
		case 1: return GL_LESS;
		case 2: return GL_EQUAL;
		case 3: return GL_LEQUAL;
		case 4: return GL_GREATER;
		case 5: return GL_NOTEQUAL;
		case 6: return GL_GEQUAL;
		case 7: return GL_ALWAYS;
		case 8: return GL_NEVER;
		default: return GL_ALWAYS;
	}
}


/*
 *  GL blend factor mapping (OpenGL enum -> GLES enum, same values)
 */
static GLenum GLBlendToGLES(uint32_t glFactor)
{
	switch (glFactor) {
		case 0x0000: return GL_ZERO;
		case 0x0001: return GL_ONE;
		case 0x0300: return GL_SRC_COLOR;
		case 0x0301: return GL_ONE_MINUS_SRC_COLOR;
		case 0x0302: return GL_SRC_ALPHA;
		case 0x0303: return GL_ONE_MINUS_SRC_ALPHA;
		case 0x0304: return GL_DST_ALPHA;
		case 0x0305: return GL_ONE_MINUS_DST_ALPHA;
		case 0x0306: return GL_DST_COLOR;
		case 0x0307: return GL_ONE_MINUS_DST_COLOR;
		case 0x0308: return GL_SRC_ALPHA_SATURATE;
		default:     return GL_ONE;
	}
}


/*
 *  Overlay lifecycle (using unified gles_compositor)
 */

void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width, int32_t height)
{
	RAVE_LOG("RaveCreateMetalOverlay(GLES): %dx%d at (%d,%d)", width, height, left, top);
	video_set_rave_display(1, left, top, width, height);
}

void RaveDestroyMetalOverlay(void)
{
	gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_RAVE);
	RAVE_LOG("RaveDestroyMetalOverlay(GLES): deactivated");
}

void RaveClearOverlayToTransparent(void)
{
	gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_RAVE);
}

void RaveScheduleDeferredOverlayDestroy(void)
{
	if (pending_overlay_destroy_scheduled) return;
	pending_overlay_destroy_scheduled = true;
	// On GLES/H700 without dispatch_after, just destroy immediately if refcount is 0
	if (accel_overlay_refcount <= 0) {
		RaveClearOverlayToTransparent();
		RaveDestroyMetalOverlay();
	}
	pending_overlay_destroy_scheduled = false;
}

void RaveCancelDeferredOverlayDestroy(void)
{
	pending_overlay_destroy_scheduled = false;
}

void RaveOverlayRetain(void)
{
	accel_overlay_refcount++;
	pending_overlay_destroy_scheduled = false;
}

void RaveOverlayRelease(void)
{
	accel_overlay_refcount--;
	if (accel_overlay_refcount <= 0) {
		accel_overlay_refcount = 0;
		RaveScheduleDeferredOverlayDestroy();
	}
}


/*
 *  Per-context GLES resource management
 */

void RaveInitMetalResources(RaveDrawPrivate *priv)
{
	RaveGLESState *gs = new RaveGLESState();
	memset(gs, 0, sizeof(RaveGLESState));
	priv->metal = (struct RaveMetalState *)gs;

	// #region agent log
	{
		const char *vendor = (const char *)glGetString(GL_VENDOR);
		const char *renderer = (const char *)glGetString(GL_RENDERER);
		const char *version = (const char *)glGetString(GL_VERSION);
		GLenum err = glGetError();
		char buf[512];
		snprintf(buf, sizeof(buf),
		         "\"vendor\":\"%s\",\"renderer\":\"%s\",\"version\":\"%s\",\"glGetError\":%u",
		         vendor ? vendor : "NULL", renderer ? renderer : "NULL",
		         version ? version : "NULL", (unsigned)err);
		rave_dbg("A", "rave_gles_renderer.cpp:RaveInitMetalResources", "GL context check", buf);
	}
	// #endregion

	for (int i = 0; i < RAVE_NUM_SHADER_VARIANTS; i++) {
		BuildShaderVariant(&gs->programs[i], i);
		if (gs->programs[i].program) {
			RAVE_LOG("Shader variant %d compiled (program=%u)", i, gs->programs[i].program);
		}
	}

	// #region agent log
	{
		int compiled = 0;
		for (int i = 0; i < RAVE_NUM_SHADER_VARIANTS; i++)
			if (gs->programs[i].program) compiled++;
		char buf[128];
		snprintf(buf, sizeof(buf), "\"compiled\":%d,\"total\":%d", compiled, RAVE_NUM_SHADER_VARIANTS);
		rave_dbg("C", "rave_gles_renderer.cpp:RaveInitMetalResources", "shader compilation summary", buf);
	}
	// #endregion

	// Create FBO for offscreen rendering
	gs->fboWidth = priv->width;
	gs->fboHeight = priv->height;

	if (gs->fboWidth > 0 && gs->fboHeight > 0) {
		glGenFramebuffers(1, &gs->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);

		// Color attachment
		glGenTextures(1, &gs->colorTexture);
		glBindTexture(GL_TEXTURE_2D, gs->colorTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gs->fboWidth, gs->fboHeight, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, gs->colorTexture, 0);

		// Depth attachment (renderbuffer, DEPTH_COMPONENT16 for GLES 2.0)
		if (!(priv->flags & kQAContext_NoZBuffer)) {
			glGenRenderbuffers(1, &gs->depthRenderbuffer);
			glBindRenderbuffer(GL_RENDERBUFFER, gs->depthRenderbuffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
			                     gs->fboWidth, gs->fboHeight);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
			                         GL_RENDERBUFFER, gs->depthRenderbuffer);
		}

		GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
			RAVE_LOG("FBO incomplete: 0x%x", fbStatus);
		}

		// #region agent log
		{
			char buf[256];
			snprintf(buf, sizeof(buf),
			         "\"fbo\":%u,\"colorTex\":%u,\"depthRB\":%u,\"fbStatus\":\"0x%x\","
			         "\"width\":%d,\"height\":%d,\"glError\":%u",
			         gs->fbo, gs->colorTexture, gs->depthRenderbuffer,
			         (unsigned)fbStatus, gs->fboWidth, gs->fboHeight, (unsigned)glGetError());
			rave_dbg("D", "rave_gles_renderer.cpp:RaveInitMetalResources", "FBO creation result", buf);
		}
		// #endregion

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	glGenVertexArrays(1, &gs->vao);
	glGenBuffers(1, &gs->vbo);

	gs->renderPassActive = false;
	gs->currentProgramIdx = -1;

	RAVE_LOG("GLES resources initialized: %dx%d, fbo=%u, vao=%u, vbo=%u, %d shader variants",
	         gs->fboWidth, gs->fboHeight, gs->fbo, gs->vao, gs->vbo, RAVE_NUM_SHADER_VARIANTS);
}


void RaveReleaseMetalResources(RaveDrawPrivate *priv)
{
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs) return;

	gles_compositor_clear_overlay(COMPOSITOR_OVERLAY_RAVE);

	for (uint32_t rttHandle : gs->rttTextureHandles)
		RaveResourceFree(rttHandle);
	gs->rttTextureHandles.clear();

	for (int i = 0; i < RAVE_NUM_SHADER_VARIANTS; i++) {
		if (gs->programs[i].program)
			glDeleteProgram(gs->programs[i].program);
	}

	if (gs->vbo) glDeleteBuffers(1, &gs->vbo);
	if (gs->vao) glDeleteVertexArrays(1, &gs->vao);
	if (gs->depthRenderbuffer) glDeleteRenderbuffers(1, &gs->depthRenderbuffer);
	if (gs->colorTexture) glDeleteTextures(1, &gs->colorTexture);
	if (gs->fbo) glDeleteFramebuffers(1, &gs->fbo);
	if (gs->readbackCPU) {
		delete[] gs->readbackCPU;
		gs->readbackCPU = nullptr;
		gs->readbackCPUSize = 0;
	}

	delete gs;
	priv->metal = nullptr;
	RAVE_LOG("GLES resources released");
}


/*
 *  Context lookup helper
 */
static RaveDrawPrivate *GetContextFromDrawAddr(uint32 drawContextAddr)
{
	uint32 handle = ReadMacInt32(drawContextAddr + 0);
	return RaveGetContext(handle);
}

static inline float ReadMacFloat(uint32 addr)
{
	uint32 bits = ReadMacInt32(addr);
	float f;
	memcpy(&f, &bits, sizeof(float));
	return f;
}

/*
 *  kQATag_FogMode (17): callers may use QASetInt or QASetFloat.  state[17] is a
 *  union — reading .i after SetFloat yields float bits (e.g. 0x3E2E9CC8);
 *  reading .f after SetInt(small enum) is wrong for integer tags.
 *  Treat uint32 <= 7 as direct enum; else treat float in [0,7] near an integer.
 */
static int RaveInterpretFogModeTag(const RaveStateValue &v)
{
	uint32_t u = v.i;
	if (u <= 7U)
		return (int)u;
	float f = v.f;
	if (f >= 0.f && f <= 7.f) {
		float r = roundf(f);
		if (fabsf(f - r) < 0.01f)
			return (int)r;
	}
	return 0;
}


/*
 *  ApplyDirtyState - bind shader program and set GL state
 */
static void ApplyDirtyState(RaveDrawPrivate *priv, bool forceAll, bool textured = false)
{
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs) return;

	int blend_mode = (int)priv->state[9].i;
	int func_bits = 0;
	if (textured) func_bits |= 1;
	if (RaveInterpretFogModeTag(priv->state[17]) != 0 || priv->ati_fog_active) func_bits |= 2;
	if (priv->state[31].i != 0 && priv->state[31].i != 7) func_bits |= 4;
	if (priv->multiTextureActive) func_bits |= 8;

	if (forceAll || func_bits != gs->currentProgramIdx) {
		if (func_bits >= 0 && func_bits < RAVE_NUM_SHADER_VARIANTS &&
		    gs->programs[func_bits].program) {
			glUseProgram(gs->programs[func_bits].program);
			gs->currentProgramIdx = func_bits;
		}
	}

	RaveShaderProgram *sp = &gs->programs[gs->currentProgramIdx >= 0 ? gs->currentProgramIdx : 0];
	if (!sp->program) return;

	// Viewport uniform
	glUniform4f(sp->loc_viewport, (float)priv->width, (float)priv->height, 0.0f, 1.0f);

	// Point width
	float pw = priv->state[5].f;
	if (pw < 1.0f) pw = 1.0f;
	glUniform1f(sp->loc_point_width, pw);

	// Depth state
	if (forceAll || (priv->dirty_flags & 1)) {
		int zfunc = (int)priv->state[0].i;
		bool depthWriteEnabled = (priv->state[28].i != 0);
		if (zfunc == 0 && !depthWriteEnabled) {
			glDisable(GL_DEPTH_TEST);
		} else {
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(ZFunctionToGL(zfunc));
			glDepthMask(depthWriteEnabled ? GL_TRUE : GL_FALSE);
		}
	}

	// Blend state
	glEnable(GL_BLEND);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	if (blend_mode == 2) {
		uint32_t glSrc = priv->state[109].i;
		uint32_t glDst = priv->state[110].i;
		glBlendFuncSeparate(GLBlendToGLES(glSrc), GLBlendToGLES(glDst),
		                   GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	} else if (blend_mode == 0) {
		glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		                   GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		                   GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}

	// Channel write mask (kQATag_ChannelMask, bit0=R bit1=G bit2=B bit3=A).
	// Treat 0 as default "all channels" to match Metal path behavior.
	uint32_t channelMask = priv->state[27].i;
	if (channelMask == 0) channelMask = 0xF;
	glColorMask((channelMask & 0x1) ? GL_TRUE : GL_FALSE,
	            (channelMask & 0x2) ? GL_TRUE : GL_FALSE,
	            (channelMask & 0x4) ? GL_TRUE : GL_FALSE,
	            (channelMask & 0x8) ? GL_TRUE : GL_FALSE);

	// Scissor from GL tags (105-108)
	if (priv->state[105].i != 0 || priv->state[106].i != 0 ||
	    priv->state[107].i != 0 || priv->state[108].i != 0) {
		int sx = (int)priv->state[105].i;
		int sy = (int)priv->state[106].i;
		int sw = (int)(priv->state[107].i - priv->state[105].i);
		int sh = (int)(priv->state[108].i - priv->state[106].i);
		if (sx < 0) { sw += sx; sx = 0; }
		if (sy < 0) { sh += sy; sy = 0; }
		if (sx + sw > priv->width)  sw = priv->width - sx;
		if (sy + sh > priv->height) sh = priv->height - sy;
		if (sw > 0 && sh > 0) {
			glEnable(GL_SCISSOR_TEST);
			// GLES scissor Y is bottom-up.
			glScissor(sx, priv->height - sy - sh, sw, sh);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
	} else {
		glDisable(GL_SCISSOR_TEST);
	}

	// Texture binding
	if (textured) {
		uint32_t tex_mac_addr = priv->state[13].i;
		if (rave_perf) {
			rtex_cur_texop_i = (int)priv->state[12].i;
			rtex_cur_texop_f = priv->state[12].f;
			rtex_cur_texstate = (tex_mac_addr == 0) ? 0 : 1;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (tex_mac_addr != 0) {
			uint32_t tex_handle = RaveResourceFindByAddr(tex_mac_addr);
			RaveResourceEntry *tex_entry = RaveResourceGet(tex_handle);
			if (tex_entry) {
				if (!tex_entry->metal_texture && tex_entry->pixmap_mac_addr != 0)
					RaveRealizeDeferredTexture(tex_entry);
				else if (tex_entry->metal_texture && !tex_entry->pixels_copied && tex_entry->pixmap_mac_addr != 0)
					RaveRefreshTextureFromPixmap(tex_entry);

				if (rave_perf && tex_entry->metal_texture)
					rtex_cur_texstate = tex_entry->pixels_copied ? 2 : 1;

				if (tex_entry->metal_texture) {
					GLuint texId = (GLuint)(uintptr_t)tex_entry->metal_texture;
					glBindTexture(GL_TEXTURE_2D, texId);
					if (sp->loc_texture0 >= 0)
						glUniform1i(sp->loc_texture0, 0);

					// Honor GL texture tags (101-104) when set; else use kQATag_TextureFilter.
					uint32_t glWrapU = priv->state[101].i;
					uint32_t glWrapV = priv->state[102].i;
					uint32_t glMag = priv->state[103].i;
					uint32_t glMin = priv->state[104].i;
					GLenum wrapS = (glWrapU == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
					GLenum wrapT = (glWrapV == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
					GLenum minF, magF;
					if (glWrapU != 0 || glWrapV != 0 || glMag != 0 || glMin != 0) {
						magF = (glMag == 1) ? GL_LINEAR : GL_NEAREST;
						minF = (glMin == 1) ? GL_LINEAR : GL_NEAREST;
					} else {
						int filter = (int)priv->state[11].i;
						minF = (filter >= 1) ? GL_LINEAR : GL_NEAREST;
						magF = (filter >= 1) ? GL_LINEAR : GL_NEAREST;
					}
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
				}
			}
		}

		glUniform1i(sp->loc_texture_op, (int)priv->state[12].i);

		// Multi-texture
		if (priv->multiTextureActive && priv->multiTextureHandle != 0) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, 0);
			uint32_t tex2_handle = RaveResourceFindByAddr(priv->multiTextureHandle);
			RaveResourceEntry *tex2_entry = RaveResourceGet(tex2_handle);
			if (tex2_entry) {
				if (!tex2_entry->metal_texture && tex2_entry->pixmap_mac_addr != 0)
					RaveRealizeDeferredTexture(tex2_entry);
				else if (tex2_entry->metal_texture && !tex2_entry->pixels_copied &&
				         tex2_entry->pixmap_mac_addr != 0)
					RaveRefreshTextureFromPixmap(tex2_entry);
				if (tex2_entry->metal_texture) {
					GLuint texId2 = (GLuint)(uintptr_t)tex2_entry->metal_texture;
					glBindTexture(GL_TEXTURE_2D, texId2);
					if (sp->loc_texture1 >= 0)
						glUniform1i(sp->loc_texture1, 1);
					int filter = (int)priv->state[11].i;
					GLenum minF = (filter >= 1) ? GL_LINEAR : GL_NEAREST;
					GLenum magF = (filter >= 1) ? GL_LINEAR : GL_NEAREST;
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
				}
			}
			glUniform1i(sp->loc_multi_texture_op, (int)priv->state[35].i);
			glUniform1f(sp->loc_multi_texture_factor, priv->state[51].f);
			glActiveTexture(GL_TEXTURE0);
		}
	}

	// Fragment uniforms (fog, alpha test, env color)
	if (priv->ati_fog_active) {
		int ati_fog_mode = (int)priv->ati_state[2].i;
		int fog_mode;
		switch (ati_fog_mode) {
			case 0: fog_mode = 0; break;
			case 1: fog_mode = 3; break;
			case 2: fog_mode = 4; break;
			case 3: fog_mode = 1; break;
			case 4: fog_mode = 2; break;
			default: fog_mode = 0; break;
		}
		glUniform1i(sp->loc_fog_mode, fog_mode);
		glUniform4f(sp->loc_fog_color,
		            priv->ati_state[3].f, priv->ati_state[4].f,
		            priv->ati_state[5].f, priv->ati_state[6].f);
		glUniform1f(sp->loc_fog_density, priv->ati_state[7].f);
		glUniform1f(sp->loc_fog_start, priv->ati_state[8].f);
		glUniform1f(sp->loc_fog_end, priv->ati_state[9].f);
	} else {
		glUniform1i(sp->loc_fog_mode, RaveInterpretFogModeTag(priv->state[17]));
		glUniform4f(sp->loc_fog_color,
		            priv->state[19].f, priv->state[20].f,
		            priv->state[21].f, priv->state[18].f);
		glUniform1f(sp->loc_fog_start, priv->state[22].f);
		glUniform1f(sp->loc_fog_end, priv->state[23].f);
		glUniform1f(sp->loc_fog_density, priv->state[24].f);
	}
	float fmd = priv->state[25].f;
	if (fmd == 0.0f) fmd = 1.0f;
	glUniform1f(sp->loc_fog_max_depth, fmd);

	glUniform1i(sp->loc_alpha_test_func, (int)priv->state[31].i);
	glUniform1f(sp->loc_alpha_test_ref, priv->state[46].f);

	glUniform4f(sp->loc_env_color,
	            priv->state[151].f, priv->state[152].f,
	            priv->state[153].f, priv->state[150].f);

	priv->dirty_flags = 0;
}


/*
 *  Vertex submission helper - uploads vertex data via client-side arrays
 */
/*
 *  CPU-side triangle clipping (Sutherland-Hodgman)
 *
 *  Mali-G31 has a limited guard band — triangles with vertices far outside
 *  the viewport are discarded entirely instead of being properly clipped.
 *  We clip against the viewport rectangle on the CPU before submission.
 */

#define CLIP_POLY_MAX 10

static inline RaveVertex ClipLerpVertex(const RaveVertex &a, const RaveVertex &b, float t)
{
	RaveVertex r;
	for (int i = 0; i < 4; i++) {
		r.pos[i]   = a.pos[i]   + t * (b.pos[i]   - a.pos[i]);
		r.color[i] = a.color[i] + t * (b.color[i] - a.color[i]);
		r.uv[i]    = a.uv[i]    + t * (b.uv[i]    - a.uv[i]);
	}
	return r;
}

// Clip polygon against a single half-plane.
//   axis: 0 = X, 1 = Y
//   sign: +1 = keep vertices where coord >= boundary (left/top edge)
//         -1 = keep vertices where coord <= boundary (right/bottom edge)
static int ClipPolyEdge(const RaveVertex *in, int inN, RaveVertex *out,
                        int axis, float sign, float boundary)
{
	if (inN < 3) return 0;
	int outN = 0;
	for (int i = 0; i < inN; i++) {
		const RaveVertex &cur  = in[i];
		const RaveVertex &prev = in[(i + inN - 1) % inN];
		float cVal = cur.pos[axis]  * sign;
		float pVal = prev.pos[axis] * sign;
		float bnd  = boundary * sign;

		bool cIn = (cVal >= bnd);
		bool pIn = (pVal >= bnd);
		if (cIn != pIn) {
			float t = (bnd - pVal) / (cVal - pVal);
			if (outN < CLIP_POLY_MAX)
				out[outN++] = ClipLerpVertex(prev, cur, t);
		}
		if (cIn && outN < CLIP_POLY_MAX)
			out[outN++] = cur;
	}
	return outN;
}

// Clip a single triangle against the viewport [0,w] x [0,h].
// Returns the number of output TRIANGLES (0–5).  Output stored as individual
// triangles (3 verts each) in out[].  out must have room for 15 RaveVertex.
static int ClipTriangleToViewport(const RaveVertex tri[3], RaveVertex *out,
                                  float w, float h)
{
	// Fast reject: if all 3 verts are on the same wrong side of any edge
	bool allLeft  = (tri[0].pos[0] < 0  && tri[1].pos[0] < 0  && tri[2].pos[0] < 0);
	bool allRight = (tri[0].pos[0] > w  && tri[1].pos[0] > w  && tri[2].pos[0] > w);
	bool allAbove = (tri[0].pos[1] < 0  && tri[1].pos[1] < 0  && tri[2].pos[1] < 0);
	bool allBelow = (tri[0].pos[1] > h  && tri[1].pos[1] > h  && tri[2].pos[1] > h);
	if (allLeft || allRight || allAbove || allBelow) return 0;

	// Fast accept: all 3 verts inside viewport
	bool allInside = true;
	for (int i = 0; i < 3; i++) {
		if (tri[i].pos[0] < 0 || tri[i].pos[0] > w ||
		    tri[i].pos[1] < 0 || tri[i].pos[1] > h) {
			allInside = false;
			break;
		}
	}
	if (allInside) {
		out[0] = tri[0]; out[1] = tri[1]; out[2] = tri[2];
		return 1;
	}

	// Sutherland-Hodgman: clip against left, right, top, bottom
	RaveVertex a[CLIP_POLY_MAX], b[CLIP_POLY_MAX];
	int n;
	n = ClipPolyEdge(tri, 3, a, 0, +1.0f, 0.0f);   // left:   x >= 0
	if (n < 3) return 0;
	n = ClipPolyEdge(a, n,   b, 0, -1.0f, w);       // right:  x <= w
	if (n < 3) return 0;
	n = ClipPolyEdge(b, n,   a, 1, +1.0f, 0.0f);   // top:    y >= 0
	if (n < 3) return 0;
	n = ClipPolyEdge(a, n,   b, 1, -1.0f, h);       // bottom: y <= h
	if (n < 3) return 0;

	// Triangulate the clipped polygon as a fan from b[0]
	int numTris = n - 2;
	for (int i = 0; i < numTris; i++) {
		out[i * 3 + 0] = b[0];
		out[i * 3 + 1] = b[i + 1];
		out[i * 3 + 2] = b[i + 2];
	}
	return numTris;
}


/*
 *  Vertex submission – uploads vertex data via VBO, clips triangles on CPU
 */

// Raw GPU submit (no clipping) for already-clipped or non-triangle geometry.
static void SubmitRawVertices(RaveGLESState *gs, const RaveVertex *verts, int count, GLenum mode,
                              const float *uv2)
{
	if (gs->vtxDiagFrames < 30) {
		for (int i = 0; i < count; i++) {
			float x = verts[i].pos[0], y = verts[i].pos[1], z = verts[i].pos[2];
			float invW = verts[i].uv[2];
			float a = verts[i].color[3], r = verts[i].color[0];
			if (x < gs->vtxMinX) gs->vtxMinX = x;
			if (x > gs->vtxMaxX) gs->vtxMaxX = x;
			if (y < gs->vtxMinY) gs->vtxMinY = y;
			if (y > gs->vtxMaxY) gs->vtxMaxY = y;
			if (z < gs->vtxMinZ) gs->vtxMinZ = z;
			if (z > gs->vtxMaxZ) gs->vtxMaxZ = z;
			if (invW < gs->vtxMinW) gs->vtxMinW = invW;
			if (invW > gs->vtxMaxW) gs->vtxMaxW = invW;
			if (a < gs->vtxMinA) gs->vtxMinA = a;
			if (a > gs->vtxMaxA) gs->vtxMaxA = a;
			if (r < gs->vtxMinR) gs->vtxMinR = r;
			if (r > gs->vtxMaxR) gs->vtxMaxR = r;
			if (x < 0 || x > gs->fboWidth || y < 0 || y > gs->fboHeight)
				gs->vtxOutsideCount++;
		}
	}

	// Texture-draw diagnostic accumulation (ungated by frame count; perf only).
	if (rave_perf) {
		const bool isTextured = (gs->currentProgramIdx & 1) != 0;
		if (isTextured) {
			rtex_textured_draws++;
			if (rtex_cur_texop_i != 1) rtex_nonmodulate_draws++;
			if (rtex_cur_texstate >= 0 && rtex_cur_texstate < 3)
				rtex_texstate_count[rtex_cur_texstate]++;
			for (int i = 0; i < count; i++) {
				for (int c = 0; c < 4; c++) {
					float v = verts[i].color[c];
					if (v < rtex_colMin[c]) rtex_colMin[c] = v;
					if (v > rtex_colMax[c]) rtex_colMax[c] = v;
				}
				float iw = verts[i].uv[2];
				if (iw > 0.0f) {
					float u = verts[i].uv[0] / iw, vv = verts[i].uv[1] / iw;
					if (u < rtex_uvMin[0]) rtex_uvMin[0] = u;
					if (u > rtex_uvMax[0]) rtex_uvMax[0] = u;
					if (vv < rtex_uvMin[1]) rtex_uvMin[1] = vv;
					if (vv > rtex_uvMax[1]) rtex_uvMax[1] = vv;
				}
			}
		} else {
			rtex_untextured_draws++;
			for (int i = 0; i < count; i++) {
				for (int c = 0; c < 4; c++) {
					float v = verts[i].color[c];
					if (v < rtex_ucolMin[c]) rtex_ucolMin[c] = v;
					if (v > rtex_ucolMax[c]) rtex_ucolMax[c] = v;
				}
			}
		}
		rtex_report();
	}

	glBindVertexArray(gs->vao);
	glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
	size_t primaryBytes = (size_t)count * sizeof(RaveVertex);
	size_t uv2Bytes = (uv2 != NULL) ? ((size_t)count * 4 * sizeof(float)) : 0;
	glBufferData(GL_ARRAY_BUFFER, primaryBytes + uv2Bytes, NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, primaryBytes, verts);
	if (uv2Bytes > 0)
		glBufferSubData(GL_ARRAY_BUFFER, primaryBytes, uv2Bytes, uv2);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(RaveVertex),
	                      (void *)offsetof(RaveVertex, pos));
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RaveVertex),
	                      (void *)offsetof(RaveVertex, color));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RaveVertex),
	                      (void *)offsetof(RaveVertex, uv));
	if (uv2Bytes > 0) {
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
		                      (void *)primaryBytes);
	} else {
		glDisableVertexAttribArray(3);
		glVertexAttrib4f(3, 0.f, 0.f, 0.f, 0.f);
	}

	glDrawArrays(mode, 0, count);

	glDisableVertexAttribArray(3);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
	glBindVertexArray(0);

	gs->drawCallCount++;
}

static void SubmitVertices(RaveGLESState *gs, const RaveVertex *verts, int count, GLenum mode,
                           const float *uv2 = NULL)
{
	SubmitRawVertices(gs, verts, count, mode, uv2);
}


/*
 *  Vertex conversion (identical logic to Metal version)
 */

static void ConvertGouraudVertex(uint32 srcAddr, RaveVertex *dst, bool perspZ)
{
	dst->pos[0] = ReadMacFloat(srcAddr + 0);
	dst->pos[1] = ReadMacFloat(srcAddr + 4);
	dst->pos[2] = ReadMacFloat(srcAddr + 8);
	dst->pos[3] = 1.0f;

	dst->color[0] = ReadMacFloat(srcAddr + 16);
	dst->color[1] = ReadMacFloat(srcAddr + 20);
	dst->color[2] = ReadMacFloat(srcAddr + 24);
	dst->color[3] = ReadMacFloat(srcAddr + 28);

	float invW = ReadMacFloat(srcAddr + 12);
	dst->uv[0] = 0.0f;
	dst->uv[1] = 0.0f;
	dst->uv[2] = invW;
	dst->uv[3] = 0.0f;
}

static void ConvertTextureVertex(uint32 srcAddr, RaveVertex *dst, bool perspZ, int textureOp)
{
	dst->pos[0] = ReadMacFloat(srcAddr + 0);
	dst->pos[1] = ReadMacFloat(srcAddr + 4);
	dst->pos[2] = ReadMacFloat(srcAddr + 8);
	dst->pos[3] = 1.0f;

	float invW = ReadMacFloat(srcAddr + 12);

	if (textureOp & 4) {
		dst->color[0] = ReadMacFloat(srcAddr + 16);
		dst->color[1] = ReadMacFloat(srcAddr + 20);
		dst->color[2] = ReadMacFloat(srcAddr + 24);
	} else if (textureOp & 1) {
		dst->color[0] = ReadMacFloat(srcAddr + 40);
		dst->color[1] = ReadMacFloat(srcAddr + 44);
		dst->color[2] = ReadMacFloat(srcAddr + 48);
	} else if (textureOp & 2) {
		dst->color[0] = ReadMacFloat(srcAddr + 52);
		dst->color[1] = ReadMacFloat(srcAddr + 56);
		dst->color[2] = ReadMacFloat(srcAddr + 60);
	} else {
		dst->color[0] = 1.0f;
		dst->color[1] = 1.0f;
		dst->color[2] = 1.0f;
	}
	dst->color[3] = ReadMacFloat(srcAddr + 28);

	float uOverW = ReadMacFloat(srcAddr + 32);
	float vOverW = ReadMacFloat(srcAddr + 36);
	dst->uv[0] = uOverW;
	// RAVE texture data is top-left origin; GL sampling expects bottom-left.
	// Keep perspective correctness by flipping in over-W space.
	dst->uv[1] = invW - vOverW;
	dst->uv[2] = invW;
	dst->uv[3] = 0.0f;
}


/*
 *  Z-sorted transparency helpers
 */
static void BufferZSortTriangle(RaveDrawPrivate *priv, const RaveVertex *v0,
                                const RaveVertex *v1, const RaveVertex *v2, bool textured)
{
	if (priv->zsortCount >= RAVE_ZSORT_MAX_TRIANGLES) return;

	ZSortTriangle *tri = &priv->zsortBuffer[priv->zsortCount++];
	memcpy(&tri->verts[0], v0, sizeof(RaveVertex));
	memcpy(&tri->verts[1], v1, sizeof(RaveVertex));
	memcpy(&tri->verts[2], v2, sizeof(RaveVertex));
	tri->sortKey = (v0->pos[2] + v1->pos[2] + v2->pos[2]) / 3.0f;
	tri->textured = textured;
	tri->textureMacAddr = priv->state[13].i;
	tri->textureOp = (int32_t)priv->state[12].i;
	tri->blendMode = (int32_t)priv->state[9].i;
	tri->filterMode = (int32_t)priv->state[11].i;
}

static void FlushZSortBuffer(RaveDrawPrivate *priv)
{
	if (priv->zsortCount == 0) return;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs) return;

	// #region agent log
	{
		static int zflog = 0;
		if (zflog < 10) {
			uint32_t texCount = 0, uniqueTex = 0;
			uint32_t prevTexAddr = 0xFFFFFFFF;
			for (uint32_t i = 0; i < priv->zsortCount; i++) {
				if (priv->zsortBuffer[i].textured) texCount++;
				if (priv->zsortBuffer[i].textureMacAddr != prevTexAddr) {
					uniqueTex++;
					prevTexAddr = priv->zsortBuffer[i].textureMacAddr;
				}
			}
			char buf[256];
			snprintf(buf, sizeof(buf),
				"\"zsortCount\":%u,\"texturedTris\":%u,\"uniqueTextures\":%u,\"frame\":%u",
				priv->zsortCount, texCount, uniqueTex, priv->frameCount);
			rave_dbg("Z3", "rave_gles_renderer.cpp:FlushZSortBuffer", "zsort flush", buf);
			zflog++;
		}
	}
	// #endregion

	std::sort(priv->zsortBuffer, priv->zsortBuffer + priv->zsortCount,
	          [](const ZSortTriangle &a, const ZSortTriangle &b) {
	              return a.sortKey > b.sortKey;
	          });

	// Preserve context tags: z-sort triangles carry their own texture/blend/filter state.
	RaveStateValue savedTexture = priv->state[13];
	RaveStateValue savedTextureOp = priv->state[12];
	RaveStateValue savedBlend = priv->state[9];
	RaveStateValue savedFilter = priv->state[11];
	bool haveReplayState = false;
	bool replayTextured = false;
	uint32_t replayTexAddr = 0;
	int32_t replayTexOp = 0;
	int32_t replayBlend = 0;
	int32_t replayFilter = 0;

	for (uint32_t i = 0; i < priv->zsortCount; i++) {
		ZSortTriangle *tri = &priv->zsortBuffer[i];
		bool stateChanged = !haveReplayState ||
		                    tri->textured != replayTextured ||
		                    tri->textureMacAddr != replayTexAddr ||
		                    tri->textureOp != replayTexOp ||
		                    tri->blendMode != replayBlend ||
		                    tri->filterMode != replayFilter;
		if (stateChanged) {
			priv->state[13].i = tri->textureMacAddr;
			priv->state[12].i = tri->textureOp;
			priv->state[9].i = tri->blendMode;
			priv->state[11].i = tri->filterMode;
			ApplyDirtyState(priv, true, tri->textured);

			haveReplayState = true;
			replayTextured = tri->textured;
			replayTexAddr = tri->textureMacAddr;
			replayTexOp = tri->textureOp;
			replayBlend = tri->blendMode;
			replayFilter = tri->filterMode;
		}
		SubmitVertices(gs, (const RaveVertex *)tri->verts, 3, GL_TRIANGLES);
		gs->triangleCount += 1;
	}

	priv->state[13] = savedTexture;
	priv->state[12] = savedTextureOp;
	priv->state[9] = savedBlend;
	priv->state[11] = savedFilter;
	priv->zsortCount = 0;
}


/*
 *  Render lifecycle
 *
 *  We share the GLES context with SDL2's renderer, so all GL state
 *  must be saved before RAVE rendering and restored afterward.
 *  SavedGLState / SaveGLState / RestoreGLState are defined near the
 *  top of this file (before the compositor).
 */

static SavedGLState s_savedGL;

int32_t NativeRenderStart(uint32_t drawContextAddr, uint32_t dirtyRectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;

	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (gs->renderPassActive) return kQAError;

	gles_gl_lock();
	SaveGLState(&s_savedGL);

	// #region agent log
	{
		char buf[640];
		snprintf(buf, sizeof(buf),
		         "\"prevFBO\":%d,\"targetFBO\":%u,\"fboWidth\":%d,\"fboHeight\":%d,"
		         "\"privWidth\":%d,\"privHeight\":%d,\"privLeft\":%d,\"privTop\":%d,"
		         "\"frame\":%u,\"clearR\":%.3f,\"clearG\":%.3f,\"clearB\":%.3f,"
		         "\"savedVP\":[%d,%d,%d,%d],\"savedDepth\":%d,\"savedBlend\":%d,"
		         "\"savedScissor\":%d,\"savedProg\":%d,\"savedVAO\":%d",
		         s_savedGL.fbo, gs->fbo, gs->fboWidth, gs->fboHeight,
		         priv->width, priv->height, priv->left, priv->top,
		         priv->frameCount, priv->state[2].f, priv->state[3].f, priv->state[4].f,
		         s_savedGL.viewport[0], s_savedGL.viewport[1],
		         s_savedGL.viewport[2], s_savedGL.viewport[3],
		         (int)s_savedGL.depthTest, (int)s_savedGL.blend,
		         (int)s_savedGL.scissorTest, s_savedGL.activeProgram,
		         s_savedGL.boundVAO);
		rave_dbg("B,D,U,Z6", "rave_gles_renderer.cpp:NativeRenderStart", "render start + saved GL", buf);
	}
	// #endregion

	glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
	glViewport(0, 0, gs->fboWidth, gs->fboHeight);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	// Clear to background color
	float r = priv->state[2].f;
	float g = priv->state[3].f;
	float b = priv->state[4].f;
	glClearColor(r, g, b, 1.0f);

	GLbitfield clearBits = GL_COLOR_BUFFER_BIT;
	if (!(priv->flags & kQAContext_NoZBuffer)) {
		glClearDepthf(1.0f);
		clearBits |= GL_DEPTH_BUFFER_BIT;
		glDepthMask(GL_TRUE);
	}
	glClear(clearBits);

	// #region agent log
	{
		GLint boundFbo = -1;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
		GLenum err = glGetError();
		char buf[128];
		snprintf(buf, sizeof(buf), "\"boundFBO\":%d,\"glError\":%u", boundFbo, (unsigned)err);
		rave_dbg("B,D", "rave_gles_renderer.cpp:NativeRenderStart", "post-clear state", buf);
	}
	// #endregion

	gs->renderPassActive = true;
	gs->currentProgramIdx = -1;
	gs->drawCallCount = 0;
	gs->triangleCount = 0;
	gs->vtxMinX = 1e30f; gs->vtxMaxX = -1e30f;
	gs->vtxMinY = 1e30f; gs->vtxMaxY = -1e30f;
	gs->vtxMinZ = 1e30f; gs->vtxMaxZ = -1e30f;
	gs->vtxMinW = 1e30f; gs->vtxMaxW = -1e30f;
	gs->vtxMinA = 1e30f; gs->vtxMaxA = -1e30f;
	gs->vtxMinR = 1e30f; gs->vtxMaxR = -1e30f;
	gs->vtxOutsideCount = 0;
	priv->multiTextureActive = false;
	priv->multiTexStagingCount = 0;

	priv->frameCount++;

	ApplyDirtyState(priv, true);

	RAVE_LOG("RenderStart(GLES): ctx=0x%08x %dx%d frame=%d",
	         drawContextAddr, gs->fboWidth, gs->fboHeight, priv->frameCount);
	return kQANoErr;
}


int32_t NativeRenderEnd(uint32_t drawContextAddr, uint32_t modifiedRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;

	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQAError;

	if (!rave_perf_init) {
		rave_perf = PrefsFindBool("perf_profile");
		rave_perf_init = true;
	}
	const uint64 rave_t0 = rave_perf ? GetTicks_usec() : 0;

	FlushZSortBuffer(priv);
	// glReadPixels below is synchronizing by itself; avoid an extra full pipeline stall.
	glFlush();
	const uint64 rave_t_flush = rave_perf ? GetTicks_usec() : 0;

	// #region agent log
	{
		char buf[256];
		snprintf(buf, sizeof(buf),
		         "\"draws\":%u,\"tris\":%u,\"fbo\":%u,\"colorTex\":%u,"
		         "\"frame\":%u,\"glError\":%u",
		         gs->drawCallCount, gs->triangleCount, gs->fbo, gs->colorTexture,
		         priv->frameCount, (unsigned)glGetError());
		rave_dbg("B,E", "rave_gles_renderer.cpp:NativeRenderEnd", "render end stats", buf);
	}
	if (gs->vtxDiagFrames < 30) {
		char buf[1024];
		int fogMode = RaveInterpretFogModeTag(priv->state[17]);
		int atiFogActive = priv->ati_fog_active ? 1 : 0;
		int atiFogMode = priv->ati_fog_active ? (int)priv->ati_state[2].i : -1;
		float fogStart = priv->ati_fog_active ? priv->ati_state[8].f : priv->state[22].f;
		float fogEnd   = priv->ati_fog_active ? priv->ati_state[9].f : priv->state[23].f;
		float fogDensity = priv->ati_fog_active ? priv->ati_state[7].f : priv->state[24].f;
		float fogR = priv->ati_fog_active ? priv->ati_state[3].f : priv->state[19].f;
		float fogG = priv->ati_fog_active ? priv->ati_state[4].f : priv->state[20].f;
		float fogB = priv->ati_fog_active ? priv->ati_state[5].f : priv->state[21].f;
		int blendMode = (int)priv->state[9].i;
		int depthFunc = (int)priv->state[0].i;
		int alphaFunc = (int)priv->state[31].i;
		float alphaRef = priv->state[46].f;
		uint32_t channelMask = priv->state[27].i;
		snprintf(buf, sizeof(buf),
		         "\"frame\":%u,\"x\":[%.1f,%.1f],\"y\":[%.1f,%.1f],"
		         "\"z\":[%.4f,%.4f],\"invW\":[%.4f,%.4f],"
		         "\"alpha\":[%.3f,%.3f],\"colorR\":[%.3f,%.3f],"
		         "\"outsideVerts\":%u,\"fboW\":%d,\"fboH\":%d,"
		         "\"draws\":%u,\"tris\":%u,"
		         "\"fogMode\":%d,\"atiFog\":%d,\"atiFogMode\":%d,"
		         "\"fogStart\":%.4f,\"fogEnd\":%.4f,\"fogDensity\":%.4f,"
		         "\"fogColor\":[%.3f,%.3f,%.3f],"
		         "\"blendMode\":%d,\"depthFunc\":%d,"
		         "\"alphaFunc\":%d,\"alphaRef\":%.3f,\"channelMask\":%u",
		         priv->frameCount,
		         gs->vtxMinX, gs->vtxMaxX, gs->vtxMinY, gs->vtxMaxY,
		         gs->vtxMinZ, gs->vtxMaxZ, gs->vtxMinW, gs->vtxMaxW,
		         gs->vtxMinA, gs->vtxMaxA, gs->vtxMinR, gs->vtxMaxR,
		         gs->vtxOutsideCount, gs->fboWidth, gs->fboHeight,
		         gs->drawCallCount, gs->triangleCount,
		         fogMode, atiFogActive, atiFogMode,
		         fogStart, fogEnd, fogDensity,
		         fogR, fogG, fogB,
		         blendMode, depthFunc,
		         alphaFunc, alphaRef, channelMask);
		rave_dbg("DIAG", "rave_gles_renderer.cpp:NativeRenderEnd", "vertex ranges", buf);
		gs->vtxDiagFrames++;
	}
	// #endregion

	// Read render FBO directly after glFinish().  The old presentFBO + blit
	// path could show alternating stale/new frames on Mali (tile deferred).
	uint8_t *readbackPtr = nullptr;
	int readbackW = 0, readbackH = 0;
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gs->fbo);
		int w = gs->fboWidth, h = gs->fboHeight;
		uint32_t needed = (uint32_t)(w * h * 4);
		if (!gs->readbackCPU || gs->readbackCPUSize < needed) {
			delete[] gs->readbackCPU;
			gs->readbackCPU = new uint8_t[needed];
			gs->readbackCPUSize = needed;
		}
		if (gs->readbackCPU) {
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gs->readbackCPU);
			readbackPtr = gs->readbackCPU;
			readbackW = w;
			readbackH = h;
		}
	}
	const uint64 rave_t_readback = rave_perf ? GetTicks_usec() : 0;

	if (modifiedRectAddr != 0) {
		WriteMacInt32(modifiedRectAddr + 0,  (uint32_t)priv->left);
		WriteMacInt32(modifiedRectAddr + 4,  (uint32_t)priv->top);
		WriteMacInt32(modifiedRectAddr + 8,  (uint32_t)(priv->left + priv->width));
		WriteMacInt32(modifiedRectAddr + 12, (uint32_t)(priv->top + priv->height));
	}

	gs->renderPassActive = false;
	RestoreGLState(&s_savedGL);
	gles_gl_unlock();

	if (readbackPtr)
		video_blit_rave_fbo(readbackPtr, readbackW, readbackH, priv->left, priv->top);

	if (rave_perf) {
		static uint64 prev_render_end_us = 0;
		const uint64 rave_t_blit = GetTicks_usec();
		const uint64 renderend_us = rave_t_blit - rave_t0;
		// g_rave_dispatch_us holds this frame's draw/setup dispatch time (the
		// current RenderEnd dispatch isn't accounted until this call's RAII
		// timer fires, after we return); add RenderEnd's measured cost here.
		const int64 draws_us = (g_rave_dispatch_us > 0) ? g_rave_dispatch_us : 0;
		const uint64 rave_cpp_us = (uint64)draws_us + renderend_us;
		if (prev_render_end_us != 0) {
			const uint64 period = rave_t_blit - prev_render_end_us;
			const uint64 guest = (period > rave_cpp_us) ? (period - rave_cpp_us) : 0;
			rave_perf_guest_us    += guest;
			rave_perf_dispatch_us += (uint64)draws_us;
			rave_perf_flush_us    += rave_t_flush    - rave_t0;
			rave_perf_readback_us += rave_t_readback - rave_t_flush;
			rave_perf_blit_us     += rave_t_blit     - rave_t_readback;
			rave_perf_frames++;
			rave_perf_report();
		}
		prev_render_end_us = rave_t_blit;
		// Seed negative so this RenderEnd dispatch's RAII (which fires after we
		// return and adds ~renderend_us) nets back to ~0 for the next frame.
		g_rave_dispatch_us = -(int64)renderend_us;
	}

	// Register FBO color texture with compositor for GPU overlay blit at present
	// time.  The last two args must be the full Mac screen dimensions (not the
	// draw-context size) so the compositor maps the (left,top) offset into NDC
	// correctly -- otherwise the overlay is scaled/shifted right and down.  This
	// matches the GL renderer's call (gl_gles_renderer.cpp).
	gles_compositor_set_overlay(COMPOSITOR_OVERLAY_RAVE, gs->colorTexture,
	                            gs->fboWidth, gs->fboHeight,
	                            priv->left, priv->top,
	                            gs->fboWidth, gs->fboHeight,
	                            VModes[cur_mode].viXsize, VModes[cur_mode].viYsize);

	RAVE_LOG("RenderEnd(GLES): ctx=0x%08x draws=%d tris=%d",
	         drawContextAddr, gs->drawCallCount, gs->triangleCount);
	return kQANoErr;
}


int32_t NativeRenderAbort(uint32_t drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;

	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	gs->renderPassActive = false;
	priv->zsortCount = 0;
	priv->multiTextureActive = false;
	priv->multiTexStagingCount = 0;
	RestoreGLState(&s_savedGL);
	gles_gl_unlock();

	RAVE_LOG("RenderAbort(GLES): ctx=0x%08x", drawContextAddr);
	return kQANoErr;
}


int32_t NativeFlush(uint32_t drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;

	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	glFlush();
	gs->currentProgramIdx = -1;

	RAVE_LOG("Flush(GLES): ctx=0x%08x", drawContextAddr);
	return kQANoErr;
}


int32_t NativeSync(uint32_t drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;

	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (gs->renderPassActive) {
		NativeFlush(drawContextAddr);
	}
	glFinish();

	return kQANoErr;
}


/*
 *  Draw methods
 */

int32_t NativeDrawTriGouraud(uint32_t drawContextAddr, uint32_t v0Addr,
                             uint32_t v1Addr, uint32_t v2Addr, uint32_t flags)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	(void)flags;
	bool perspZ = (priv->state[10].i != 0);

	RaveVertex verts[3];
	ConvertGouraudVertex(v0Addr, &verts[0], perspZ);
	ConvertGouraudVertex(v1Addr, &verts[1], perspZ);
	ConvertGouraudVertex(v2Addr, &verts[2], perspZ);

	if (priv->state[29].i == 1) {
		BufferZSortTriangle(priv, &verts[0], &verts[1], &verts[2], false);
		return kQANoErr;
	}

	ApplyDirtyState(priv, false);
	SubmitVertices(gs, verts, 3, GL_TRIANGLES);
	gs->triangleCount += 1;
	return kQANoErr;
}


int32_t NativeDrawTriTexture(uint32_t drawContextAddr, uint32_t v0Addr,
                             uint32_t v1Addr, uint32_t v2Addr, uint32_t flags)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	(void)flags;
	bool perspZ = (priv->state[10].i != 0);
	int textureOp = (int)priv->state[12].i;

	RaveVertex verts[3];
	ConvertTextureVertex(v0Addr, &verts[0], perspZ, textureOp);
	ConvertTextureVertex(v1Addr, &verts[1], perspZ, textureOp);
	ConvertTextureVertex(v2Addr, &verts[2], perspZ, textureOp);

	if (priv->state[29].i == 1) {
		BufferZSortTriangle(priv, &verts[0], &verts[1], &verts[2], true);
		return kQANoErr;
	}

	ApplyDirtyState(priv, false, true);
	const float *uv2 = NULL;
	if (priv->multiTextureActive && priv->multiTexStagingCount >= 3)
		uv2 = (const float *)priv->multiTexStagingBuffer;
	SubmitVertices(gs, verts, 3, GL_TRIANGLES, uv2);
	gs->triangleCount += 1;
	return kQANoErr;
}


int32_t NativeDrawVGouraud(uint32_t drawContextAddr, uint32_t nVertices,
                           uint32_t vertexMode, uint32_t verticesAddr, uint32_t flagsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive || nVertices == 0) return kQANoErr;

	(void)flagsAddr; // Backfacing flag is a hint; renderer does not cull here.
	bool perspZ = (priv->state[10].i != 0);
	ApplyDirtyState(priv, false);

	switch (vertexMode) {
	case 0: { // Point
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, nVertices, GL_POINTS);
		delete[] verts;
		break;
	}
	case 1: { // Line
		uint32_t nLines = nVertices / 2;
		if (nLines == 0) break;
		RaveVertex *verts = new RaveVertex[nLines * 2];
		for (uint32_t i = 0; i < nLines * 2; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, nLines * 2, GL_LINES);
		delete[] verts;
		break;
	}
	case 2: { // Polyline
		if (nVertices < 2) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, nVertices, GL_LINE_STRIP);
		delete[] verts;
		break;
	}
	case 3: { // Tri (triangle list)
		uint32_t triVerts = (nVertices / 3) * 3;
		if (triVerts == 0) break;
		RaveVertex *verts = new RaveVertex[triVerts];
		for (uint32_t i = 0; i < triVerts; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, triVerts, GL_TRIANGLES);
		gs->triangleCount += triVerts / 3;
		delete[] verts;
		break;
	}
	case 4: { // Strip
		if (nVertices < 3) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, nVertices, GL_TRIANGLE_STRIP);
		gs->triangleCount += nVertices - 2;
		delete[] verts;
		break;
	}
	case 5: { // Fan
		if (nVertices < 3) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		SubmitVertices(gs, verts, nVertices, GL_TRIANGLE_FAN);
		gs->triangleCount += nVertices - 2;
		delete[] verts;
		break;
	}
	default:
		RAVE_LOG("DrawVGouraud: unknown vertexMode %d", vertexMode);
		break;
	}
	return kQANoErr;
}


int32_t NativeDrawVTexture(uint32_t drawContextAddr, uint32_t nVertices,
                           uint32_t vertexMode, uint32_t verticesAddr, uint32_t flagsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive || nVertices == 0) return kQANoErr;

	(void)flagsAddr; // Backfacing flag is a hint; renderer does not cull here.
	bool perspZ = (priv->state[10].i != 0);
	int textureOp = (int)priv->state[12].i;
	ApplyDirtyState(priv, false, true);
	auto uv2_for_count = [&](uint32_t count) -> const float * {
		if (priv->multiTextureActive && priv->multiTexStagingCount >= count)
			return (const float *)priv->multiTexStagingBuffer;
		return NULL;
	};

	switch (vertexMode) {
	case 0: { // Point
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, nVertices, GL_POINTS, uv2_for_count(nVertices));
		delete[] verts;
		break;
	}
	case 1: { // Line
		uint32_t nLines = nVertices / 2;
		if (nLines == 0) break;
		RaveVertex *verts = new RaveVertex[nLines * 2];
		for (uint32_t i = 0; i < nLines * 2; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, nLines * 2, GL_LINES, uv2_for_count(nLines * 2));
		delete[] verts;
		break;
	}
	case 2: { // Polyline
		if (nVertices < 2) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, nVertices, GL_LINE_STRIP, uv2_for_count(nVertices));
		delete[] verts;
		break;
	}
	case 3: { // Tri (triangle list)
		uint32_t triVerts = (nVertices / 3) * 3;
		if (triVerts == 0) break;
		RaveVertex *verts = new RaveVertex[triVerts];
		for (uint32_t i = 0; i < triVerts; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, triVerts, GL_TRIANGLES, uv2_for_count(triVerts));
		gs->triangleCount += triVerts / 3;
		delete[] verts;
		break;
	}
	case 4: { // Strip
		if (nVertices < 3) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, nVertices, GL_TRIANGLE_STRIP, uv2_for_count(nVertices));
		gs->triangleCount += nVertices - 2;
		delete[] verts;
		break;
	}
	case 5: { // Fan
		if (nVertices < 3) break;
		RaveVertex *verts = new RaveVertex[nVertices];
		for (uint32_t i = 0; i < nVertices; i++)
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ, textureOp);
		SubmitVertices(gs, verts, nVertices, GL_TRIANGLE_FAN, uv2_for_count(nVertices));
		gs->triangleCount += nVertices - 2;
		delete[] verts;
		break;
	}
	default:
		RAVE_LOG("DrawVTexture: unknown vertexMode %d", vertexMode);
		break;
	}
	return kQANoErr;
}


int32_t NativeSubmitVerticesGouraud(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || nVertices == 0) return kQANoErr;

	// #region agent log
	{
		static int svglog = 0;
		if (svglog < 20) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				"\"prevCount\":%u,\"nVertices\":%u,\"capacity\":%u,\"frame\":%u",
				priv->vertexStagingCount, nVertices, priv->vertexStagingCapacity,
				priv->frameCount);
			rave_dbg("Z1", "rave_gles_renderer.cpp:NativeSubmitVerticesGouraud", "staging before submit", buf);
			svglog++;
		}
	}
	// #endregion

	if (nVertices > priv->vertexStagingCapacity)
		nVertices = priv->vertexStagingCapacity;

	priv->vertexStagingCount = 0;
	bool perspZ = (priv->state[10].i != 0);
	RaveVertex *dst = (RaveVertex *)priv->vertexStagingBuffer;
	for (uint32_t i = 0; i < nVertices; i++)
		ConvertGouraudVertex(verticesAddr + i * 32, &dst[i], perspZ);
	priv->vertexStagingCount = nVertices;
	return kQANoErr;
}


int32_t NativeSubmitVerticesTexture(uint32_t drawContextAddr, uint32_t nVertices, uint32_t verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || nVertices == 0) return kQANoErr;

	// #region agent log
	{
		static int svtlog = 0;
		if (svtlog < 20) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				"\"prevCount\":%u,\"nVertices\":%u,\"capacity\":%u,\"frame\":%u,\"texAddr\":\"0x%08x\",\"texOp\":%d",
				priv->vertexStagingCount, nVertices, priv->vertexStagingCapacity,
				priv->frameCount, priv->state[13].i, (int)priv->state[12].i);
			rave_dbg("Z1", "rave_gles_renderer.cpp:NativeSubmitVerticesTexture", "staging before submit", buf);
			svtlog++;
		}
	}
	// #endregion

	if (nVertices > priv->vertexStagingCapacity)
		nVertices = priv->vertexStagingCapacity;

	priv->vertexStagingCount = 0;
	bool perspZ = (priv->state[10].i != 0);
	int textureOp = (int)priv->state[12].i;
	RaveVertex *dst = (RaveVertex *)priv->vertexStagingBuffer;
	for (uint32_t i = 0; i < nVertices; i++)
		ConvertTextureVertex(verticesAddr + i * 64, &dst[i], perspZ, textureOp);
	priv->vertexStagingCount = nVertices;
	return kQANoErr;
}


int32_t NativeSubmitMultiTextureParams(uint32_t drawContextAddr, uint32_t nVertices, uint32_t multiTexParamsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || nVertices == 0 || multiTexParamsAddr == 0) return kQANoErr;
	if (nVertices > priv->vertexStagingCapacity)
		nVertices = priv->vertexStagingCapacity;

	float *dst = (float *)priv->multiTexStagingBuffer;
	for (uint32_t i = 0; i < nVertices; i++) {
		uint32_t uvAddr = multiTexParamsAddr + i * 12;
		// TQAVMultiTexture layout: invW, uOverW, vOverW (RAVE 1.6).
		float invW = ReadMacFloat(uvAddr + 0);
		float uOverW = ReadMacFloat(uvAddr + 4);
		float vOverW = ReadMacFloat(uvAddr + 8);
		dst[i * 4 + 0] = uOverW;
		dst[i * 4 + 1] = invW - vOverW;
		dst[i * 4 + 2] = invW;
		dst[i * 4 + 3] = 0.0f;
	}
	priv->multiTexStagingCount = nVertices;
	priv->multiTextureActive = true;
	// Match Pocket behavior: latch the secondary texture handle once.
	if (priv->multiTextureHandle == 0)
		priv->multiTextureHandle = priv->state[26].i;
	priv->multiTextureOp = 0;
	priv->multiTextureFactor = 0.5f;
	return kQANoErr;
}


int32_t NativeDrawTriMeshGouraud(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive || numTriangles == 0) return kQANoErr;

	ApplyDirtyState(priv, false);

	RaveVertex *verts = new RaveVertex[numTriangles * 3];
	uint32_t outIdx = 0;
	uint32_t oobCount = 0;

	for (uint32_t t = 0; t < numTriangles; t++) {
		uint32_t triAddr = trianglesAddr + t * 16;
		// TQAIndexedTriangle layout: +0 flags, +4/+8/+12 indices.
		uint32_t idx0 = ReadMacInt32(triAddr + 4);
		uint32_t idx1 = ReadMacInt32(triAddr + 8);
		uint32_t idx2 = ReadMacInt32(triAddr + 12);

		if (idx0 < priv->vertexStagingCount && idx1 < priv->vertexStagingCount &&
		    idx2 < priv->vertexStagingCount) {
			RaveVertex *staged = (RaveVertex *)priv->vertexStagingBuffer;
			verts[outIdx++] = staged[idx0];
			verts[outIdx++] = staged[idx1];
			verts[outIdx++] = staged[idx2];
		} else {
			oobCount++;
		}
	}

	// #region agent log
	{
		static int dmglog = 0;
		if (dmglog < 20) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				"\"numTris\":%u,\"emitted\":%u,\"oob\":%u,\"staged\":%u,\"frame\":%u",
				numTriangles, outIdx / 3, oobCount, priv->vertexStagingCount, priv->frameCount);
			rave_dbg("Z1", "rave_gles_renderer.cpp:NativeDrawTriMeshGouraud", "mesh draw gouraud", buf);
			dmglog++;
		}
	}
	// #endregion

	if (outIdx > 0) {
		SubmitVertices(gs, verts, outIdx, GL_TRIANGLES);
		gs->triangleCount += outIdx / 3;
	}

	delete[] verts;
	return kQANoErr;
}


int32_t NativeDrawTriMeshTexture(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive || numTriangles == 0) return kQANoErr;

	ApplyDirtyState(priv, false, true);

	RaveVertex *verts = new RaveVertex[numTriangles * 3];
	float *uv2Indexed = NULL;
	const float *uv2Staged = NULL;
	if (priv->multiTextureActive && priv->multiTexStagingCount > 0) {
		uv2Indexed = new float[numTriangles * 3 * 4];
		uv2Staged = (const float *)priv->multiTexStagingBuffer;
	}
	uint32_t outIdx = 0;
	uint32_t oobCount = 0;
	uint32_t maxIdx = 0;

	for (uint32_t t = 0; t < numTriangles; t++) {
		uint32_t triAddr = trianglesAddr + t * 16;
		// TQAIndexedTriangle layout: +0 flags, +4/+8/+12 indices.
		uint32_t idx0 = ReadMacInt32(triAddr + 4);
		uint32_t idx1 = ReadMacInt32(triAddr + 8);
		uint32_t idx2 = ReadMacInt32(triAddr + 12);

		if (idx0 > maxIdx) maxIdx = idx0;
		if (idx1 > maxIdx) maxIdx = idx1;
		if (idx2 > maxIdx) maxIdx = idx2;

		if (idx0 < priv->vertexStagingCount && idx1 < priv->vertexStagingCount &&
		    idx2 < priv->vertexStagingCount) {
			RaveVertex *staged = (RaveVertex *)priv->vertexStagingBuffer;
			uint32_t base = outIdx;
			verts[outIdx++] = staged[idx0];
			verts[outIdx++] = staged[idx1];
			verts[outIdx++] = staged[idx2];
			if (uv2Indexed) {
				float *d0 = &uv2Indexed[(base + 0) * 4];
				float *d1 = &uv2Indexed[(base + 1) * 4];
				float *d2 = &uv2Indexed[(base + 2) * 4];
				if (idx0 < priv->multiTexStagingCount) {
					const float *s = &uv2Staged[idx0 * 4];
					d0[0] = s[0]; d0[1] = s[1]; d0[2] = s[2]; d0[3] = s[3];
				} else {
					d0[0] = d0[1] = d0[2] = d0[3] = 0.0f;
				}
				if (idx1 < priv->multiTexStagingCount) {
					const float *s = &uv2Staged[idx1 * 4];
					d1[0] = s[0]; d1[1] = s[1]; d1[2] = s[2]; d1[3] = s[3];
				} else {
					d1[0] = d1[1] = d1[2] = d1[3] = 0.0f;
				}
				if (idx2 < priv->multiTexStagingCount) {
					const float *s = &uv2Staged[idx2 * 4];
					d2[0] = s[0]; d2[1] = s[1]; d2[2] = s[2]; d2[3] = s[3];
				} else {
					d2[0] = d2[1] = d2[2] = d2[3] = 0.0f;
				}
			}
		} else {
			oobCount++;
		}
	}

	// #region agent log
	{
		static int dmtlog = 0;
		if (dmtlog < 30) {
			GLboolean scissorOn = GL_FALSE;
			glGetBooleanv(GL_SCISSOR_TEST, &scissorOn);
			char buf[384];
			snprintf(buf, sizeof(buf),
				"\"numTris\":%u,\"emitted\":%u,\"oob\":%u,\"staged\":%u,"
				"\"maxIdx\":%u,\"frame\":%u,\"texAddr\":\"0x%08x\",\"texOp\":%d,"
				"\"scissorOn\":%d,\"progIdx\":%d",
				numTriangles, outIdx / 3, oobCount, priv->vertexStagingCount,
				maxIdx, priv->frameCount,
				priv->state[13].i, (int)priv->state[12].i,
				(int)scissorOn, gs->currentProgramIdx);
			rave_dbg("Z1,Z2,Z4", "rave_gles_renderer.cpp:NativeDrawTriMeshTexture", "mesh draw texture", buf);
			dmtlog++;
		}
	}
	// #endregion

	if (outIdx > 0) {
		SubmitVertices(gs, verts, outIdx, GL_TRIANGLES, uv2Indexed);
		gs->triangleCount += outIdx / 3;
	}

	delete[] uv2Indexed;
	delete[] verts;
	return kQANoErr;
}


int32_t NativeDrawPoint(uint32_t drawContextAddr, uint32_t v0Addr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	bool perspZ = (priv->state[10].i != 0);
	RaveVertex vert;
	ConvertGouraudVertex(v0Addr, &vert, perspZ);
	ApplyDirtyState(priv, false);
	SubmitVertices(gs, &vert, 1, GL_POINTS);
	return kQANoErr;
}


int32_t NativeDrawLine(uint32_t drawContextAddr, uint32_t v0Addr, uint32_t v1Addr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	bool perspZ = (priv->state[10].i != 0);
	RaveVertex verts[2];
	ConvertGouraudVertex(v0Addr, &verts[0], perspZ);
	ConvertGouraudVertex(v1Addr, &verts[1], perspZ);
	ApplyDirtyState(priv, false);
	SubmitVertices(gs, verts, 2, GL_LINES);
	return kQANoErr;
}


int32_t NativeDrawBitmap(uint32_t drawContextAddr, uint32_t vertexAddr, uint32_t bitmapMacAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQANoErr;

	float x = ReadMacFloat(vertexAddr + 0);
	float y = ReadMacFloat(vertexAddr + 4);
	// Match Pocket path: bitmap quads render in screen space at z=0.
	float z = 0.0f;

	uint32_t bmp_handle = RaveResourceFindByAddr(bitmapMacAddr);
	RaveResourceEntry *bmp_entry = RaveResourceGet(bmp_handle);
	if (!bmp_entry) return kQANoErr;

	if (!bmp_entry->metal_texture && bmp_entry->pixmap_mac_addr != 0)
		RaveRealizeDeferredTexture(bmp_entry);
	if (!bmp_entry->metal_texture) return kQANoErr;

	float bw = (float)bmp_entry->width;
	float bh = (float)bmp_entry->height;

	// Apply bitmap scaling from tags 52/53
	float scaleX = priv->state[52].f;
	float scaleY = priv->state[53].f;
	if (scaleX <= 0.0f) scaleX = 1.0f;
	if (scaleY <= 0.0f) scaleY = 1.0f;
	bw *= scaleX;
	bh *= scaleY;

	GLuint texId = (GLuint)(uintptr_t)bmp_entry->metal_texture;
	float alpha = priv->state[55].f; // kQATag_BitmapOpacity
	if (alpha < 0.0f) alpha = 0.0f;
	if (alpha > 1.0f) alpha = 1.0f;
	float invW = 1.0f;

	RaveVertex quad[6];
	// Flip V to match top-left source texel origin.
	quad[0] = { {x,      y,      z, 1.0f}, {1,1,1,alpha}, {0,      invW,   invW, 0} };
	quad[1] = { {x + bw, y,      z, 1.0f}, {1,1,1,alpha}, {invW,   invW,   invW, 0} };
	quad[2] = { {x,      y + bh, z, 1.0f}, {1,1,1,alpha}, {0,      0,      invW, 0} };
	quad[3] = { {x + bw, y,      z, 1.0f}, {1,1,1,alpha}, {invW,   invW,   invW, 0} };
	quad[4] = { {x + bw, y + bh, z, 1.0f}, {1,1,1,alpha}, {invW,   0,      invW, 0} };
	quad[5] = { {x,      y + bh, z, 1.0f}, {1,1,1,alpha}, {0,      0,      invW, 0} };

	// Bitmap path should be texture-only (no fog/alpha-test/multi-texture),
	// and should not participate in scene depth testing.
	ApplyDirtyState(priv, false, true);
	if (gs->programs[1].program) {
		glUseProgram(gs->programs[1].program);
		gs->currentProgramIdx = 1;
		RaveShaderProgram *sp = &gs->programs[1];
		if (sp->loc_texture_op >= 0) glUniform1i(sp->loc_texture_op, 0);
		if (sp->loc_texture0 >= 0) glUniform1i(sp->loc_texture0, 0);
	}
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	// Override texture binding to bitmap
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texId);
	int filter = (int)priv->state[54].i;
	GLenum filterMode = (filter >= 1) ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterMode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterMode);

	SubmitVertices(gs, quad, 6, GL_TRIANGLES);
	gs->triangleCount += 2;
	gs->currentProgramIdx = -1;
	priv->dirty_flags |= 1; // re-apply depth state on next draw

	return kQANoErr;
}


int32_t NativeSetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                              uint32_t callback, uint32_t refCon)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQAError;
	if (method >= RAVE_NUM_NOTICE_METHODS) return kQANoErr;
	priv->noticeMethods[method].callback = callback;
	priv->noticeMethods[method].refCon = refCon;
	return kQANoErr;
}


int32_t NativeGetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                              uint32_t callbackOutPtr, uint32_t refConOutPtr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQAError;
	if (method >= RAVE_NUM_NOTICE_METHODS) return kQANoErr;
	WriteMacInt32(callbackOutPtr, priv->noticeMethods[method].callback);
	WriteMacInt32(refConOutPtr, priv->noticeMethods[method].refCon);
	return kQANoErr;
}


/*
 *  Buffer access methods
 */

int32_t NativeAccessDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr,
                               uint32_t rowBytesPtr, uint32_t bufferPtrPtr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQAError;

	uint32_t rowBytes = (uint32_t)(priv->width * 4);
	uint32_t bufSize = rowBytes * (uint32_t)priv->height;

	if (!gs->drawBufferCPU || gs->drawBufferCPUSize != bufSize) {
		uint32_t macAddr = Mac_sysalloc(bufSize);
		if (macAddr == 0) return kQAError;
		gs->drawBufferCPU = Mac2HostAddr(macAddr);
		gs->drawBufferCPUMac = macAddr;
		gs->drawBufferCPUSize = bufSize;
	}

	glReadPixels(0, 0, priv->width, priv->height, GL_RGBA, GL_UNSIGNED_BYTE, gs->drawBufferCPU);
	WriteMacInt32(bufferPtrPtr, gs->drawBufferCPUMac);
	WriteMacInt32(rowBytesPtr, rowBytes);
	gs->drawBufferAccessed = true;

	return kQANoErr;
}


int32_t NativeAccessDrawBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->drawBufferAccessed) return kQANoErr;

	// Re-upload modified pixels to FBO color texture
	glBindTexture(GL_TEXTURE_2D, gs->colorTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, priv->width, priv->height,
	                GL_RGBA, GL_UNSIGNED_BYTE, gs->drawBufferCPU);
	gs->drawBufferAccessed = false;
	return kQANoErr;
}


int32_t NativeAccessZBuffer(uint32_t drawContextAddr, uint32_t rectAddr,
                            uint32_t rowBytesPtr, uint32_t bufferPtrPtr)
{
	// GLES 2.0 cannot read the depth buffer via glReadPixels.
	// Return kQAError to signal unsupported.
	return kQAError;
}


int32_t NativeAccessZBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr)
{
	return kQANoErr;
}


/*
 *  Clear methods
 */

int32_t NativeClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQAError;

	int32_t left   = (int32_t)ReadMacInt32(rectAddr + 0);
	int32_t top    = (int32_t)ReadMacInt32(rectAddr + 4);
	int32_t right  = (int32_t)ReadMacInt32(rectAddr + 8);
	int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > priv->width) right = priv->width;
	if (bottom > priv->height) bottom = priv->height;
	if (right <= left || bottom <= top) return kQANoErr;

	float r, g, b;
	if (initialContextAddr != 0) {
		uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx) {
			r = initCtx->state[2].f;
			g = initCtx->state[3].f;
			b = initCtx->state[4].f;
		} else {
			r = priv->state[2].f;
			g = priv->state[3].f;
			b = priv->state[4].f;
		}
	} else {
		r = priv->state[2].f;
		g = priv->state[3].f;
		b = priv->state[4].f;
	}

	glEnable(GL_SCISSOR_TEST);
	glScissor(left, priv->height - bottom, right - left, bottom - top);
	glClearColor(r, g, b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);

	gs->currentProgramIdx = -1;
	return kQANoErr;
}


int32_t NativeClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveGLESState *gs = (RaveGLESState *)priv->metal;
	if (!gs->renderPassActive) return kQAError;

	if (priv->state[28].i == 0) return kQANoErr;

	int32_t left   = (int32_t)ReadMacInt32(rectAddr + 0);
	int32_t top    = (int32_t)ReadMacInt32(rectAddr + 4);
	int32_t right  = (int32_t)ReadMacInt32(rectAddr + 8);
	int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > priv->width) right = priv->width;
	if (bottom > priv->height) bottom = priv->height;
	if (right <= left || bottom <= top) return kQANoErr;

	float clearDepth = 1.0f;
	if (initialContextAddr != 0) {
		uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx) {
			float d = initCtx->state[112].f;
			if (d != 0.0f) clearDepth = d;
		}
	}

	glEnable(GL_SCISSOR_TEST);
	glScissor(left, priv->height - bottom, right - left, bottom - top);
	glDepthMask(GL_TRUE);
	glClearDepthf(clearDepth);
	glClear(GL_DEPTH_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);

	gs->currentProgramIdx = -1;
	return kQANoErr;
}


int32_t NativeSwapBuffers(uint32_t drawContextAddr)
{
	return NativeRenderEnd(drawContextAddr, 0);
}


int32_t NativeBusy(uint32_t drawContextAddr)
{
	return kQANoErr;
}


uint32_t NativeTextureNewFromDrawContext(uint32_t drawContextAddr)
{
	// RTT (render-to-texture) not yet implemented for GLES
	return 0;
}


uint32_t NativeBitmapNewFromDrawContext(uint32_t drawContextAddr)
{
	return 0;
}


/*
 *  ATI clear methods
 */
int32_t NativeATIClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr)
{
	return NativeClearDrawBuffer(drawContextAddr, rectAddr, 0);
}

int32_t NativeATIClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr)
{
	return NativeClearZBuffer(drawContextAddr, rectAddr, 0);
}


/*
 *  Texture management (GLES 2.0)
 */

void *RaveCreateMetalTexture(uint32_t width, uint32_t height, uint32_t mipLevels,
                             const uint8_t *pixelData, uint32_t bytesPerRow)
{
	GLuint texId;
	glGenTextures(1, &texId);
	glBindTexture(GL_TEXTURE_2D, texId);

	// pixelData is already RGBA8 (the Convert*/Expand* helpers emit R,G,B,A
	// byte order), so it uploads directly as GL_RGBA with no swizzle.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, pixelData);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	if (mipLevels > 1) {
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}

	return (void *)(uintptr_t)texId;
}


void RaveUploadMipLevel(void *metalTexture, uint32_t level, uint32_t width, uint32_t height,
                        const uint8_t *pixelData, uint32_t bytesPerRow)
{
	if (!metalTexture) return;
	GLuint texId = (GLuint)(uintptr_t)metalTexture;
	glBindTexture(GL_TEXTURE_2D, texId);
	glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, width, height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, pixelData);
}


void RaveGenerateMipmaps(void *metalTexture)
{
	if (!metalTexture) return;
	GLuint texId = (GLuint)(uintptr_t)metalTexture;
	glBindTexture(GL_TEXTURE_2D, texId);
	glGenerateMipmap(GL_TEXTURE_2D);
}


void RaveReleaseTexture(void *metalTexture)
{
	if (metalTexture) {
		GLuint texId = (GLuint)(uintptr_t)metalTexture;
		glDeleteTextures(1, &texId);
	}
}


/*
 *  Texture refresh from pixmap (same logic as Metal, but uploads via glTexSubImage2D)
 */
void RaveRefreshTextureFromPixmap(RaveResourceEntry *entry)
{
	if (!entry || !entry->metal_texture || entry->pixmap_mac_addr == 0) return;
	if (entry->pixels_copied) return;

	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint32_t pixmap = entry->pixmap_mac_addr;

	uint8_t *expanded = new uint8_t[w * h * 4];

	// Distinguish real (varied) texel data from a uniform white/black
	// placeholder.  Latching pixels_copied on a placeholder is what left menu
	// sprites stuck white, since Bugdom never calls TextureDetach to correct
	// them.  Keep refreshing until the data is varied (or we hit the attempt
	// cap for legitimately solid textures).  Prefer whichever source (live
	// pixmap or the TextureNew snapshot in cpu_pixel_data) holds real data.
	bool nonZero = false, varied = false, usedLivePixmap = true;
	RaveExpandTexturePixels(entry, pixmap, expanded, &nonZero, &varied, &usedLivePixmap);

	// Always upload latest contents. QD3D writes to these buffers
	// between frames; stopping early leaves stale placeholder textures.
	GLuint texId = (GLuint)(uintptr_t)entry->metal_texture;
	glBindTexture(GL_TEXTURE_2D, texId);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, expanded);

	if (nonZero && (varied || entry->refresh_attempts >= RAVE_TEX_LATCH_ATTEMPTS)) {
		entry->pixels_copied = true;
		entry->refresh_attempts = 0;
	} else {
		if (entry->refresh_attempts < 255)
			entry->refresh_attempts++;
	}

	delete[] expanded;
}
