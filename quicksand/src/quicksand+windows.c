// -----------------------------------------------------------------------
// quicksand_win.c – Windows implementation of the QuickSand API
// -----------------------------------------------------------------------
// This file is a 1‑to‑1 port of the Unix implementation, using
// Win32 shared‑memory objects (CreateFileMapping / MapViewOfFile) and
// the Win32 interlocked primitives.  All algorithmic improvements made
// in the Unix version (timeout handling, lock‑field, power‑of‑two ring,
// back‑pressure, unlock on timeout, etc.) are present here.
//
// Build example (Visual C):
//   cl /nologo /EHsc /MD /std:c11 /D_CRT_SECURE_NO_WARNINGS quicksand_win.c
// -----------------------------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600 // Vista+ for Interlocked*64

#include "quicksand.h"
#include "quicksand_style.h"

#include <intrin.h> // Interlocked*64
#include <math.h>   // ceil, log2
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define QUICKSAND_TIMEOUT 250e6 // nanoseconds – same value as Unix

// --------------------------------------------------------------------
// Helper – round up to a multiple of 64 (cache‑line size)
// --------------------------------------------------------------------
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

// --------------------------------------------------------------------
// Helper – copy the topic name into the u8 name[] of connection
// --------------------------------------------------------------------
static void copy_topic_to_name(quicksand_connection *c,
			       const char *topic,
			       i64 topic_len)
{
	memset(c->name, 0, sizeof(c->name));
	size_t copy_len = (size_t) topic_len;
	if(copy_len > sizeof(c->name)) {
		copy_len = sizeof(c->name);
	}
	memcpy(c->name, topic, copy_len);
}

/* -------------------------------------------------------------------
   quicksand_connect – create or attach to a shared‑memory segment
   ------------------------------------------------------------------- */
i64 quicksand_connect(quicksand_connection **out_ptr,
		      char *topic,
		      i64 topic_length,
		      i64 message_size,
		      i64 message_rate,
		      void *alloc)
{
	if(!topic) {
		return -EINVAL;
	}
	if(topic_length < -1 || topic_length > 255) {
		return -EINVAL;
	}

	/* -------------------------------------------------------------
       Choose allocator – default to malloc()
       ------------------------------------------------------------- */
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t)) alloc;
	}

	/* -------------------------------------------------------------
       Build a null‑terminated name string
       ------------------------------------------------------------- */
	char name_buf[256] = {0};
	if(topic_length == -1) {
		snprintf(name_buf, sizeof(name_buf), "%s", topic);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%.*s",
			 (int) topic_length, topic);
	}

	/* -------------------------------------------------------------
       ATTACH‑ONLY mode (message_size <= 0 || message_rate <= 0)
       ------------------------------------------------------------- */
	if(message_size <= 0 || message_rate <= 0) {
		HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS,
					       FALSE, name_buf);
		if(!hMap) {
			return -ENOENT; // segment does not exist
		}

		/* Map the whole region (size is stored in the header) */
		void *addr = MapViewOfFile(hMap,
					   FILE_MAP_ALL_ACCESS,
					   0, 0, 0);
		if(!addr) {
			CloseHandle(hMap);
			return -ENOMEM;
		}

		quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;
		if(rb->length <= 0 || rb->message_size <= 0) {
			UnmapViewOfFile(addr);
			CloseHandle(hMap);
			return -EINVAL;
		}

		/* Allocate the connection structure if needed */
		if(!*out_ptr) {
			*out_ptr = allocate(sizeof(quicksand_connection));
		}
		if(!*out_ptr) {
			UnmapViewOfFile(addr);
			CloseHandle(hMap);
			return -ENOMEM;
		}

		(*out_ptr)->read_stamp = 0;
		(*out_ptr)->read_index = 0;
		(*out_ptr)->shared_memory_handle = (u64) (uintptr_t) hMap;
		(*out_ptr)->shared_memory_size = 0; // not required on Win32
		(*out_ptr)->buffer = rb;
		copy_topic_to_name(*out_ptr, name_buf,
				   (i64) strlen(name_buf));
		return 0;
	}

	/* -------------------------------------------------------------
       CREATE / OPEN mode – calculate required segment size
       ------------------------------------------------------------- */
	i64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	i64 padded_msg = round_to_64(8 + message_size); // 8‑byte size header
	i64 ring_length = round_to_pow2(message_rate);
	i64 payload_area = padded_msg * ring_length;
	if(padded_msg < 0 || payload_area < 0) {
		return -EINVAL;
	}

	i64 shm_size = data_offset + payload_area;
	if(shm_size > (i64) INT64_MAX) {
		return -EOVERFLOW;
	}

	/* -------------------------------------------------------------
       Try to create a new map; if it already exists just open it
       ------------------------------------------------------------- */
	HANDLE hMap = CreateFileMappingA(
			INVALID_HANDLE_VALUE,
			NULL,
			PAGE_READWRITE,
			(DWORD) (shm_size >> 32),
			(DWORD) (shm_size & 0xffffffffULL),
			name_buf);
	if(!hMap) {
		return -GetLastError();
	}

	/* Map the region */
	void *addr = MapViewOfFile(hMap,
				   FILE_MAP_ALL_ACCESS,
				   0, 0, 0);
	if(!addr) {
		CloseHandle(hMap);
		return -ENOMEM;
	}

	quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;

	/* -------------------------------------------------------------
       Initialise header if we are the creator (length==0 && msg_sz==0)
       ------------------------------------------------------------- */
	if(rb->length == 0 && rb->message_size == 0) {
		/* ring length must be a power‑of‑two */
		rb->length = ring_length; // power‑of‑two
		rb->message_size = padded_msg;
		atomic_store_explicit(&rb->reserve, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->index, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->updatestamp, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->locked, 0, memory_order_relaxed);
	} else {
		/* sanity‑check existing header */
		if(rb->length != ring_length || rb->message_size < padded_msg) {
			UnmapViewOfFile(addr);
			CloseHandle(hMap);
			return -EINVAL;
		}
	}

	/* Allocate and fill the connection structure */
	if(!*out_ptr) {
		*out_ptr = allocate(sizeof(quicksand_connection));
	}
	if(!*out_ptr) {
		UnmapViewOfFile(addr);
		CloseHandle(hMap);
		return -ENOMEM;
	}

	(*out_ptr)->read_stamp = 0;
	(*out_ptr)->read_index = 0;
	(*out_ptr)->shared_memory_handle = (u64) (uintptr_t) hMap;
	(*out_ptr)->shared_memory_size = (u64) shm_size;
	(*out_ptr)->buffer = rb;
	copy_topic_to_name(*out_ptr, name_buf,
			   (i64) strlen(name_buf));
	return 0;
}

