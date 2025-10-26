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
	quicksand_connection *writer = NULL;
	int64_t rate = 1000000; // 1 million msgs/s
	// quicksand_delete("test_pubsub", -1);  // deletes existing (for new size)
	int64_t success = quicksand_connect(&writer, "test_pubsub", -1, 8, rate, NULL);
	assert(success == 0 && writer);

	int32_t data = 0;

	double delay = 1.0 / rate * 1e9;

	while(ok) {
		uint64_t start = quicksand_now();
		int64_t ret = quicksand_write(writer, (uint8_t *) &data, sizeof(data));
		if(ret != 0) {
			continue;
		}
		data = (data + 1) & (32768 - 1); // modulo 32768
		// printf("data %d\n", data);
		quicksand_sleep(delay - quicksand_ns(quicksand_now(), start) - 100.0);
	}

	quicksand_disconnect(&writer, NULL); // is this required for cleanup??
	// quicksand_delete("test_pubsub", -1);
	return 0;
}
