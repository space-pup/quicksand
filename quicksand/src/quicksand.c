// -------------------------------------------------------------------------
// quicksand.c – implementation of the public API
// -------------------------------------------------------------------------
//
// The implementation follows the algorithm that was used in the old
// “ravenring” code.  The only differences are:
//   •  the names of the structs and functions,
//   •  the way we obtain the allocator (malloc() by default),
//   •  a small amount of defensive checking for the new API.
//
// Everything else (slot reservation, cursor checking, timeout handling,
// padding to cache‑line boundaries, storing the payload size in the first
// eight bytes of a slot) is kept 100% compatible.
// -------------------------------------------------------------------------

#define _POSIX_C_SOURCE 200809L // for shm_open, ftruncate, etc.

#include "quicksand.h"
#include "quicksand_style.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUICKSAND_TIMEOUT 250e6 // nanoseconds

#define DEBUG 1

#if DEBUG
#include <stdio.h>
#endif


#if defined(__GNUC__) || defined(__clang__)
#define RESTRICT __restrict__
#elif defined(_MSC_VER)
#define RESTRICT __restrict
#else
#define RESTRICT
#endif

// memcpy(dest, src, bytes);
static inline void fast_memcpy(u8 *RESTRICT dest, const u8 *RESTRICT src, u64 n)
{
	u64 i = 0;
	for(; i + 64 <= n; i += 64) {
		u64 *dstptr = (u64 *) (dest + i);
		const u64 *srcptr = (const u64 *) (src + i);
		dstptr[0] = srcptr[0];
		dstptr[1] = srcptr[1];
		dstptr[2] = srcptr[2];
		dstptr[3] = srcptr[3];
		dstptr[4] = srcptr[4];
		dstptr[5] = srcptr[5];
		dstptr[6] = srcptr[6];
		dstptr[7] = srcptr[7];
	}
	for(; i + 32 <= n; i += 32) {
		u64 *dstptr = (u64 *) (dest + i);
		const u64 *srcptr = (const u64 *) (src + i);
		dstptr[0] = srcptr[0];
		dstptr[1] = srcptr[1];
		dstptr[2] = srcptr[2];
		dstptr[3] = srcptr[3];
	}
	for(; i + 8 <= n; i += 8) {
		*(u64 *) (dest + i) = *(const u64 *) (src + i);
	}
	for(; i < n; i += 1) {
		dest[i] = src[i];
	}
}

// ---------------------------------------------------------------------
// Helper – round a number up to a multiple of 64 (cache line)
// ---------------------------------------------------------------------
static inline i64 round_to_64(i64 v)
{
	i64 rem = v & 63;
	return (rem == 0) ? v : (v + (64 - rem));
}

// ---------------------------------------------------------------------
// Helper – round a number up to a power of two
// ---------------------------------------------------------------------
static inline i64 round_to_pow2(i64 v)
{
	i64 res = 1;
	while(res < v) {
		res = res << 1;
	}
	return res;
}

// ---------------------------------------------------------------------
// Helper – copy the topic name (null terminated) into the 256‑word
//           array that lives inside quicksand_connection.
// ---------------------------------------------------------------------
static inline void copy_topic_to_name(quicksand_connection *c, const char *topic,
				      i64 topic_len)
{
	memset(c->name, 0, sizeof(c->name));
	size_t copy_len = (size_t) topic_len;
	if(copy_len >= sizeof(c->name)) {
		copy_len = sizeof(c->name) - 1; // truncate
	}

	fast_memcpy(c->name, (u8 *) topic, copy_len);
}

