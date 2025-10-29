# Quicksand 🏜️

A multiple-reader multiple-writer communication library enabling robots to think at the speed of silicon!

We have benchmarked processing over 19 million messages per second per process on a on a 2020 laptop processor (i7-1165G7 @ 2.80GHz). This library uses a lock-free shared-memory ring buffer in order to perform inter-process message passing significantly faster than other libraries.

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

Output:
```
952725.222008827 msgs/s
1479993.9828409045 msgs/s
1465022.6357627842 msgs/s
1443236.1460424745 msgs/s
1438154.2595805922 msgs/s
1445005.8260167285 msgs/s
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

Removing the sleep from the test_pub.c and running with only one publisher can result in the following messaging rate on a 2020 laptop processor (i7-1165G7 @ 2.80GHz):

```
$ ./build/test/sub
19043470.781218 msgs/s (drop: 0.000005 %)
18690428.803813 msgs/s (drop: 0.000000 %)
18506879.815560 msgs/s (drop: 0.000000 %)
18680283.804462 msgs/s (drop: 0.000000 %)
18474634.817623 msgs/s (drop: 0.000000 %)
18760865.799305 msgs/s (drop: 0.000000 %)
18270097.830714 msgs/s (drop: 0.000000 %)
```

## Installation

Install library:
```
make
make check
sudo make install
```

Python install:
```
make python
```
