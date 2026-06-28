/*
 *  gl_thunks_stub.cpp - No-op GLThunksInit for non-OpenGL builds
 *
 *  Phase B uses the RAVE 3D path only; the OpenGL CFM library thunks are
 *  introduced in Phase D.  Until then, thunks.cpp still calls GLThunksInit()
 *  during emulator boot, so we provide a tiny stub that satisfies the linker
 *  without pulling in the rest of the gl_*.cpp / gl_dispatch / state-machine
 *  units (which depend on ~640 GL/AGL/GLU/GLUT TVECT slots and the full
 *  GLContext state machine).
 *
 *  Phase D should remove this file from Makefile.in's GFXACCEL_SRCS in the
 *  same commit that adds gl_thunks.cpp / gl_dispatch.cpp etc.
 */

#include "sysdeps.h"

void GLThunksInit(void)
{
	// Intentionally empty.  No GL TVECTs are allocated, so the guest will
	// never see an OpenGL CFM library injected by us in Phase B; classic
	// Mac OS falls back to its own (software) OpenGL stub library.
}