// ---------------------------------------------------------------------
// quicksand_connect – create or attach to an existing shm segment
// ---------------------------------------------------------------------
i64 quicksand_connect(quicksand_connection **out, char *topic,
		      i64 topic_length, i64 message_size,
		      i64 message_rate, void *alloc)
{
	// -----------------------------------------------------------------
	// Parameter validation
	// -----------------------------------------------------------------
	if(!topic) {
		return -EINVAL;
	}
	if(topic_length < -1 || topic_length > 255) {
		return -EINVAL;
	}

	// ---------------------------------------------------------------
	// Allocation – fall back to malloc() if the caller gave us NULL.
	// ---------------------------------------------------------------
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t)) alloc;
	}

	// ---------------------------------------------------------------
	// Build a null‑terminated C string from the caller‑supplied name.
	// ---------------------------------------------------------------
	char name_buf[256] = {0};
	if(topic_length == -1) {
		// -1 means “use strlen()”.  The original code behaved the same.
		snprintf(name_buf, sizeof(name_buf), "%s", topic);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%.*s", (int) topic_length,
			 topic);
	}

	// ---------------------------------------------------------------
	// If message_size == 0 || message_rate == 0 we are only *connecting*
	// to an already existing segment.
	// ---------------------------------------------------------------
	if(message_size <= 0 || message_rate <= 0) {
		int fd = shm_open(name_buf, O_RDWR, 0);
		if(fd == -1) {
			return -ENOENT; // segment does not exist
		}

		struct stat sb;
		if(fstat(fd, &sb) < 0) {
			close(fd);
			return -EIO;
		}
		if(sb.st_size < (off_t) sizeof(quicksand_ringbuffer)) {
			close(fd);
			return -EINVAL; // too small
		}

		void *addr = mmap(NULL, (size_t) sb.st_size,
				  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if(addr == MAP_FAILED) {
			close(fd);
			return -ENOMEM;
		}

		quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;

		// sanity‑check the meta‑data
		if(rb->length > (u64) 1e12 || rb->message_size >= (u64) 1e12) {
			munmap(addr, (size_t) sb.st_size);
			close(fd);
			return -EINVAL;
		}

		// Allocate out if null
		if(!*out) {
			*out = allocate(sizeof(quicksand_connection));
		}
		if(!*out) {
			close(fd);
			shm_unlink(name_buf);
			return -ENOMEM;
		}

		// Fill the user‑supplied connection object
		(*out)->read_stamp = quicksand_now();
		(*out)->read_index = 0;
		(*out)->shared_memory_handle = (u64) fd;
		(*out)->shared_memory_size = (u64) sb.st_size;
		(*out)->buffer = rb;
		copy_topic_to_name(*out, name_buf, (i64) strlen(name_buf));
		return 0;
	}

	// ---------------------------------------------------------------
	// Otherwise we need to create a new ring buffer (or connect to an
	// already existing one).  First try O_EXCL to see if it already
	// exists – if it does, fall back to plain O_RDWR.
	// ---------------------------------------------------------------

	// ---------------------------------------------------------------
	// Compute the total size of the segment.
	//
	//   data_offset = round_up(sizeof(quicksand_ringbuffer)) to 64‑byte
	//   padded_message = round_up(8 + message_size)   // we keep the 8‑byte
	//           // header that stores the actual payload size
	//   payload_area  = padded_message * message_rate   // 1‑second worth
	//   shm_size      = data_offset + payload_area
	// ---------------------------------------------------------------
	i64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));

	// reserve enough space for 1e9 ns (1 second) of messages.
	// padded = [write_timestamp] + [message_len] + [message]
	i64 padded_msg = round_to_64(8 + 8 + message_size);
	i64 ring_length = round_to_pow2(message_rate);
	i64 payload_area = padded_msg * ring_length;
	if(padded_msg < 0 || payload_area < 0) {
		return -EINVAL;
	}

	i64 shm_size = data_offset + payload_area;
	if(shm_size > (i64) INT64_MAX) {
		return -EOVERFLOW;
	}

	int already_exists = 0;
	int fd = shm_open(name_buf, O_EXCL | O_CREAT | O_RDWR,
			  S_IRUSR | S_IWUSR);
	if(fd == -1 && errno == EEXIST) {
		fd = shm_open(name_buf, O_RDWR, 0);
		struct stat sb;
		if(fstat(fd, &sb) < 0) {
			close(fd);
			return -EIO;
		}
		if(sb.st_size != shm_size) { // Abort if size does not match
			close(fd);
			return -EINVAL; // too small
		}
		already_exists = 1;
	}
	if(fd == -1) {
		return -errno; // cannot create/open segment
	}

	// Resize the shm object to the required size
	if(!already_exists) {
		if(ftruncate(fd, (off_t) shm_size) == -1) {
			close(fd);
			shm_unlink(name_buf);
			return -errno;
		}
	}

	void *addr = mmap(NULL, (size_t) shm_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if(addr == MAP_FAILED) {
		close(fd);
		shm_unlink(name_buf);
		return -EINVAL;
	}

	// -----------------------------------------------------------------
	// Initialise the meta‑data.  The original code stored the length as
	// a power‑of‑two (so that modulo can be replaced by & (len‑1)).
	// -----------------------------------------------------------------
	quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;
	if(!already_exists) {
		rb->length = ring_length;
		rb->message_size = padded_msg;
		atomic_store_explicit(&rb->reserve, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->index, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->updatestamp, 0, memory_order_relaxed);
	} else if(rb->length != (u64) ring_length
		  || rb->message_size < (u64) padded_msg) {
		close(fd);
		shm_unlink(name_buf);
		return -EINVAL;
	}

	// Allocate out if null
	if(!*out) {
		*out = allocate(sizeof(quicksand_connection));
	}
	if(!*out) {
		close(fd);
		shm_unlink(name_buf);
		return -ENOMEM;
	}

	// Fill the user‑supplied connection object
	((*out)->read_stamp) = quicksand_now();
	((*out)->read_index) = 0;
	((*out)->shared_memory_handle) = (u64) fd;
	((*out)->shared_memory_size) = (u64) shm_size;
	((*out)->buffer) = rb;
	copy_topic_to_name(*out, name_buf, (i64) strlen(name_buf));

	return 0;
}

