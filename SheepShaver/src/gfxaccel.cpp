/*
 *  gfxaccel.cpp - Generic Native QuickDraw acceleration
 *
 *  SheepShaver (C) 1997-2008 Marc Hellwig and Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#include "prefs.h"
#include "video.h"
#include "video_defs.h"

#define DEBUG 0
#include "debug.h"

// 3D acceleration hooks (RAVE engine + OpenGL CFM library).  Forward-declared
// here to avoid pulling gfxaccel/include/rave_engine.h or gl_engine.h into
// this translation unit (which would also drag in their private state structs).
extern void RaveRegisterEngine(void);
extern void GLInstallHooks(void);

// Runtime logging toggles defined in gfxaccel/rave_dispatch.cpp and
// gfxaccel/gl_dispatch.cpp.  When the gfxaccel logging master is compiled
// out (ACCEL_LOGGING_ENABLED=0) these symbols are not provided, so the prefs
// wiring below is gated on the same flag.
#include "accel_logging.h"
#if ACCEL_LOGGING_ENABLED
extern bool rave_logging_enabled;
extern bool gl_logging_enabled;
#endif

/*
 *  NQD blit diagnostics ("nqd_diag" pref, defined in the SDL video backend).
 *  All NQD hooks run on the single emulation thread, so the counters below need
 *  no locking.  We aggregate per-2s windows and emit a [nqd:blit] summary plus a
 *  rate-limited sample of the most recent screen-targeted bitblt's geometry so
 *  the on-screen destination coordinates can be checked against where content
 *  actually appears (e.g. the reported ~10px horizontal shift).
 */
extern bool nqd_diag_enabled;

static uint64 nqd_blit_window_start = 0;
static uint64 nqd_blit_accel = 0;			// bitblts accelerated (srcCopy native path)
static uint64 nqd_blit_reject = 0;			// bitblts rejected (fell back to guest CPU)
static uint64 nqd_blit_to_screen = 0;		// blits whose dest is the live screen
static uint64 nqd_fill_accel = 0;			// fills/inverts accelerated
static uint64 nqd_fill_reject = 0;
static uint64 nqd_unknown = 0;				// ACCL_BLTMASK/FILLMASK/etc (never accelerated)
static uint64 nqd_reject_mode[64] = {0};	// rejected-bitblt count by transfer mode (0..63)
// Last screen-targeted bitblt sample (for geometry inspection)
static int nqd_last_kind = -1;				// 0=accel bitblt, 1=reject bitblt
static int nqd_last_dx=0, nqd_last_dy=0, nqd_last_w=0, nqd_last_h=0;
static int nqd_last_dbnd_l=0, nqd_last_dbnd_t=0, nqd_last_drb=0, nqd_last_mode=0, nqd_last_bpp=0;

static void nqd_blit_diag_report(void)
{
	if (!nqd_diag_enabled)
		return;
	const uint64 now = GetTicks_usec();
	if (nqd_blit_window_start == 0) {
		nqd_blit_window_start = now;
		return;
	}
	const uint64 elapsed = now - nqd_blit_window_start;
	if (elapsed < 2000000)
		return;
	// Build a compact list of the rejected-bitblt transfer modes seen
	char modes[128]; int mp = 0; modes[0] = 0;
	for (int m = 0; m < 64 && mp < (int)sizeof(modes) - 16; m++) {
		if (nqd_reject_mode[m])
			mp += snprintf(modes + mp, sizeof(modes) - mp, "%s%d:%llu",
						   mp ? "," : "", m, (unsigned long long)nqd_reject_mode[m]);
	}
	printf("[nqd:blit] %.2fs | bitblt accel %llu reject %llu (to_screen %llu) | "
		   "fill accel %llu reject %llu | unknown(masked) %llu | reject_modes[%s]\n",
		   elapsed / 1000000.0,
		   (unsigned long long)nqd_blit_accel,
		   (unsigned long long)nqd_blit_reject,
		   (unsigned long long)nqd_blit_to_screen,
		   (unsigned long long)nqd_fill_accel,
		   (unsigned long long)nqd_fill_reject,
		   (unsigned long long)nqd_unknown,
		   modes);
	if (nqd_last_kind >= 0)
		printf("[nqd:blit]   last screen bitblt %s: dst(%d,%d) %dx%d destBounds(l=%d,t=%d) rowBytes=%d mode=%d bpp=%d\n",
			   nqd_last_kind == 0 ? "ACCEL" : "REJECT",
			   nqd_last_dx, nqd_last_dy, nqd_last_w, nqd_last_h,
			   nqd_last_dbnd_l, nqd_last_dbnd_t, nqd_last_drb, nqd_last_mode, nqd_last_bpp);
	fflush(stdout);
	nqd_blit_accel = nqd_blit_reject = nqd_blit_to_screen = 0;
	nqd_fill_accel = nqd_fill_reject = nqd_unknown = 0;
	for (int m = 0; m < 64; m++) nqd_reject_mode[m] = 0;
	nqd_last_kind = -1;
	nqd_blit_window_start = now;
}

