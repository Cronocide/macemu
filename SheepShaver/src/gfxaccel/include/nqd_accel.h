/*
 *  nqd_accel.h - GPU acceleration for Native QuickDraw operations (stub)
 *
 *  Ported from PocketShaver (https://github.com/carbjo/PocketShaver)
 *  Original branch: sierra760/gfxaccel
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This stub provides no-op implementations for the aarch64/GLES port.
 *  Metal compute acceleration is not available on this platform.
 */

#ifndef NQD_ACCEL_H
#define NQD_ACCEL_H

#include "sysdeps.h"

static constexpr bool nqd_metal_available = false;

static inline void NQDMetalInit(void) {}
static inline void NQDMetalCleanup(void) {}
static inline void NQDMetalBitblt(uint32) {}
static inline void NQDMetalFillRect(uint32) {}
static inline void NQDMetalInvertRect(uint32) {}
static inline void NQDMetalBltMask(uint32) {}
static inline void NQDMetalFillMask(uint32) {}
static inline bool NQDMetalAddrInBuffer(uint32) { return false; }

#endif /* NQD_ACCEL_H */