// ---------------------------------------------------------------------
// quicksand_disconnect – undo everything done in quicksand_connect
// ---------------------------------------------------------------------
void quicksand_disconnect(quicksand_connection **c, void *dealloc)
{
	if(!(*c)) {
		return;
	}

	if((*c)->shared_memory_handle > 0) {
		// Unmap the segment first
		munmap((void *) (*c)->buffer, (size_t) (*c)->shared_memory_size);
		close((int) (*c)->shared_memory_handle);
		// shm_unlink((char*)(*c)->name); // removes the buffer for future
		// use quicksand_delete(name, namelen) instead
	}

	// free() the struct that the caller allocated (the pointer itself)
	// if they passed a custom deallocator, use it.
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}
	deallocate(*c);
	*c = NULL;
}

void quicksand_delete(char *topic, i64 topic_length)
{
	char name_buf[256] = {0};
	if(topic_length == -1) {
		// -1 means “use strlen()”.  The original code behaved the same.
		snprintf(name_buf, sizeof(name_buf), "%s", topic);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%.*s", (int) topic_length,
			 topic);
	}
	shm_unlink(name_buf);
}

// ---------------------------------------------------------------------
// internal - attempt to un-lock a stalled ringbuffer.
// ---------------------------------------------------------------------
static i64 _quicksand_unlock(quicksand_ringbuffer *ring, u64 locktime)
{
	u64 now = quicksand_now();
	if(!ring) {
		return -1;
	}

	// only if sufficient time passed, attempt unlock.
	if(quicksand_ns(now, locktime) <= QUICKSAND_TIMEOUT) {
		return -2;
	}

	// Return success if ring is already alive.
	if(!ring->locked) {
		return 0;
	}

	u64 current = locktime;
	// attempt to update locktime
	if(!atomic_compare_exchange_strong_explicit(&ring->locked, &current,
						    now, memory_order_relaxed, memory_order_relaxed)) {
		return -2;
	}
	atomic_store_explicit(&ring->updatestamp, now, memory_order_relaxed);
	ring->reserve = ring->index;
	atomic_store_explicit(&ring->locked, 0, memory_order_release);
	return 0;
}


