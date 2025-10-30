// -----------------------------------------------------------------------
// time+windows.c – Windows timing implementation
// -----------------------------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <math.h>

#include "quicksand.h"
#include "quicksand_style.h"

// static calibration value for timer
static volatile f64 NS_PER_TICK = 0.0;
static volatile f64 TICK_PER_NS = 0.0;

extern u64 quicksand_now(void);

f64 quicksand_ns(u64 final_timestamp, u64 initial_timestamp)
{
	if(NS_PER_TICK <= 0.0) {
		quicksand_ns_calibrate(1e6); // delay for 1 millisecond
	}
	f64 dir = 1.0;
	// If overflow, direction is reversed (initial > final)
	if(final_timestamp - initial_timestamp > (u64) 1e15) {
		// (swapping here generates less-insane assembly)
		u64 tmp = initial_timestamp;
		initial_timestamp = final_timestamp;
		final_timestamp = tmp;
		dir = -1.0;
	}
	return (f64) (final_timestamp - initial_timestamp) * NS_PER_TICK * dir;
}


// Calibrate conversion from timestamp counters to nanoseconds
void quicksand_ns_calibrate(f64 nanoseconds)
{
	// Save the start time
	LARGE_INTEGER freq, start_qpc, stop_qpc, stop_qpc_2;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start_qpc);
	u64 start_tick = quicksand_now();

	// Sleep for the calibration time.
	f64 sf = floor(nanoseconds * 1e-9);
	u64 s = (u64) sf;
	u64 ns = (u64) (nanoseconds - sf * 1e9);

	// Convert to milliseconds for Sleep()
	DWORD sleep_ms = (DWORD) (nanoseconds / 1e6);
	if(sleep_ms > 0) {
		Sleep(sleep_ms);
	}

	u64 elapsed_ns;
	do { // Continue looping until desired time elapsed
		QueryPerformanceCounter(&stop_qpc);
		elapsed_ns = (u64) (((stop_qpc.QuadPart - start_qpc.QuadPart) * 1000000000ULL)
				/ freq.QuadPart);
		quicksand_now();
	} while(elapsed_ns < (u64) nanoseconds);

	// Save the stop time
	QueryPerformanceCounter(&stop_qpc);
	u64 end_tick = quicksand_now();
	QueryPerformanceCounter(&stop_qpc_2);

	// Compute time required to do measurement call
	u64 measurement_ns = (u64) (((stop_qpc_2.QuadPart - stop_qpc.QuadPart) * 1000000000ULL)
			/ freq.QuadPart);

	// Compute elapsed time
	u64 elapsed_ticks = end_tick - start_tick;
	elapsed_ns = (u64) (((stop_qpc.QuadPart - start_qpc.QuadPart) * 1000000000ULL)
			/ freq.QuadPart) - measurement_ns / 2;

	// Update the calibration value
	NS_PER_TICK = (f64) elapsed_ns / (f64) elapsed_ticks;
	TICK_PER_NS = (f64) elapsed_ticks / (f64) elapsed_ns;
}

void quicksand_sleep(f64 nanoseconds)
{
	if(nanoseconds < 0.0) {
		return;
	}
	u64 start = quicksand_now();
	u64 end = start + (u64) (TICK_PER_NS * nanoseconds);
	f64 threshold = (100e3);
	u64 near_end = end - (u64) (TICK_PER_NS * threshold);

	if(nanoseconds >= 1e6) { // actually sleep if over 1 ms
		// Sleep using system timer for long durations
		// Compute sleep duration
		LARGE_INTEGER freq, now_qpc;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&now_qpc);

		f64 ns_now = (f64) now_qpc.QuadPart * 1e9 / (f64) freq.QuadPart;
		f64 ns_end = ns_now + nanoseconds - threshold;

		// Convert to milliseconds and sleep
		DWORD sleep_ms = (DWORD) ((ns_end - ns_now) / 1e6);
		if(sleep_ms > 0) {
			Sleep(sleep_ms);
		}
	}

	// Finish sleep using loop.
	while(quicksand_now() < end) {
		if(quicksand_now() < near_end) {
			Sleep(0); // yield to other threads
		} else {
			continue;
		}
	}
}
