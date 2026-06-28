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

static inline unsigned long get_icache_line_size(void)
{
	static unsigned long cache_line_size = 0;
	if (cache_line_size == 0) {
		unsigned long ctr_el0 = 0;
		asm volatile ("mrs %0, ctr_el0" : "=r"(ctr_el0));
		cache_line_size = 4UL << ((ctr_el0 >> 16) & 0xF);
		if (cache_line_size == 0)
			cache_line_size = 64;
	}
	return cache_line_size;
}

static inline void flush_icache_range(unsigned long start, unsigned long stop)
{
	if (stop <= start)
		return;

	const unsigned long cache_line_size = get_icache_line_size();
	const unsigned long range_start = start & ~(cache_line_size - 1);
	const unsigned long range_stop = (stop + cache_line_size - 1) & ~(cache_line_size - 1);
	unsigned long addr;

	for (addr = range_start; addr < range_stop; addr += cache_line_size)
		asm volatile ("dc cvau, %0" :: "r"(addr));
	asm volatile ("dsb ish" ::: "memory");

	for (addr = range_start; addr < range_stop; addr += cache_line_size)
		asm volatile ("ic ivau, %0" :: "r"(addr));
	asm volatile ("dsb ish" ::: "memory");
	asm volatile ("isb" ::: "memory");
}

#endif /* JIT_TARGET_CACHE_H */