/* -------------------------------------------------------------------
   quicksand_disconnect – unmap and close the shared‑memory object
   ------------------------------------------------------------------- */
void quicksand_disconnect(quicksand_connection **c_ptr, void *dealloc)
{
	if(!c_ptr || !*c_ptr) {
		return;
	}

	quicksand_connection *c = *c_ptr;

	if(c->shared_memory_handle) {
		UnmapViewOfFile((LPCVOID) c->buffer);
		CloseHandle((HANDLE) (uintptr_t) c->shared_memory_handle);
	}

	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}
	deallocate(c);
	*c_ptr = NULL;
}

/* -------------------------------------------------------------------
   quicksand_delete – remove a named mapping from the system
   ------------------------------------------------------------------- */
void quicksand_delete(char *topic, i64 topic_length)
{
	char name_buf[256] = {0};
	if(topic_length == -1) {
		snprintf(name_buf, sizeof(name_buf), "%s", topic);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%.*s",
			 (int) topic_length, topic);
	}
	/* Note: CloseHandle is not required; just remove the name */
	CloseHandle(OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_buf));
	// For a clean removal we call the Win32 “named object” deletion API:
	// (When all handles are closed the object vanishes.)
}

/* -------------------------------------------------------------------
   Internal – unlock a timed‑out writer (mirrors the Unix version)
   ------------------------------------------------------------------- */
static i64 _quicksand_unlock(quicksand_ringbuffer *ring, u64 locktime)
{
	u64 now = quicksand_now();
	if(!ring) {
		return -1;
	}

	if(quicksand_ns(now, locktime) <= QUICKSAND_TIMEOUT / 2) {
		return -2; // not enough time elapsed
	}

	// Try to clear the lock atomically
	u64 expected = locktime;
	if(!InterlockedCompareExchange64(
			   (volatile LONG64 *) &ring->locked,
			   0, (LONG64) expected)
	   == (LONG64) expected) {
		// Successful unlock – bring reserve forward to index to unblock writers
		atomic_store_explicit(&ring->reserve,
				      atomic_load_explicit(&ring->index,
							   memory_order_acquire),
				      memory_order_release);
		atomic_store_explicit(&ring->updatestamp, now,
				      memory_order_release);
		return 0;
	}
	return -2;
}

/* -------------------------------------------------------------------
   quicksand_write – reserve a slot, copy data, publish it
   ------------------------------------------------------------------- */
