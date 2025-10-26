# Quicksand 🏜️

A multiple-reader multiple-writer communication library enabling robots to think at the speed of silicon!

We have benchmarked up to four million messages per second per process on a relatively recent laptop (11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz). This library uses a lock-free shared-memory ring buffer in order to perform inter-process message passing significantly faster than other libraries.

## Usage - Python

Publisher - Python

```Python
import quicksand as qs

c = qs.connection("hi_topic", 10, 1000000)

while True:
    c.write(b"hello")
```

Subscriber - Python
```Python
import quicksand as qs

c = qs.connection("hi_topic", -1, -1)
count = 0
start = qs.now()

while True:
    if c.read(16) is not None:
        count += 1
    dt = qs.ns(qs.now(), start)
    if dt > 1e9:
        print(count / (dt*1e-9), "msgs/s")
        count = 0
        start = qs.now()
```

Output:
```
483931.71820689563 msgs/s
483507.35573312046 msgs/s
486753.9641683966 msgs/s
492463.97348016815 msgs/s
...
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
2265555.587733 msgs/s (drop: 0.001677 %)
2249827.963072 msgs/s (drop: 0.001422 %)
2217960.594811 msgs/s (drop: 0.001668 %)
2285704.625659 msgs/s (drop: 0.001356 %)
2250935.179984 msgs/s (drop: 0.001777 %)
2286291.857240 msgs/s (drop: 0.001575 %)
2227035.111612 msgs/s (drop: 0.001482 %)
1882225.905305 msgs/s (drop: 0.001488 %)
2227245.665432 msgs/s (drop: 0.001661 %)
2270321.570664 msgs/s (drop: 0.001762 %)
2202301.696682 msgs/s (drop: 0.002407 %)
2282561.673411 msgs/s (drop: 0.001796 %)
2042535.652465 msgs/s (drop: 0.002154 %)
1921274.373430 msgs/s (drop: 0.024047 %)
```

## Installation

```
make
make check
sudo make install
```

Python install
```
cd python
pip install .
```
