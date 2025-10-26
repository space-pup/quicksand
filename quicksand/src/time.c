#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <sched.h>
#include <time.h>

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

	return (final_timestamp - initial_timestamp) * NS_PER_TICK;
}

// Calibrate conversion from timestamp counters to nanoseconds
void quicksand_ns_calibrate(f64 nanoseconds)
{
	// Save the start time
	struct timespec start_ts, stop_ts, stop_ts_2;
	clock_gettime(CLOCK_MONOTONIC, &start_ts);
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
	do { // Continue looping until desired time elapsed
		clock_gettime(CLOCK_MONOTONIC, &stop_ts);
		elapsed_ns = (stop_ts.tv_sec - start_ts.tv_sec) * ((u64) 1e9)
				+ stop_ts.tv_nsec - start_ts.tv_nsec;
		quicksand_now();
	} while(elapsed_ns < (u64) nanoseconds);

	// Save the stop time
	clock_gettime(CLOCK_MONOTONIC, &stop_ts);
	u64 end_tick = quicksand_now();
	clock_gettime(CLOCK_MONOTONIC, &stop_ts_2);

	// Compute time required to do measuement call
	uint64_t measurement_ns =
			(stop_ts_2.tv_sec - stop_ts.tv_sec) * ((u64) 1e9)
			+ stop_ts_2.tv_nsec - stop_ts.tv_nsec;

	// Compute elapsed time
	uint64_t elapsed_ticks = end_tick - start_tick;
	elapsed_ns = (stop_ts.tv_sec - start_ts.tv_sec) * ((u64) 1e9)
			+ stop_ts.tv_nsec - start_ts.tv_nsec
			- measurement_ns / 2;

	// Update the calibration value
	NS_PER_TICK = (double) elapsed_ns / (double) elapsed_ticks;
	TICK_PER_NS = (double) elapsed_ticks / (double) elapsed_ns;
}

void quicksand_sleep(double nanoseconds)
{
	if(nanoseconds < 0.0) {
		return;
	}
	u64 start = quicksand_now();
	u64 end = start + (u64) (TICK_PER_NS * nanoseconds);
	f64 threshold = (16.0 * 1024.0);
	u64 near_end = end - (TICK_PER_NS * threshold);
	if(nanoseconds >= 100e3) { // actually sleep if over 100 microseconds
		// Sleep using system timer for long durations
		// Compute sleep duration
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		f64 ns_now = (f64) now.tv_sec * 1e9 + (f64) now.tv_nsec;
		f64 ns_end = ns_now + nanoseconds - threshold;
		f64 sf = floor(ns_end * 1e-9);
		struct timespec wakeup = {
				.tv_sec = (u64) sf,
				.tv_nsec = (u64) (ns_end - sf * 1e9)};
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, NULL);
	}

	// Finish sleep using loop.
	while(quicksand_now() < end) {
		if(quicksand_now() < near_end) {
			sched_yield(); // this is slow
		} else {
			continue;
		}
	}
}