i64 quicksand_write(quicksand_connection *c, u8 *msg, i64 msg_len)
{
	u64 start = quicksand_now();
	if(!c || !msg) {
		return -EINVAL;
	}

	quicksand_ringbuffer *rb = c->buffer;
	if(!rb || rb->length <= 0) {
		return -EPIPE;
	}

	/* ----------------------------------------------------------------
       Check the lock field – if another writer timed‑out we attempt an
       unlock before failing.
       ---------------------------------------------------------------- */
	u64 lock_val = atomic_load_explicit(&rb->locked,
					    memory_order_relaxed);
	if(lock_val) {
		_quicksand_unlock(rb, lock_val);
		return -ETIMEDOUT;
	}

	if(msg_len < 0 || msg_len > ((i64) rb->message_size) - 8) {
		return -EMSGSIZE;
	}

	/* -------------------------------------------------------------
       1. Reserve a slot (CAS loop)
       ------------------------------------------------------------- */
	u64 my_reserve;
	while(1) {
		u64 cur = atomic_load_explicit(&rb->reserve,
					       memory_order_relaxed);
		if(InterlockedCompareExchange64(
				   (volatile LONG64 *) &rb->reserve,
				   (i64) (cur + 1), (i64) cur)
		   == (i64) cur) {
			my_reserve = cur;
			break;
		}
		if(quicksand_ns(quicksand_now(), start) > QUICKSAND_TIMEOUT) {
			atomic_store_explicit(&rb->locked, start,
					      memory_order_relaxed);
			return -ETIMEDOUT;
		}
	}

	/* -------------------------------------------------------------
       2. Back‑pressure: wait until we are not more than half‑ring
          ahead of the consumer.
       ------------------------------------------------------------- */
	const u64 half_len = rb->length / 2;
	u64 idx;
	while(1) {
		idx = atomic_load_explicit(&rb->index,
					   memory_order_acquire);
		if(my_reserve - idx <= half_len) {
			break;
		}
		if(quicksand_ns(quicksand_now(), start) > QUICKSAND_TIMEOUT) {
			atomic_store_explicit(&rb->locked, start,
					      memory_order_relaxed);
			return -ETIMEDOUT;
		}
		Sleep(0); // tiny yield
	}

	/* -------------------------------------------------------------
       3. Write the payload (size header + data)
       ------------------------------------------------------------- */
	u64 slot = my_reserve & (rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_off = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_off + slot * (u64) rb->message_size;

	*((i64 *) slot_ptr) = msg_len;		     // size header
	memcpy(slot_ptr + 8, msg, (size_t) msg_len); // payload

	/* -------------------------------------------------------------
       4. Publish the new index (release store)
       ------------------------------------------------------------- */
	atomic_store_explicit(&rb->updatestamp, quicksand_now(),
			      memory_order_relaxed);
	atomic_store_explicit(&rb->index, my_reserve + 1,
			      memory_order_release);
	return 0;
}

/* -------------------------------------------------------------------
   quicksand_read – fetch the next available message
   ------------------------------------------------------------------- */
i64 quicksand_read(quicksand_connection *c, u8 *msg, i64 *msg_len)
{
	if(!c || !msg || !msg_len) {
		return -EINVAL;
	}

	quicksand_ringbuffer *rb = c->buffer;
	if(!rb || rb->length <= 0) {
		return -EPIPE;
	}

	/* -------------------------------------------------------------
       1. Load the writer’s index (acquire) – guarantees payload is visible.
       ------------------------------------------------------------- */
	u64 write_idx = atomic_load_explicit(&rb->index,
					     memory_order_acquire);
	if(write_idx == 0) {
		return -ENODATA; // nothing ever written
	}

	/* -------------------------------------------------------------
       2. Have we already consumed up to this point?
       ------------------------------------------------------------- */
	if(c->read_index >= write_idx) {
		c->read_index = write_idx;
		c->read_stamp = quicksand_now();
		return -1; // no new message
	}

	/* -------------------------------------------------------------
       3. Drop stale data if we fell behind > half‑ring or timeout.
       ------------------------------------------------------------- */
	u64 distance = write_idx - c->read_index;
	f64 delta_ns = quicksand_ns(rb->updatestamp, c->read_stamp);
	if(distance > rb->length / 2 || (delta_ns > QUICKSAND_TIMEOUT && write_idx > c->read_index)) {
		c->read_index = write_idx - 1; // skip oldest slot
	}

	/* -------------------------------------------------------------
       4. Locate the slot in memory.
       ------------------------------------------------------------- */
	u64 slot = c->read_index & (rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_off = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_off + slot * (u64) rb->message_size;

	/* -------------------------------------------------------------
       5. Retrieve the stored payload length.
       ------------------------------------------------------------- */
	i64 payload_len = *((i64 *) slot_ptr);
	if(payload_len < 0 || payload_len > ((i64) rb->message_size) - 8) {
		return -EBADMSG; // corrupted size header
	}

	/* -------------------------------------------------------------
       6. Copy into caller’s buffer.
       ------------------------------------------------------------- */
	if(*msg_len < payload_len) {
		return -EINVAL; // caller buffer too small
	}
	memcpy(msg, slot_ptr + 8, (size_t) payload_len);
	*msg_len = payload_len;

	/* -------------------------------------------------------------
       7. Advance our local read pointer.
       ------------------------------------------------------------- */
	c->read_index += 1;
	c->read_stamp = quicksand_now();

	/* Return number of messages still pending */
	return (i64) (write_idx - c->read_index);
}
