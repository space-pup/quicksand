// -----------------------------------------------------------------------
// quicksand_win.c – Windows version of the QuickSand API
// -----------------------------------------------------------------------
// This file implements the same semantics as the POSIX version, but
// uses Win32 shared‑memory objects (CreateFileMapping / MapViewOfFile)
// and the Win32 interlocked primitives.
//
// Build command (example):
//   cl /nologo /EHsc /MD /std:c11 /D_CRT_SECURE_NO_WARNINGS quicksand_win.c
// -----------------------------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS // silence strcpy/strncpy warnings
#define _WIN32_WINNT 0x0600	// at least Windows Vista for Interlocked*64

#include "quicksand.h"
#include "quicksand_style.h"

#include <intrin.h> // InterlockedCompareExchange
#include <math.h>   // ceil, log2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define QUICKSAND_TIME 500e3 // NANOSECONDS
// --------------------------------------------------------------------
// Utility – round a value up to a multiple of 64 (cache‑line size)
// --------------------------------------------------------------------
static inline i64 round_to_64(i64 v)
{
	i64 rem = v & 63;
	return (rem == 0) ? v : (v + (64 - rem));
}

// --------------------------------------------------------------------
// Utility – copy the topic into the name[] array of the connection.
// The API stores the name as an array of u64; we simply copy the
// raw bytes and zero‑pad the rest.
// --------------------------------------------------------------------
static void copy_topic_to_name(quicksand_connection *c, const char *topic,
			       i64 topic_len)
{
	memset(c->name, 0, sizeof(c->name));
	size_t copy_len = (size_t) topic_len;
	if(copy_len > sizeof(c->name) * sizeof(u64)) {
		copy_len = sizeof(c->name) * sizeof(u64);
	}
	memcpy(c->name, topic, copy_len);
}

// --------------------------------------------------------------------
// quicksand_connect – create or attach to a named shared‑memory segment
// --------------------------------------------------------------------
i64 quicksand_connect(quicksand_connection *out, char *topic,
		      i64 topic_length, i64 message_size,
		      i64 message_rate, void *(*alloc)(size_t))
{
	if(!topic) {
		return -EINVAL;
	}
	if(topic_length < -1 || topic_length > 255) {
		return -EINVAL;
	}

	//  -------------------------------------------------------------
	// Choose allocator – fall back to the CRT malloc()
	// -------------------------------------------------------------
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = alloc;
	}

	if(!out) {
		out = allocate(sizeof(quicksand_connection));
	}

	if(!out) {
		return -ENOMEM;
	}

	//  -------------------------------------------------------------
	// Build a null‑terminated name string (Windows object names are
	// Unicode, but the ANSI version works fine for ASCII names)
	// -------------------------------------------------------------
	char name_buf[256] = {0};
	if(topic_length == -1) {
		snprintf(name_buf, sizeof(name_buf), "%s", topic);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%.*s", (int) topic_length,
			 topic);
	}

	// -------------------------------------------------------------
	// 1. ATTACH‑ONLY mode (message_size==-1 || message_rate==-1)
	// -------------------------------------------------------------
	if(message_size <= 0 || message_rate <= 0) {
		HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
					       name_buf);
		if(!hMap) {
			return -ENOENT; // not existing
		}

		// Query the size of the existing mapping
		MEMORY_BASIC_INFORMATION info;
		if(VirtualQueryEx(GetCurrentProcess(), NULL, &info,
				  sizeof(info))
		   == 0) {
			CloseHandle(hMap);
			return -EIO;
		}

		// Map the whole region
		void *addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
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

		out->read_stamp = 0;
		out->read_index = 0;
		out->shared_memory_handle = (u64) hMap;
		out->shared_memory_size =
				(u64) 0; // not needed on Windows
		out->buffer = rb;
		copy_topic_to_name(out, name_buf, (i64) strlen(name_buf));
		return 0;
	}

	// -------------------------------------------------------------
	// 2. CREATE / OPEN mode (full parameters supplied)
	// -------------------------------------------------------------
	// Compute the total memory size the same way the POSIX version does
	i64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	i64 padded_msg = round_to_64(8 + message_size); // 8‑byte size
	i64 payload_area = round_to_64(padded_msg * message_rate);
	if(padded_msg < 0 || payload_area < 0) {
		return -EINVAL;
	}
	i64 shm_size = data_offset + payload_area;

	// Try to create the mapping – if it already exists we simply open it
	HANDLE hMap = CreateFileMappingA(
			INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
			(DWORD) (shm_size >> 32),
			(DWORD) (shm_size & 0xffffffffULL), name_buf);
	if(!hMap) {
		return -GetLastError(); /* creation failed */
	}

	// If the mapping already existed we didn't initialise it – map it first
	// to see whether it contains a valid header.
	void *addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if(!addr) {
		CloseHandle(hMap);
		return -ENOMEM;
	}

	quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;

	// If this is a brand‑new segment we need to fill the header. The
	// original code used a power‑of‑two length, so we mimic that here.
	if(rb->length == 0 && rb->message_size == 0) {
		// Round length up to the next power of two (same as the old
		// Windows code did)
		i64 log2_len = (i64) ceil(log2((double) message_rate));
		i64 ring_len = (i64) 1 << log2_len;
		while(ring_len < message_rate) {
			++log2_len;
			ring_len = (i64) 1 << log2_len;
		}

		rb->length = ring_len;	       // power‑of‑two
		rb->message_size = padded_msg; // already cache‑line aligned
		atomic_store_explicit(&rb->reserve, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->index, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->updatestamp, 0,
				      memory_order_relaxed);
	} else {
		/* Existing segment – sanity‑check the values */
		if(rb->length <= 0 || rb->message_size <= 0) {
			UnmapViewOfFile(addr);
			CloseHandle(hMap);
			return -EINVAL;
		}
	}

	out->read_stamp = 0;
	out->read_index = 0;
	out->shared_memory_handle = (u64) hMap;
	out->shared_memory_size = (u64) shm_size;
	out->buffer = rb;
	copy_topic_to_name(out, name_buf, (i64) strlen(name_buf));
	return 0;
}

