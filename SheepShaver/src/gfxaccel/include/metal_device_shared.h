/*
 *  metal_device_shared.h - Shared GPU device interface (stub for GLES port)
 *
 *  Ported from PocketShaver (https://github.com/carbjo/PocketShaver)
 *  Original branch: sierra760/gfxaccel
 *  Original Metal implementation: (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Stub: returns NULL on aarch64/GLES. Phase 2+ will provide
 *  EGL/SDL2 context management instead.
 */

#ifndef METAL_DEVICE_SHARED_H
#define METAL_DEVICE_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

static inline void *SharedMetalDevice(void) { return 0; }
static inline void *SharedMetalCommandQueue(void) { return 0; }

#ifdef __cplusplus
}
#endif

#endif /* METAL_DEVICE_SHARED_H */