// ---------------------------------------------------------------------
// quicksand_write – put a new payload into the ring buffer
// ---------------------------------------------------------------------
i64 quicksand_write(quicksand_connection *c, u8 *msg, i64 msg_len)
{
	u64 start_time = quicksand_now();
	if(!c || !msg) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;
	if(!rb || rb->length <= 0) {
		return -EPIPE; // uninitialised
	}

	// attempt unlock
	u64 locktime = atomic_load_explicit(&rb->locked, memory_order_relaxed);
	if(rb->locked) {
		_quicksand_unlock(rb, locktime);
		return -ETIMEDOUT;
	}


	if(msg_len < 0 || msg_len > ((i64) rb->message_size) - 16) {
		return -EMSGSIZE; // message does not fit
	}

	// -----------------------------------------------------------------
	// 1. Reserve a slot (atomic fetch‑add)
	// -----------------------------------------------------------------
	u64 my_reserve = atomic_load_explicit(&rb->reserve, memory_order_relaxed);
	while(!atomic_compare_exchange_weak_explicit(&rb->reserve, &my_reserve,
						     my_reserve + 1, memory_order_relaxed, memory_order_relaxed)) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			return -ETIMEDOUT;
		}
	}

	// -----------------------------------------------------------------
	// 2. Block until reserve < 50% away from index
	// -----------------------------------------------------------------
	while(my_reserve - atomic_load_explicit(&rb->index, memory_order_relaxed)
	      > rb->length / 2) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			atomic_store_explicit(&rb->locked, quicksand_now(), memory_order_relaxed);
			return -ETIMEDOUT;
		}
	}

	// -----------------------------------------------------------------
	// 3. Write the message
	// -----------------------------------------------------------------
	u64 slot = my_reserve & (rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_offset + slot * (u64) rb->message_size;

	*((u64 *) slot_ptr) = quicksand_now();
	*((i64 *) (slot_ptr + 8)) = msg_len;
	fast_memcpy(slot_ptr + 16, msg, msg_len);

	// -----------------------------------------------------------------
	// 4. Wait to advance index
	// -----------------------------------------------------------------
	while(my_reserve != atomic_load_explicit(&rb->index, memory_order_relaxed)) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			atomic_store_explicit(&rb->locked, start_time, memory_order_release);
			return -ETIMEDOUT;
		}
	}

	atomic_store_explicit(&rb->updatestamp, quicksand_now(), memory_order_relaxed);
	atomic_store_explicit(&rb->index, my_reserve + 1, memory_order_release);

	return 0;
}

// ---------------------------------------------------------------------
// quicksand_read – fetch the next available payload, if any
// ---------------------------------------------------------------------
i64 quicksand_read(quicksand_connection *c, u8 *msg, i64 *msg_len)
{
	if(!c || !msg || !msg_len) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;

	if(rb->length <= 0) {
		return -EPIPE; // not initialized
	}

	// -----------------------------------------------------------------
	// 1. Load the current write index (the slot that *has* been
	//    committed).  We need an acquire load because the writer
	//    released the store after writing the data.
	// -----------------------------------------------------------------
	u64 write_cursor = atomic_load_explicit(&rb->index,
						memory_order_relaxed);

	// -----------------------------------------------------------------
	// 2. Did we already consume this slot?  The read index is stored in
	//    the connection object.
	// -----------------------------------------------------------------
	// if(write_cursor - c->read_index > (u64) 1e18) {  // Should not happen, reader>writer
	// 	c->read_index = write_cursor;
	// }
	u64 now = quicksand_now();
	if(c->read_index == write_cursor) {
		// printf("read index at limit\n");
		// No new message – consumer is caught up
		c->read_stamp = now;
		return -1; // “0 messages read”
	}

	// -----------------------------------------------------------------
	// 3. The writer may have advanced *many* slots ahead.  If the distance
	//    is larger than half the ring we clamp the “oldest readable” slot
	//    to (write_cursor‑1).
	// -----------------------------------------------------------------
	u64 distance = write_cursor - c->read_index;
	f64 time_delta = quicksand_ns(rb->updatestamp, c->read_stamp);
	if(distance > (rb->length / 2) || (time_delta > QUICKSAND_TIMEOUT && write_cursor > c->read_index)) {
		c->read_index = write_cursor - 1; // skip stale data
	}

	// -----------------------------------------------------------------
	// 4. Compute slot location and update read index
	// -----------------------------------------------------------------
	u64 slot = c->read_index & (rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_offset + slot * (u64) rb->message_size;

	// -----------------------------------------------------------------
	// 5. Advance our local read pointer so the next call reads the next slot.
	// -----------------------------------------------------------------
	c->read_index = c->read_index + 1;
	c->read_stamp = now;

	// -----------------------------------------------------------------
	// 6. Read the timestamp and size that the writer stored at front
	// -----------------------------------------------------------------
	// u64 payload_stamp = *((u64 *) slot_ptr); // unused
	i64 payload_len = *((i64 *) (slot_ptr + 8));
	if(payload_len < 0 || payload_len > ((i64) rb->message_size) - 16) {
		// Corrupted size – treat as no‑data
		return -EBADMSG;
	}

	// -----------------------------------------------------------------
	// 7. Copy to the buffer supplied by the caller.
	// ----------------------------------------------------------------
	if(*msg_len < payload_len) {
		return -EINVAL; // too short
	}

	fast_memcpy(msg, slot_ptr + 16, payload_len);
	*msg_len = payload_len; // tell the caller how many bytes we wrote

	// Return the number of messages still pending after we consumed one.
	return (i64) (write_cursor - c->read_index);
}