// Capture the geometry of a screen-targeted bitblt for the periodic sample line.
static void nqd_blit_diag_sample(uint32 p, int kind)
{
	nqd_last_kind = kind;
	nqd_last_dx = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	nqd_last_dy = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	nqd_last_w  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	nqd_last_h  = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	nqd_last_dbnd_l = (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	nqd_last_dbnd_t = (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	nqd_last_drb = (int32)ReadMacInt32(p + acclDestRowBytes);
	nqd_last_mode = (int32)ReadMacInt32(p + acclTransferMode);
	nqd_last_bpp = (int32)ReadMacInt32(p + acclDestPixelSize);
}

/*
 *  Phase B stub for the RAVE display-clear hook used by NativeHookDrawContextDelete
 *  in gfxaccel/rave_engine.cpp.  In Phase C the compositor-aware video_sdl2.cpp
 *  provides the real (non-weak) definition that drops the RAVE overlay slot
 *  via gles_compositor_clear_overlay; until then a no-op is correct because
 *  no GPU overlay is registered while the renderer is stubbed out.
 */
extern "C" __attribute__((weak)) void video_clear_rave_display(void)
{
}

/*
 *  Phase C weak no-op for the RAVE/GL CPU-readback compositing fallback.
 *  Renderers only call this when they took the readback path; the GLES
 *  compositor's GPU overlay path is primary and skips this.  Per-title
 *  needs (e.g. QuickDraw menus over 3D content) can override it from the
 *  SDL backend with a real RGBA->framebuffer blit.
 */
extern "C" __attribute__((weak)) void video_blit_rave_fbo(const uint8 *,
                                                          int, int,
                                                          int, int)
{
}

/*
 *  Weak no-op for overlay placement registration. The SDL backend can
 *  override this with a real implementation once it tracks RAVE overlay
 *  geometry for framebuffer clear/readback paths.
 */
extern "C" __attribute__((weak)) void video_set_rave_display(unsigned int, int, int, int, int)
{
}


/*
 *	Utility functions
 */

// Return bytes per pixel for requested depth
static inline int bytes_per_pixel(int depth)
{
	int bpp;
	switch (depth) {
	case 8:
		bpp = 1;
		break;
	case 15: case 16:
		bpp = 2;
		break;
	case 24: case 32:
		bpp = 4;
		break;
	default:
		abort();
	}
	return bpp;
}

// Pass-through dirty areas to redraw functions
static inline void NQD_set_dirty_area(uint32 p)
{
	if (ReadMacInt32(p + acclDestBaseAddr) == screen_base) {
		int16 x = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
		int16 y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
		int16 w  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
		int16 h = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
		video_set_dirty_area(x, y, w, h);
	}
}


/*
 *	Rectangle inversion
 */

template< int bpp >
static inline void do_invrect(uint8 *dest, uint32 length)
{
#define INVERT_1(PTR, OFS) ((uint8  *)(PTR))[OFS] = ~((uint8  *)(PTR))[OFS]
#define INVERT_2(PTR, OFS) ((uint16 *)(PTR))[OFS] = ~((uint16 *)(PTR))[OFS]
#define INVERT_4(PTR, OFS) ((uint32 *)(PTR))[OFS] = ~((uint32 *)(PTR))[OFS]
#define INVERT_8(PTR, OFS) ((uint64 *)(PTR))[OFS] = ~((uint64 *)(PTR))[OFS]

#ifndef UNALIGNED_PROFITABLE
	// Align on 16-bit boundaries
	if (bpp < 16 && (((uintptr)dest) & 1)) {
		INVERT_1(dest, 0);
		dest += 1; length -= 1;
	}

	// Align on 32-bit boundaries
	if (bpp < 32 && (((uintptr)dest) & 2) && length >= 2) {
		INVERT_2(dest, 0);
		dest += 2; length -= 2;
	}
#endif

	// Invert 8-byte words
	if (length >= 8) {
		const int r = (length / 8) % 8;
		dest += r * 8;

		int n = ((length / 8) + 7) / 8;
		switch (r) {
		case 0: do {
				dest += 64;
				INVERT_8(dest, -8);
		case 7: INVERT_8(dest, -7);
		case 6: INVERT_8(dest, -6);
		case 5: INVERT_8(dest, -5);
		case 4: INVERT_8(dest, -4);
		case 3: INVERT_8(dest, -3);
		case 2: INVERT_8(dest, -2);
		case 1: INVERT_8(dest, -1);
				} while (--n > 0);
		}
	}

	// 32-bit cell to invert?
	if (length & 4) {
		INVERT_4(dest, 0);
		if (bpp <= 16)
			dest += 4;
	}

	// 16-bit cell to invert?
	if (bpp <= 16 && (length & 2)) {
		INVERT_2(dest, 0);
		if (bpp <= 8)
			dest += 2;
	}

	// 8-bit cell to invert?
	if (bpp <= 8 && (length & 1))
		INVERT_1(dest, 0);

#undef INVERT_1
#undef INVERT_2
#undef INVERT_4
#undef INVERT_8
}

void NQD_invrect(uint32 p)
{
	D(bug("accl_invrect %08x\n", p));

	// Get inversion parameters
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	D(bug(" dest X %d, dest Y %d\n", dest_X, dest_Y));
	D(bug(" width %d, height %d, bytes_per_row %d\n", width, height, (int32)ReadMacInt32(p + acclDestRowBytes)));

	//!!?? pen_mode == 14

	// And perform the inversion
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclDestPixelSize));
	const int dest_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
	uint8 *dest = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dest_row_bytes) + (dest_X * bpp));
	width *= bpp;
	switch (bpp) {
	case 1:
		for (int i = 0; i < height; i++) {
			do_invrect<8>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	case 2:
		for (int i = 0; i < height; i++) {
			do_invrect<16>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	case 4:
		for (int i = 0; i < height; i++) {
			do_invrect<32>(dest, width);
			dest += dest_row_bytes;
		}
		break;
	}
}


/*
 *	Rectangle filling
 */

template< int bpp >
static inline void do_fillrect(uint8 *dest, uint32 color, uint32 length)
{
#define FILL_1(PTR, OFS, VAL) ((uint8  *)(PTR))[OFS] = (VAL)
#define FILL_2(PTR, OFS, VAL) ((uint16 *)(PTR))[OFS] = (VAL)
#define FILL_4(PTR, OFS, VAL) ((uint32 *)(PTR))[OFS] = (VAL)
#define FILL_8(PTR, OFS, VAL) ((uint64 *)(PTR))[OFS] = (VAL)

#ifndef UNALIGNED_PROFITABLE
	// Align on 16-bit boundaries
	if (bpp < 16 && (((uintptr)dest) & 1)) {
		FILL_1(dest, 0, color);
		dest += 1; length -= 1;
	}

	// Align on 32-bit boundaries
	if (bpp < 32 && (((uintptr)dest) & 2) && length >= 2) {
		FILL_2(dest, 0, color);
		dest += 2; length -= 2;
	}
#endif

	// Fill 8-byte words
	if (length >= 8) {
		const uint64 c = (((uint64)color) << 32) | color;
		const int r = (length / 8) % 8;
		dest += r * 8;

		int n = ((length / 8) + 7) / 8;
		switch (r) {
		case 0: do {
				dest += 64;
				FILL_8(dest, -8, c);
		case 7: FILL_8(dest, -7, c);
		case 6: FILL_8(dest, -6, c);
		case 5: FILL_8(dest, -5, c);
		case 4: FILL_8(dest, -4, c);
		case 3: FILL_8(dest, -3, c);
		case 2: FILL_8(dest, -2, c);
		case 1: FILL_8(dest, -1, c);
				} while (--n > 0);
		}
	}

	// 32-bit cell to fill?
	if (length & 4) {
		FILL_4(dest, 0, color);
		if (bpp <= 16)
			dest += 4;
	}

	// 16-bit cell to fill?
	if (bpp <= 16 && (length & 2)) {
		FILL_2(dest, 0, color);
		if (bpp <= 8)
			dest += 2;
	}

	// 8-bit cell to fill?
	if (bpp <= 8 && (length & 1))
		FILL_1(dest, 0, color);

#undef FILL_1
#undef FILL_2
#undef FILL_4
#undef FILL_8
}

void NQD_fillrect(uint32 p)
{
	D(bug("accl_fillrect %08x\n", p));

	// Get filling parameters
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	uint32 color = htonl(ReadMacInt32(p + acclPenMode) == 8 ? ReadMacInt32(p + acclForePen) : ReadMacInt32(p + acclBackPen));
	D(bug(" dest X %d, dest Y %d\n", dest_X, dest_Y));
	D(bug(" width %d, height %d\n", width, height));
	D(bug(" bytes_per_row %d color %08x\n", (int32)ReadMacInt32(p + acclDestRowBytes), color));

	// And perform the fill
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclDestPixelSize));
	const int dest_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
	uint8 *dest = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dest_row_bytes) + (dest_X * bpp));
	width *= bpp;
	switch (bpp) {
	case 1:
		for (int i = 0; i < height; i++) {
			memset(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	case 2:
		for (int i = 0; i < height; i++) {
			do_fillrect<16>(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	case 4:
		for (int i = 0; i < height; i++) {
			do_fillrect<32>(dest, color, width);
			dest += dest_row_bytes;
		}
		break;
	}
}

bool NQD_fillrect_hook(uint32 p)
{
	D(bug("accl_fillrect_hook %08x\n", p));
	NQD_set_dirty_area(p);

	// Check if we can accelerate this fillrect
	if (ReadMacInt32(p + 0x284) != 0 && ReadMacInt32(p + acclDestPixelSize) >= 8) {
		const int transfer_mode = ReadMacInt32(p + acclTransferMode);
		if (transfer_mode == 8) {
			// Fill
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_FILLRECT));
			if (nqd_diag_enabled) { nqd_fill_accel++; nqd_blit_diag_report(); }
			return true;
		}
		else if (transfer_mode == 10) {
			// Invert
			WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_INVRECT));
			if (nqd_diag_enabled) { nqd_fill_accel++; nqd_blit_diag_report(); }
			return true;
		}
	}
	if (nqd_diag_enabled) { nqd_fill_reject++; nqd_blit_diag_report(); }
	return false;
}


