#ifndef QUICKSAND_H
#define QUICKSAND_H
#ifdef __cplusplus // C++ compatibility
extern "C" {
#endif

#include <stdatomic.h>
#include <stdint.h>

#define CACHE_LINE_SIZE 64

// Quicksand ring buffer data struct
typedef struct {
	uint64_t length;				   // Number of slots
	uint64_t message_size;				   // Size (bytes) of slot
	char pad1[CACHE_LINE_SIZE - 2 * sizeof(int64_t)];  //
	volatile _Atomic(uint64_t) reserve;		   // Writer reserve index
	char pad2[CACHE_LINE_SIZE - sizeof(uint64_t)];	   //
	volatile _Atomic(uint64_t) index;		   // Ring current head
	volatile _Atomic(uint64_t) updatestamp;		   // Last update timestamp
	volatile _Atomic(uint64_t) locked;		   // Write timeout stamp
	char pad3[CACHE_LINE_SIZE - 3 * sizeof(uint64_t)]; //
} quicksand_ringbuffer;
// char data[]  // (DATA STORED IN SHM AFTER BUFFER)

// Quicksand Reader/Writer information struct
typedef struct {
	uint64_t read_stamp;	       // Last read timestamp
	uint64_t read_index;	       // Last read index
	uint64_t shared_memory_handle; // OS Shared memory handle
	uint64_t shared_memory_size;   // Size of shared memory segment
	quicksand_ringbuffer *buffer;  // Mapped ring buffer address
	uint8_t name[256];	       // Shared memory name
} quicksand_connection;

/// Core reading/writing

// Connect to a shared memory ring buffer
// Parameters:
// (OUT) connection: pointer to warren_tunnel object or null.
// topic: shared memory segment name
// topic_length: bytes of message size minus null termination (-1 to use strlen)
// message_size: max size per message (-1 to connect)
// message_rate: max number of messages per second (-1 to connect)
// alloc: custom allocator following malloc(size_t) semantics, or null.
// Returns: 0 if successful or -x for error
int64_t quicksand_connect(quicksand_connection **connection, char *topic,
			  int64_t topic_length, int64_t message_size,
			  int64_t message_rate, void *alloc);

// Disconnect from a ring buffer and free connection memory
// Provide a custom deallocator following free(void*) semantics if required.
void quicksand_disconnect(quicksand_connection **connection, void *dealloc);

// Delete a topic, freeing it to be re-created (affects new connections)
void quicksand_delete(char *topic, int64_t topic_length);

// Write a message of a specified size to the ring buffer
// Parameters:
// connection: the initialized quicksand connection
// message: pointer to message data to write
// message_size: size of data to write.
// Returns 0 if successful or -x for error
int64_t quicksand_write(quicksand_connection *connection, uint8_t *message,
			int64_t message_size);

// Read a message with a max size from the ring buffer
// Parameters:
// connection: the initialized quicksand connection
// message: pointer to buffer to store message
// (IN/OUT) message_size: max size to read, overwritten with bytes read
// Returns: number of messages remaining, -1 for no message read or -1
int64_t quicksand_read(quicksand_connection *connection, uint8_t *message,
		       int64_t *message_size);

/// Timing functions

// Monotonic time stamp counter (rdtsc on x86_64)
// Returns: current CPU timestamp counter
uint64_t quicksand_now();

// Computes elapsed nanoseconds since the start tick
// Parameters:
// stop_stamp: the last timestamp
// start_stamp: the first timestamp
// Returns: number of nanoseconds elapsed between two timestamps
double quicksand_ns(uint64_t final_timestamp, uint64_t initial_timestamp);

// Calibrate the nanosecond timer by sleeping for the specified nanoseconds time
// Parameters:
// nanoseconds: amount of time to sleep while calibrating
void quicksand_ns_calibrate(double nanoseconds);

// Sleep for the specified number of nanoseconds.
// Busy loops for times less than 100 microseconds.
// Parameters:
// nanoseconds: amount of time to sleep or busy-loop
void quicksand_sleep(double nanoseconds);

#ifdef __cplusplus
}
#endif

#endif
