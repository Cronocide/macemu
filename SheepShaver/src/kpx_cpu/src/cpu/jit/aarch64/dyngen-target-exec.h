/*
 *  dyngen defines for micro operation code
 *
 *  Copyright (c) 2003-2004 Fabrice Bellard
 *  Copyright (c) 2026 AArch64 port
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef DYNGEN_TARGET_EXEC_H
#define DYNGEN_TARGET_EXEC_H

/*
 * AArch64 (AAPCS64) callee-saved registers: x19-x28, d8-d15
 * Map to virtual register set used by dyngen micro-ops.
 */
enum {
#define AREG0 "x19"
  AREG0_ID = 19,

#define AREG1 "x20"
  AREG1_ID = 20,

#define AREG2 "x21"
  AREG2_ID = 21,

#define AREG3 "x22"
  AREG3_ID = 22,

#define AREG4 "x23"
  AREG4_ID = 23,

#define AREG5 "x24"
  AREG5_ID = 24,

#define AREG6 "x25"
  AREG6_ID = 25,

#define AREG7 "x26"
  AREG7_ID = 26,

#define AREG8 "x27"
  AREG8_ID = 27,

#define AREG9 "x28"
  AREG9_ID = 28,

  /* callee-saved SIMD/FP registers */
#define FREG0 "d8"
  FREG0_ID = 8,

#define FREG1 "d9"
  FREG1_ID = 9,

#define FREG2 "d10"
  FREG2_ID = 10,

#define FREG3 "d11"
  FREG3_ID = 11,

  /* vector registers aliased to FP registers */
#define VREG0 FREG0
  VREG0_ID = FREG0_ID,

#define VREG1 FREG1
  VREG1_ID = FREG1_ID,

#define VREG2 FREG2
  VREG2_ID = FREG2_ID,

#define VREG3 FREG3
  VREG3_ID = FREG3_ID,
};

#endif /* DYNGEN_TARGET_EXEC_H */