/*
 *	Isomorphic rectangle blitting
 */

void NQD_bitblt(uint32 p)
{
	D(bug("accl_bitblt %08x\n", p));

	// Get blitting parameters
	int16 src_X  = (int16)ReadMacInt16(p + acclSrcRect + 2) - (int16)ReadMacInt16(p + acclSrcBoundsRect + 2);
	int16 src_Y  = (int16)ReadMacInt16(p + acclSrcRect + 0) - (int16)ReadMacInt16(p + acclSrcBoundsRect + 0);
	int16 dest_X = (int16)ReadMacInt16(p + acclDestRect + 2) - (int16)ReadMacInt16(p + acclDestBoundsRect + 2);
	int16 dest_Y = (int16)ReadMacInt16(p + acclDestRect + 0) - (int16)ReadMacInt16(p + acclDestBoundsRect + 0);
	int16 width  = (int16)ReadMacInt16(p + acclDestRect + 6) - (int16)ReadMacInt16(p + acclDestRect + 2);
	int16 height = (int16)ReadMacInt16(p + acclDestRect + 4) - (int16)ReadMacInt16(p + acclDestRect + 0);
	D(bug(" src addr %08x, dest addr %08x\n", ReadMacInt32(p + acclSrcBaseAddr), ReadMacInt32(p + acclDestBaseAddr)));
	D(bug(" src X %d, src Y %d, dest X %d, dest Y %d\n", src_X, src_Y, dest_X, dest_Y));
	D(bug(" width %d, height %d\n", width, height));

	// And perform the blit
	const int bpp = bytes_per_pixel(ReadMacInt32(p + acclSrcPixelSize));
	width *= bpp;
	if ((int32)ReadMacInt32(p + acclSrcRowBytes) > 0) {
		const int src_row_bytes = (int32)ReadMacInt32(p + acclSrcRowBytes);
		const int dst_row_bytes = (int32)ReadMacInt32(p + acclDestRowBytes);
		uint8 *src = Mac2HostAddr(ReadMacInt32(p + acclSrcBaseAddr) + (src_Y * src_row_bytes) + (src_X * bpp));
		uint8 *dst = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + (dest_Y * dst_row_bytes) + (dest_X * bpp));
		for (int i = 0; i < height; i++) {
			memmove(dst, src, width);
			src += src_row_bytes;
			dst += dst_row_bytes;
		}
	}
	else {
		const int src_row_bytes = -(int32)ReadMacInt32(p + acclSrcRowBytes);
		const int dst_row_bytes = -(int32)ReadMacInt32(p + acclDestRowBytes);
		uint8 *src = Mac2HostAddr(ReadMacInt32(p + acclSrcBaseAddr) + ((src_Y + height - 1) * src_row_bytes) + (src_X * bpp));
		uint8 *dst = Mac2HostAddr(ReadMacInt32(p + acclDestBaseAddr) + ((dest_Y + height - 1) * dst_row_bytes) + (dest_X * bpp));
		for (int i = height - 1; i >= 0; i--) {
			memmove(dst, src, width);
			src -= src_row_bytes;
			dst -= dst_row_bytes;
		}
	}
}

