/*
 *  video_profile.h - Lightweight, pref-gated timing instrumentation for the
 *  QuickDraw 2D video present/refresh pipeline.
 *
 *  Enabled at runtime by the "perf_profile" preference. When disabled, every
 *  hook is a single predicted-not-taken branch, so it is safe to leave the
 *  instrumentation compiled into release builds.
 *
 *  The pipeline is split into four phases so we can see, per second, how the
 *  per-frame budget divides between CPU dirty-detection, CPU pixel conversion,
 *  the streaming-texture upload, and the GPU present:
 *
 *    scan    - dirty-region detection (framebuffer memcmp in update_display_*)
 *    convert - format/palette conversion (SDL_BlitSurface / Screen_blit)
 *    upload  - streaming-texture lock + memcpy upload
 *    render  - RenderClear + RenderCopy + RenderPresent (incl. GLES overlay)
 */

#ifndef VIDEO_PROFILE_H
#define VIDEO_PROFILE_H

#include "sysdeps.h"

#ifdef __cplusplus

enum {
	VPROF_SCAN = 0,
	VPROF_CONVERT,
	VPROF_UPLOAD,
	VPROF_RENDER,
	VPROF_PHASE_COUNT
};

// Set once at video init from the "perf_profile" preference.
extern bool video_profile_enabled;

struct VideoProfileCounters {
	uint64 phase_usec[VPROF_PHASE_COUNT];
	uint64 phase_calls[VPROF_PHASE_COUNT];
	uint64 dirty_boxes;
	uint64 dirty_pixels;
	uint64 present_frames;
	uint64 window_start_usec;
};

extern VideoProfileCounters video_profile;

// Accumulate elapsed microseconds into a phase bucket.
static inline void video_profile_add(int phase, uint64 usec)
{
	if (!video_profile_enabled) return;
	video_profile.phase_usec[phase] += usec;
	video_profile.phase_calls[phase]++;
}

// Record that a refresh pass produced a dirty region of the given size.
static inline void video_profile_note_dirty(uint64 boxes, uint64 pixels)
{
	if (!video_profile_enabled) return;
	video_profile.dirty_boxes += boxes;
	video_profile.dirty_pixels += pixels;
}

// Called once per attempted present; prints a rolling summary every ~2 seconds.
extern void video_profile_frame_end(void);

// RAII scope timer: accumulates its lifetime into a phase bucket.
class VideoProfileScope {
	int m_phase;
	uint64 m_start;
public:
	explicit VideoProfileScope(int phase) : m_phase(phase), m_start(0) {
		if (video_profile_enabled) m_start = GetTicks_usec();
	}
	~VideoProfileScope() {
		if (video_profile_enabled)
			video_profile_add(m_phase, GetTicks_usec() - m_start);
	}
};

#endif /* __cplusplus */
#endif /* VIDEO_PROFILE_H */
