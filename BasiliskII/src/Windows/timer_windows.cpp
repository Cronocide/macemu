/*
 *  timer_windows.cpp - Time Manager emulation, Windows specific stuff
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#include "main.h"
#include "macos_util.h"
#include "timer.h"

#define DEBUG 0
#include "debug.h"


// Helper time functions
#define MSECS2TICKS(MSECS) (((uint64)(MSECS) * frequency) / 1000)
#define USECS2TICKS(USECS) (((uint64)(USECS) * frequency) / 1000000)
#define TICKS2USECS(TICKS) (((uint64)(TICKS) * 1000000) / frequency)

// From main_windows.cpp
extern HANDLE emul_thread;

// Global variables
static uint64 cpufrequency;
static const uint64 frequency = 10000000; // 100 ns timer ticks

static tm_time_t count_to_ticks(uint64 count)
{
	uint64 seconds = count / cpufrequency;
	uint64 remainder = count % cpufrequency;
	return (tm_time_t)(seconds * frequency +
		(remainder * frequency) / cpufrequency);
}
static tm_time_t qpc_start_ticks;
static tm_time_t qpc_to_unix_ticks;


/*
 *  Initialize native Windows timers
 */

void timer_init(void)
{
	D(bug("SysTimerInit\n"));

	LARGE_INTEGER tt;
	if (!QueryPerformanceFrequency(&tt)) {
		ErrorAlert("No high resolution timers available\n");
		QuitEmulator();
	}
	cpufrequency = (uint64)tt.QuadPart;
	D(bug(" frequency %d\n", frequency));

	// qpc_start_ticks is the 100 ns QPC tick count when Basilisk II starts.
	QueryPerformanceCounter(&tt);
	qpc_start_ticks = count_to_ticks((uint64)tt.QuadPart);

	// Convert QPC ticks to the Unix epoch used by timer_unix.cpp.
	qpc_to_unix_ticks = (tm_time_t)time(NULL) * frequency - qpc_start_ticks;
}


  /*
 *  Return microseconds since boot (64 bit)
 */

void Microseconds(uint32 &hi, uint32 &lo)
{
	D(bug("Microseconds\n"));
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	uint64 elapsed = TICKS2USECS(count_to_ticks((uint64)tt.QuadPart) - qpc_start_ticks);
	hi = (uint32)(elapsed >> 32);
	lo = (uint32)elapsed;
}


/*
 *  Return local date/time in Mac format (seconds since 1.1.1904)
 */

uint32 TimerDateTime(void)
{
	return TimeToMacTime(time(NULL));
}


/*
 *  Get current time
 */

void timer_current_time(tm_time_t &t)
{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	t = count_to_ticks((uint64)tt.QuadPart) + qpc_to_unix_ticks;
}


/*
 *  Add times
 */

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a + b;
}


/*
 *  Subtract times
 */

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a - b;
}


/*
 *  Compare times (<0: a < b, =0: a = b, >0: a > b)
 */

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	tm_time_t r = a - b;
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}


/*
 *  Convert Mac time value (>0: microseconds, <0: microseconds) to tm_time_t
 */

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0) {
		// Time in milliseconds
		res = MSECS2TICKS(mactime);
	} else {
		// Time in negative microseconds
		res = USECS2TICKS(-mactime);
	}
}


/*
 *  Convert positive tm_time_t to Mac time value (>0: microseconds, <0: microseconds)
 *  A negative input value for hosttime results in a zero return value
 *  As long as the microseconds value fits in 32 bit, it must not be converted to milliseconds!
 */

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime < 0)
		return 0;
	else {
		uint64 t = TICKS2USECS(hosttime);
		if (t > 0x7fffffff)
			return int32(t / 1000);	// Time in milliseconds
		else
			return -int32(t);			// Time in negative microseconds
	}
}


/*
 *  Get current value of microsecond timer
 */

uint64 GetTicks_usec(void)
{
	LARGE_INTEGER tt;
	QueryPerformanceCounter(&tt);
	return TICKS2USECS(count_to_ticks((uint64)tt.QuadPart) - qpc_start_ticks);
}


/*
 *  Delay by specified number of microseconds (<1 second)
 */

void Delay_usec(uint32 usec)
{
	const uint64 deadline = GetTicks_usec() + usec;

	for (;;) {
		uint64 now = GetTicks_usec();
		if (now >= deadline)
			return;

		uint64 remaining = deadline - now;
		if (remaining >= 2000)
			Sleep((DWORD)((remaining - 1000) / 1000));
	}
}


/*
 *  Suspend emulator thread, virtual CPU in idle mode
 */

struct idle_sentinel {
	idle_sentinel();
	~idle_sentinel();
};
static idle_sentinel idle_sentinel;

static int idle_sem_ok = -1;
static HANDLE idle_sem = NULL;

static HANDLE idle_lock = NULL;
#define LOCK_IDLE WaitForSingleObject(idle_lock, INFINITE)
#define UNLOCK_IDLE ReleaseMutex(idle_lock)

idle_sentinel::idle_sentinel()
{
	idle_sem_ok = 1;
	if ((idle_sem = CreateSemaphore(NULL, 0, 1, NULL)) == NULL)
		idle_sem_ok = 0;
	if ((idle_lock = CreateMutex(NULL, FALSE, NULL)) == NULL)
		idle_sem_ok = 0;
}

idle_sentinel::~idle_sentinel()
{
	if (idle_lock != NULL)
		CloseHandle(idle_lock);
	if (idle_sem != NULL)
		CloseHandle(idle_sem);
}

void idle_wait(void)
{
	LOCK_IDLE;
	if (idle_sem_ok > 0) {
		idle_sem_ok++;
		UNLOCK_IDLE;
		WaitForSingleObject(idle_sem, INFINITE);
		return;
	}
	UNLOCK_IDLE;

	// Fallback: sleep 10 ms (this should not happen though)
	Delay_usec(10000);
}


/*
 *  Resume execution of emulator thread, events just arrived
 */

void idle_resume(void)
{
	LOCK_IDLE;
	if (idle_sem_ok > 1) {
		idle_sem_ok--;
		UNLOCK_IDLE;
		ReleaseSemaphore(idle_sem, 1, NULL);
		return;
	}
	UNLOCK_IDLE;
}
