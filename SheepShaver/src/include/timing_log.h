/*
 *  timing_log.h - Lightweight timing instrumentation (ndjson)
 *
 *  SheepShaver (C) Christian Bauer and Marc Hellwig
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

#ifndef TIMING_LOG_H
#define TIMING_LOG_H

// Optional timing instrumentation that writes newline-delimited JSON to the
// file named by the SHEEPSHAVER_TIMING_LOG environment variable. When the
// variable is unset (the default), timing_log_active stays false and every
// entry point below short-circuits to a cheap no-op, so the instrumentation
// has no measurable cost in normal runs.

extern bool timing_log_active;

// Open the log file if SHEEPSHAVER_TIMING_LOG is set. Idempotent.
extern void timing_log_init(void);

// Flush and close the log file.
extern void timing_log_exit(void);

// Emit a generic record. "fields" is a JSON fragment of comma-separated
// key/value pairs without surrounding braces, or NULL. A monotonic host
// timestamp (t_us) is always prepended.
extern void timing_log_emit(const char *event, const char *fields);

// 60Hz tick interval sample (host microseconds).
extern void timing_log_tick(int64 expected_us, int64 actual_us);

// Time Manager wakeup latency sample (host microseconds).
extern void timing_log_tm(int64 scheduled_us, int64 actual_us);

// Audio stream buffer event ("kind" is e.g. "data", "underrun"). "frames" is
// the current value of the media clock (total frames played) for drift analysis.
extern void timing_log_audio(const char *kind, int work_size, int buffer_size, uint64 frames);

// Apply real-time scheduling/affinity to the calling thread, gated by the
// environment variable SHEEPSHAVER_RT_<ROLE> (ROLE upper-cased), whose value is
// "prio[:cpu]" (e.g. "10:3" => SCHED_FIFO priority 10, pinned to CPU 3). When
// the variable is unset this is a no-op, so the default behavior is unchanged.
extern void thread_set_realtime(const char *role);

#endif
