/*
 *  jit-target-cache.hpp - Target specific code to invalidate cache
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
 *  AArch64 port (C) 2026
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

#ifndef JIT_TARGET_CACHE_H
#define JIT_TARGET_CACHE_H

static inline void flush_icache_range(unsigned long start, unsigned long stop)
{
	unsigned long addr;
	unsigned long ctr_el0;
	unsigned long cache_line_size;

	asm volatile ("mrs %0, ctr_el0" : "=r"(ctr_el0));
	cache_line_size = 4 << ((ctr_el0 >> 16) & 0xF);

	for (addr = start & ~(cache_line_size - 1); addr < stop; addr += cache_line_size)
		asm volatile ("dc cvau, %0" :: "r"(addr));
	asm volatile ("dsb ish" ::: "memory");

	for (addr = start & ~(cache_line_size - 1); addr < stop; addr += cache_line_size)
		asm volatile ("ic ivau, %0" :: "r"(addr));
	asm volatile ("dsb ish" ::: "memory");
	asm volatile ("isb" ::: "memory");
}

#endif /* JIT_TARGET_CACHE_H */
