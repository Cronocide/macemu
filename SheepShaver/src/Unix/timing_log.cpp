/*
 *  timing_log.cpp - Lightweight timing instrumentation (ndjson), Unix
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1		// for pthread_setaffinity_np / CPU_SET
#endif

#include "sysdeps.h"
#include "timing_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

bool timing_log_active = false;

static FILE *log_file = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64 log_start_nsec = 0;

// Host monotonic clock in nanoseconds. Deliberately independent of the
// emulator's timing surfaces (GetTicks_usec etc.) so the log timestamp stays
// stable regardless of which clock those surfaces are wired to.
static uint64 host_mono_nsec(void)
{
#if defined(CLOCK_MONOTONIC)
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64)t.tv_sec * 1000000000ULL + (uint64)t.tv_nsec;
#else
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return (uint64)t.tv_sec * 1000000000ULL + (uint64)t.tv_nsec;
#endif
}

void timing_log_init(void)
{
	if (timing_log_active)
		return;
	const char *path = getenv("SHEEPSHAVER_TIMING_LOG");
	if (!path || !*path)
		return;
	log_file = fopen(path, "w");
	if (!log_file) {
		fprintf(stderr, "WARNING: cannot open timing log '%s'\n", path);
		return;
	}
	// Line-buffered so partial captures are still useful on a crash.
	setvbuf(log_file, NULL, _IOLBF, 0);
	log_start_nsec = host_mono_nsec();
	timing_log_active = true;
	timing_log_emit("start", NULL);
}

void timing_log_exit(void)
{
	if (!timing_log_active)
		return;
	pthread_mutex_lock(&log_lock);
	timing_log_active = false;
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
	pthread_mutex_unlock(&log_lock);
}

void timing_log_emit(const char *event, const char *fields)
{
	if (!timing_log_active || log_file == NULL)
		return;
	uint64 t_us = (host_mono_nsec() - log_start_nsec) / 1000;
	pthread_mutex_lock(&log_lock);
	if (log_file) {
		if (fields && *fields)
			fprintf(log_file, "{\"t_us\":%llu,\"event\":\"%s\",%s}\n",
				(unsigned long long)t_us, event, fields);
		else
			fprintf(log_file, "{\"t_us\":%llu,\"event\":\"%s\"}\n",
				(unsigned long long)t_us, event);
	}
	pthread_mutex_unlock(&log_lock);
}

void timing_log_tick(int64 expected_us, int64 actual_us)
{
	if (!timing_log_active)
		return;
	char buf[160];
	snprintf(buf, sizeof(buf),
		"\"expected_us\":%lld,\"actual_us\":%lld,\"err_us\":%lld",
		(long long)expected_us, (long long)actual_us,
		(long long)(actual_us - expected_us));
	timing_log_emit("tick", buf);
}

void timing_log_tm(int64 scheduled_us, int64 actual_us)
{
	if (!timing_log_active)
		return;
	char buf[160];
	snprintf(buf, sizeof(buf),
		"\"scheduled_us\":%lld,\"actual_us\":%lld,\"latency_us\":%lld",
		(long long)scheduled_us, (long long)actual_us,
		(long long)(actual_us - scheduled_us));
	timing_log_emit("tm_wakeup", buf);
}

void timing_log_audio(const char *kind, int work_size, int buffer_size, uint64 frames)
{
	if (!timing_log_active)
		return;
	char buf[192];
	snprintf(buf, sizeof(buf),
		"\"kind\":\"%s\",\"work_size\":%d,\"buffer_size\":%d,\"frames\":%llu",
		kind, work_size, buffer_size, (unsigned long long)frames);
	timing_log_emit("audio", buf);
}

void thread_set_realtime(const char *role)
{
	// Build the env var name SHEEPSHAVER_RT_<ROLE>
	char var[64];
	snprintf(var, sizeof(var), "SHEEPSHAVER_RT_%s", role ? role : "");
	for (char *p = var + 14; *p; ++p)
		*p = (char)toupper((unsigned char)*p);

	const char *val = getenv(var);
	if (!val || !*val)
		return;		// disabled by default

	int prio = atoi(val);
	int cpu = -1;
	const char *colon = strchr(val, ':');
	if (colon)
		cpu = atoi(colon + 1);

	if (prio > 0) {
		struct sched_param sp;
		memset(&sp, 0, sizeof(sp));
		sp.sched_priority = prio;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			fprintf(stderr, "WARNING: %s: cannot set SCHED_FIFO prio %d (insufficient privilege?)\n", var, prio);
	}

#if defined(__linux__) && defined(CPU_SETSIZE)
	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
			fprintf(stderr, "WARNING: %s: cannot pin to cpu %d\n", var, cpu);
	}
#endif

	if (timing_log_active) {
		char buf[128];
		snprintf(buf, sizeof(buf), "\"role\":\"%s\",\"prio\":%d,\"cpu\":%d", role, prio, cpu);
		timing_log_emit("rt_setup", buf);
	}
}
