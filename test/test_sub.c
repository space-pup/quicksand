#include <assert.h>
#include <signal.h>
#include <stdio.h>

#include "quicksand.h"

static volatile int ok = 1;

void interrupt()
{
	ok = 0;
}

int main()
{
	signal(SIGINT, interrupt);
	quicksand_connection *reader = NULL;
	int64_t disconnected = 1;
	while(ok && disconnected) {
		disconnected = quicksand_connect(&reader, "test_pubsub", -1, -1, -1, NULL);
	}
	assert(disconnected == 0 && reader);

	uint64_t start = quicksand_now();
	uint64_t count = 0;
	uint64_t skipcount = 0;
	int32_t data = 0;
	int64_t size = sizeof(data);
	int32_t last = 0;
	quicksand_read(reader, (uint8_t *) &data, &size);
	while(ok) {
		int64_t ret = quicksand_read(reader, (uint8_t *) &data, &size);
		if(ret > -1) {
			count += 1;
			skipcount += (uint64_t) ((data != ((last + 1) & (32768 - 1))) & 1);
			last = data;
		}

		double ns = quicksand_ns(quicksand_now(), start);
		if(ns > 1e9) {
			printf("%f msgs/s (drop: %f %%)\n",
			       (double) count / (ns * 1e-9),
			       (double) skipcount / (double) count * 100.0);
			count = 0;
			skipcount = 0;
			start = quicksand_now();
		}
	}

	quicksand_disconnect(&reader, NULL); // is this required for cleanup??
	return 0;
}
