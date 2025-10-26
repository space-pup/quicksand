/* ==============================================================
    quicksand_time_windows.c
    Windows‑specific timer helpers for the quicksand API.
    Author: Alec 2025
   ============================================================== */

#if defined(_WIN32) || defined(_WIN64)

#include <Windows.h>
#include <inttypes.h>
#include <stdint.h>

#include "quicksand_style.h"


/* -------------  global calibration state -----------------------------------
 */
static volatile double NS_PER_TICK = 0.0; /* ns per counter tick            */
static volatile double TICK_PER_NS = 0.0; /* counter ticks per ns           */
static volatile uint64_t PERF_FREQ = 0;	  /* counter frequency (ticks/s)    */
static volatile int initialized = 0;	  /* guard for first “now()” call */

/* -------------  helpers -----------------------------------------------------
 */

/* initialise the frequency (called once on first use) */
static void init_perf_freq(void)
{
	if(!initialized) {
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		PERF_FREQ = (uint64_t) freq.QuadPart;
		initialized = 1;
	}
}

/* high‑resolution timestamp (ticks) -------------------------------------------
 */
extern u64 quicksand_now(void);

/* -------------  core public API --------------------------------------------
 */

/* convert counter ticks into nanoseconds */
f64 quicksand_ns(u64 final_tick, u64 init_tick)
{
	if(NS_PER_TICK <= 0.0) {
		quicksand_ns_calibrate(1e6); /* calibrate for 1 ms first */
	}
	return (u64) (((double) (final_tick - init_tick)) * NS_PER_TICK);
}

/* calibrate the conversion factor from ticks → ns
 *   `nanoseconds` = desired sleep / calibration interval.
 */
void quicksand_ns_calibrate(f64 nanoseconds)
{
	/* 1. take a start counter value */
	uint64_t start_tick = quicksand_now();
	LARGE_INTEGER start_time;
	QueryPerformanceCounter(&start_time);

	/* 2. sleep for the requested amount (use real sleep for >=1 ms)   */
	if(nanoseconds >= 1e6) {
		/* Windows sleep granularity is ~1 ms */
		Sleep((DWORD) (nanoseconds / 1e6));
	} else {
		/* busy‑spin short intervals   */
		f64 diff_ns = 0.0;
		LARGE_INTEGER now_time, end_time;
		while(diff_ns < nanoseconds) {
			QueryPerformanceCounter(&now_time);
			diff_ns = ((f64) (now_time.QuadPart - start_time.QuadPart) * 1e9)
					/ (f64) PERF_FREQ;
		}
	}

	/* 3. take an end counter value */
	uint64_t end_tick = quicksand_now();
	LARGE_INTEGER end_time;
	QueryPerformanceCounter(&end_time);

	/* 4. compute elapsed ticks & ms, subtract measurement overhead */
	uint64_t elapsed_ticks = end_tick - start_tick;
	f64 elapsed_ns = ((f64) (end_time.QuadPart - start_time.QuadPart) * 1e9)
			/ (f64) PERF_FREQ;

	/* 5. store conversion factors */
	NS_PER_TICK = elapsed_ns / (f64) elapsed_ticks;
	TICK_PER_NS = (f64) elapsed_ticks / elapsed_ns;
}

/* busy‑or‑system‑sleep delay */
void quicksand_sleep(f64 nanoseconds)
{
	/* For < 100 µs use a tight spin‑lock so the caller gets instant accuracy. */
	if(nanoseconds < 100e3) {
		uint64_t start = quicksand_now();
		uint64_t end =
				start + (uint64_t) (nanoseconds * TICK_PER_NS);
		while(quicksand_now() < end) {
			/* hint to the CPU that we’re in a spin loop */
			// _mm_pause(); // this is slow.
			continue;
		}
	} else {
		/* For longer waits, use the OS sleep routine – this consumes almost no
			CPU cycles and keeps the process nicely schedulable. */
		DWORD ms = (DWORD) (nanoseconds * 1e-6);
		if(ms == 0) {
			ms = 1; /* keep at least 1ms granularity */
		}
		Sleep(ms);
	}
}
