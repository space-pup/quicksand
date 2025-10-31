# Quicksand 🏜️

A multiple-reader multiple-writer communication library enabling robots to think at the speed of silicon!

We have benchmarked processing over 20 million messages per second per process on a on a 2020 laptop processor (i7-1165G7 @ 2.80GHz). This library uses a lock-free shared-memory ring buffer in order to perform inter-process message passing significantly faster than other libraries.

## Usage - Python

Publisher - Python
```Python
import quicksand as qs

qs.delete("hi_topic")
c = qs.connection("hi_topic", 8, 1000000)

while True:
    c.write(b"hello")
```

Subscriber - Python:
```Python
import quicksand as qs

c = qs.connection("hi_topic", -1, -1)
count = 0
start = qs.now()

c.read_latest()
while True:
    for msg in c:
        if msg ==  b"hello":
            count += 1
    dt = qs.ns_elapsed(start)
    if dt > 1e9:
        print(count / (dt * 1e-9), "msgs/s")
        count = 0
        start = qs.now()
```

Output (Python 3.10, Clang 14.0.0, i7-1165G7 @ 2.80GHz):
```
1136568.6862551533 msgs/s
1524979.1982293406 msgs/s
1536257.701024759 msgs/s
1516702.770531038 msgs/s
1517196.7936193675 msgs/s
1500912.66844739 msgs/s
```

## Usage - C

Publisher example - C
```C
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
	int64_t success = quicksand_connect(&writer, "test_pubsub", -1, 8, rate, NULL);
	assert(success == 0 && writer);

	int32_t data = 0;

	double delay = 1.0 / (f64) rate * 1e9;
	while(ok) {
		uint64_t start = quicksand_now();
		int64_t ret = quicksand_write(writer, (uint8_t *) &data, sizeof(data));
		if(ret != 0) {
			continue;
		}
		data = (data + 1) & (32768 - 1); // modulo
		quicksand_sleep(delay - quicksand_ns(quicksand_now(), start));
	}

	quicksand_disconnect(&writer, NULL);
	quicksand_delete("test_pubsub", -1);
	return 0;
}
```


Subscriber example - C
```C
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
		disconnected = quicksand_connect(&reader, "test_pubsub", -1,
				-1, -1, NULL);
	}
	assert(disconnected == 0 && reader);

	uint64_t start = quicksand_now();
	uint64_t count = 0;
	uint64_t skipcount = 0;
	int32_t data = 0;
	int64_t size = sizeof(data);
	int32_t last = 0;
	while(ok) {
		int64_t ret = quicksand_read(reader, (uint8_t *) &data, &size);
		if(ret > -1) {
			count += 1;
			skipcount += (uint64_t) (data != ((last + 1) & (32768 - 1)));
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

	quicksand_disconnect(&reader, NULL);
	return 0;
}
```


Running:

* Terminal 1:
```
make build/test/pub
make build/test/sub
./build/test/sub
```
* Terminal 2:
```
./build/test/pub
```

* Terminal 3:
```
./build/test/pub
```

Output:
```
$ ./build/test/sub
999397.986483 msgs/s (drop: 0.000100 %)
1003744.982127 msgs/s (drop: 0.000000 %)
1005704.982092 msgs/s (drop: 0.000000 %)
1749063.970103 msgs/s (drop: 83.113197 %)
1998851.997210 msgs/s (drop: 97.216402 %)
1998569.991506 msgs/s (drop: 97.370070 %)
```
*(Note: the Drop % calculation assumes one publisher.)*

Removing the sleep from the test_pub.c and running with only one publisher can result in the following messaging rate using Clang 14.0.0 on a 2020 laptop processor (i7-1165G7 @ 2.80GHz):

```
$ ./build/test/sub
18603454.891196 msgs/s (drop: 0.000005 %)
20161391.342656 msgs/s (drop: 0.000000 %)
20459358.354837 msgs/s (drop: 0.000005 %)
19552304.997248 msgs/s (drop: 0.000000 %)
20273548.563203 msgs/s (drop: 0.000000 %)
20429499.443235 msgs/s (drop: 0.000000 %)
```

## Installation

Install library:
```
make
make check
sudo make install
```

Python install (after library install):
```
cd lang/python && pip install .
```