/*
  BitBlt transfer modes:
  0 : srcCopy
  1 : srcOr
  2 : srcXor
  3 : srcBic
  4 : notSrcCopy
  5 : notSrcOr
  6 : notSrcXor
  7 : notSrcBic
  32 : blend
  33 : addPin
  34 : addOver
  35 : subPin
  36 : transparent
  37 : adMax
  38 : subOver
  39 : adMin
  50 : hilite
*/

bool NQD_bitblt_hook(uint32 p)
{
	D(bug("accl_draw_hook %08x\n", p));
	NQD_set_dirty_area(p);

	const bool to_screen = (ReadMacInt32(p + acclDestBaseAddr) == screen_base);

	// Check if we can accelerate this bitblt
	if (ReadMacInt32(p + 0x018) + ReadMacInt32(p + 0x128) == 0 &&
		ReadMacInt32(p + 0x130) == 0 &&
		ReadMacInt32(p + acclSrcPixelSize) >= 8 &&
		ReadMacInt32(p + acclSrcPixelSize) == ReadMacInt32(p + acclDestPixelSize) &&
		(int32)(ReadMacInt32(p + acclSrcRowBytes) ^ ReadMacInt32(p + acclDestRowBytes)) >= 0 &&	// same sign?
		ReadMacInt32(p + acclTransferMode) == 0 &&												// srcCopy?
		(int32)ReadMacInt32(p + 0x15c) > 0) {

		// Yes, set function pointer
		WriteMacInt32(p + acclDrawProc, NativeTVECT(NATIVE_NQD_BITBLT));
		if (nqd_diag_enabled) {
			nqd_blit_accel++;
			if (to_screen) { nqd_blit_to_screen++; nqd_blit_diag_sample(p, 0); }
			nqd_blit_diag_report();
		}
		return true;
	}
	if (nqd_diag_enabled) {
		nqd_blit_reject++;
		const int mode = (int)ReadMacInt32(p + acclTransferMode);
		if (mode >= 0 && mode < 64) nqd_reject_mode[mode]++;
		if (to_screen) { nqd_blit_to_screen++; nqd_blit_diag_sample(p, 1); }
		nqd_blit_diag_report();
	}
	return false;
}

