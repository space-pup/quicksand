// -----------------------------------------------------------------------
// time+darwin.c – macOS timing implementation
// -----------------------------------------------------------------------

#define _POSIX_C_SOURCE 200809L
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

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
	u64 start_mach = mach_absolute_time();
	u64 start_tick = quicksand_now();

	// Sleep for the calibration time.
	f64 sf = floor(nanoseconds * 1e-9);
	u64 s = (u64) sf;
	u64 ns = (u64) (nanoseconds - sf * 1e9);
	struct timespec sleep_dt = {
			.tv_sec = s,
			.tv_nsec = ns};
	nanosleep(&sleep_dt, NULL);

	u64 elapsed_ns;
	mach_timebase_info_data_t timebase;
	mach_timebase_info(&timebase);

	do { // Continue looping until desired time elapsed
		u64 now_mach = mach_absolute_time();
		elapsed_ns = (now_mach - start_mach) * timebase.numer / timebase.denom;
		quicksand_now();
	} while(elapsed_ns < (u64) nanoseconds);

	// Save the stop time
	u64 stop_mach = mach_absolute_time();
	u64 end_tick = quicksand_now();
	u64 stop_mach_2 = mach_absolute_time();

	// Compute time required to do measurement call
	u64 measurement_ns = (stop_mach_2 - stop_mach) * timebase.numer / timebase.denom;

	// Compute elapsed time
	u64 elapsed_ticks = end_tick - start_tick;
	elapsed_ns = (stop_mach - start_mach) * timebase.numer / timebase.denom
			- measurement_ns / 2;

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
	f64 threshold = (16.0 * 1024.0);
	u64 near_end = end - (u64) (TICK_PER_NS * threshold);

	if(nanoseconds >= 100e3) { // actually sleep if over 100 microseconds
		// Sleep using system timer for long durations
		// Compute sleep duration (use relative time since TIMER_ABSTIME unavailable)
		f64 sleep_ns = nanoseconds - threshold;
		if(sleep_ns > 0.0) {
			f64 sf = floor(sleep_ns * 1e-9);
			struct timespec sleep_dt = {
					.tv_sec = (u64) sf,
					.tv_nsec = (u64) (sleep_ns - sf * 1e9)};
			nanosleep(&sleep_dt, NULL);
		}
	}

	// Finish sleep using loop.
	while(quicksand_now() < end) {
		if(quicksand_now() < near_end) {
			pthread_yield_np(); // macOS equivalent of sched_yield
		} else {
			continue;
		}
	}
}