// --------------------------------------------------------------------
// quicksand_disconnect – unmap and close the Windows object
// --------------------------------------------------------------------
void quicksand_disconnect(quicksand_connection *c, void (*dealloc)(void *))
{
	if(!c) {
		return;
	}

	if(c->shared_memory_handle) {
		UnmapViewOfFile((LPCVOID) c->buffer);
		CloseHandle((HANDLE) c->shared_memory_handle);
	}

	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = dealloc;
	}
	if(c) {
		deallocate(c);
		c = NULL;
	}
}

// --------------------------------------------------------------------
// quicksand_write – reserve a slot, copy data and publish it
// --------------------------------------------------------------------
i64 quicksand_write(quicksand_connection *c, u8 *msg, i64 msg_len)
{
	if(!c || !msg) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;
	if(rb->length <= 0) {
		return -EPIPE;
	}

	if(msg_len < 0 || msg_len > rb->message_size - 8) {
		return -EMSGSIZE;
	}

	//  -------------------------------------------------------------
	// 1. Reserve a slot (CAS loop). We use InterlockedCompareExchange64
	//     which is the Windows analogue of the C11 atomic compare‑exchange.
	// -------------------------------------------------------------
	u64 my_reserve;
	while(1) {
		u64 current = atomic_load_explicit(&rb->reserve,
						   memory_order_relaxed);
		if(InterlockedCompareExchange64(
				   (volatile LONG64 *) &rb->reserve,
				   (i64) (current + 1), (i64) current)
		   == (i64) current) {
			my_reserve = current;
			break; /* success */
		}
		/* else someone else beat us – retry */
	}

	//  -------------------------------------------------------------
	// 2. Wait until we are not more than half‑ring ahead of the
	//     reader (same rule the Unix version uses).
	// -------------------------------------------------------------
	u64 idx;
	const u64 half_len = (u64) rb->length / 2;
	do {
		idx = atomic_load_explicit(&rb->index, memory_order_acquire);
		if(my_reserve <= idx + half_len) {
			break;
		}
		/* tiny sleep to avoid a tight spin */
		Sleep(0);
	} while(1);

	//  -------------------------------------------------------------
	// 3. Write the payload at the computed offset.
	// -------------------------------------------------------------
	u64 slot = my_reserve & ((u64) rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_offset + slot * (u64) rb->message_size;

	/* store the size header (first 8 bytes) */
	*((i64 *) slot_ptr) = msg_len;
	/* store the payload */
	memcpy(slot_ptr + 8, msg, (size_t) msg_len);

	//  -------------------------------------------------------------
	// 4. Publish the new index (release store so the data is visible
	//     before the reader loads the index)
	// -------------------------------------------------------------
	do { // Wait for index to advance.
		idx = atomic_load_explicit(&rb->index, memory_order_acquire);
		if(my_reserve == idx) {
			break;
		}
		/* tiny sleep */
		quicksand_sleep(5e3);
	} while(1);
	atomic_store_explicit(&rb->index, my_reserve + 1, memory_order_release);
	atomic_store_explicit(&rb->updatestamp, quicksand_now(),
			      memory_order_relaxed);
	return 0;
}

// --------------------------------------------------------------------
// quicksand_read – fetch the next message, if one exists
// --------------------------------------------------------------------
i64 quicksand_read(quicksand_connection *c, u8 *msg, i64 *msg_len)
{
	if(!c || !msg || !msg_len) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;
	if(rb->length <= 0) {
		return -EPIPE;
	}

	//  -------------------------------------------------------------
	// 1. Load the writer’s index (acquire) – this also guarantees
	//     that the payload written before the index store is visible.
	// -------------------------------------------------------------
	u64 write_idx = atomic_load_explicit(&rb->index, memory_order_acquire);
	if(write_idx == 0) {
		return -ENODATA; /* nothing ever written */
	}

	//  -------------------------------------------------------------
	// 2. Have we already consumed this slot?
	// -------------------------------------------------------------
	if(c->read_index == write_idx) {
		return 0; /* caught‑up – 0 pending */
	}

	//  -------------------------------------------------------------
	// 3. If we are too far behind (more than half the ring),
	//     advance to the last written slot (drop old data).
	// -------------------------------------------------------------
	u64 distance = write_idx - c->read_index;
	f64 time_delta = quicksand_ns(rb->updatestamp, c->read_stamp);
	if(distance > (u64) rb->length / 2 || (time_delta > QUICKSAND_TIMEOUT && write_cursor > c->read_index)) {
		c->read_index = write_idx - 1; /* skip stale data */
	}

	//  -------------------------------------------------------------
	// 4. Compute where the slot lives.
	// -------------------------------------------------------------
	u64 slot = c->read_index & ((u64) rb->length - 1);
	u8 *base = (u8 *) rb;
	u64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	u8 *slot_ptr = base + data_offset + slot * (u64) rb->message_size;

	//  -------------------------------------------------------------
	// 5. Read the size stored by the writer.
	// -------------------------------------------------------------
	i64 payload_len = *((i64 *) slot_ptr);
	if(payload_len < 0 || payload_len > rb->message_size - 8) {
		return -EBADMSG; /* corrupted size */
	}

	//  -------------------------------------------------------------
	// 6. Copy to the user buffer, respecting the caller's maximum.
	// -------------------------------------------------------------
	if(*msg_len < payload_len) {
		return -EINVAL; // too short
	}
	i64 copy_len = (*msg_len < payload_len) ? *msg_len : payload_len;
	memcpy(msg, slot_ptr + 8, (size_t) copy_len);
	*msg_len = copy_len; // tell the caller how many bytes we wrote

	//  -------------------------------------------------------------
	// 7. Advance our local read cursor and return how many
	//     messages are still pending after we consumed one.
	// -------------------------------------------------------------
	c->read_index = c->read_index + 1;
	c->read_stamp = quicksand_now();
	return (i64) (write_idx - c->read_index);
}