// Unknown hook
bool NQD_unknown_hook(uint32 arg)
{
	D(bug("accl_unknown_hook %08x\n", arg));
	NQD_set_dirty_area(arg);

	if (nqd_diag_enabled) {
		nqd_unknown++;
		nqd_blit_diag_report();
	}
	return false;
}

// Wait for graphics operation to finish
bool NQD_sync_hook(uint32 arg)
{
	D(bug("accl_sync_hook %08x\n", arg));
	return true;
}


/*
 *	Install Native QuickDraw acceleration hooks
 */

void VideoInstallAccel(void)
{
	// Install acceleration hooks
	if (PrefsFindBool("gfxaccel")) {
		D(bug("Video: Installing acceleration hooks\n"));
		uint32 base;

		SheepVar bitblt_hook_info(sizeof(accl_hook_info));
		base = bitblt_hook_info.addr();
		WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_BITBLT_HOOK));
		WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
		WriteMacInt32(base + 8, ACCL_BITBLT);
		NQDMisc(6, bitblt_hook_info.addr());

		SheepVar fillrect_hook_info(sizeof(accl_hook_info));
		base = fillrect_hook_info.addr();
		WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_FILLRECT_HOOK));
		WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
		WriteMacInt32(base + 8, ACCL_FILLRECT);
		NQDMisc(6, fillrect_hook_info.addr());

		for (int op = 0; op < 8; op++) {
			switch (op) {
			case ACCL_BITBLT:
			case ACCL_FILLRECT:
				continue;
			}
			SheepVar unknown_hook_info(sizeof(accl_hook_info));
			base = unknown_hook_info.addr();
			WriteMacInt32(base + 0, NativeTVECT(NATIVE_NQD_UNKNOWN_HOOK));
			WriteMacInt32(base + 4, NativeTVECT(NATIVE_NQD_SYNC_HOOK));
			WriteMacInt32(base + 8, op);
			NQDMisc(6, unknown_hook_info.addr());
		}
	}

	// Install 3D acceleration (RAVE engine + OpenGL CFM).  Independent of
	// the 2D `gfxaccel` switch above: a user may want software QuickDraw
	// acceleration disabled while still using the GPU 3D path, or vice
	// versa.  RaveRegisterEngine() is re-entrancy-safe and keeps retrying
	// across PatchAfterStartup ticks until the RAVE manager fragment is
	// loaded, so calling it from here is sufficient.
	//
	// Fallback safety: if `gfx_accel` is false, none of the hooks are
	// installed -- the guest's RAVE manager enumeration sees only its
	// built-in software engine, the OpenGL CFM library is left untouched,
	// and the GLES compositor never registers an overlay (so
	// present_sdl_video() takes the pure SDL_Renderer 2D path).
	// Setting `gfx_accel=false` is the documented escape hatch when a
	// title regresses on the 3D path.
	if (PrefsFindBool("gfx_accel")) {
#if ACCEL_LOGGING_ENABLED
		bool log = PrefsFindBool("gfx_accel_log");
		rave_logging_enabled = log;
		gl_logging_enabled = log;
#endif
		D(bug("Video: Registering 3D acceleration engine\n"));
		RaveRegisterEngine();
		// Install OpenGL CFM library symbol-lookup hooks so guest GL calls
		// land in our dispatch.  Idempotent + retry-safe like RaveRegisterEngine,
		// so it's correct to call from this same accRun-driven entry point.
		GLInstallHooks();
	}
}
