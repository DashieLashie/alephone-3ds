/*
	Copyright (C) 2026 and beyond by the "Aleph One" developers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html
*/

/*
	libsysbase hooks that libctru 2.7.0 does not provide.

	devkitARM's libsysbase implements pthread_key_create/pthread_setspecific/
	pthread_getspecific/pthread_key_delete by calling the weak hooks
	__syscall_tls_{create,set,get,delete}; libctru defines the thread hooks
	but not these, so every pthread key call failed with ENOSYS. That made
	OpenAL Soft's alcCreateContext() always fail (al::Context holds a tss<>)
	and left libstdc++'s exception globals shared by all threads.

	sched_yield() is a weak libsysbase stub that only returns ENOSYS, so
	std::this_thread::yield() did not yield.

	See docs/3ds/PATCHES.md.
*/

#include <3ds.h>

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>

// Keys are (generation << 8) | slot. Deleting a key bumps the slot's
// generation, so a re-created key reads as NULL in every thread (POSIX)
// even though stale per-thread values for the old generation remain.
#define A1_TLS_MAX_KEYS 64

static uint32_t tls_used[A1_TLS_MAX_KEYS];
static uint32_t tls_generation[A1_TLS_MAX_KEYS];

static __thread struct {
	uint32_t generation;
	const void* value;
} tls_slots[A1_TLS_MAX_KEYS];

static int tls_slot(uint32_t key, uint32_t* slot)
{
	*slot = key & 0xff;
	return *slot < A1_TLS_MAX_KEYS
		&& __atomic_load_n(&tls_used[*slot], __ATOMIC_ACQUIRE)
		&& (__atomic_load_n(&tls_generation[*slot], __ATOMIC_ACQUIRE) & 0xffffff) == key >> 8;
}

// Destructors are not supported: libctru has no thread-exit hook to run
// them, so per-thread values are simply abandoned when a thread ends (e.g.
// libstdc++'s 12-byte per-thread exception globals).
int __syscall_tls_create(uint32_t* key, void (*destructor)(void*))
{
	(void) destructor;
	for (uint32_t i = 0; i < A1_TLS_MAX_KEYS; i++)
	{
		if (__atomic_exchange_n(&tls_used[i], 1, __ATOMIC_ACQ_REL) == 0)
		{
			uint32_t generation = __atomic_add_fetch(&tls_generation[i], 1, __ATOMIC_ACQ_REL) & 0xffffff;
			*key = (generation << 8) | i;
			return 0;
		}
	}
	return EAGAIN;
}

int __syscall_tls_set(uint32_t key, const void* value)
{
	uint32_t slot;
	if (!tls_slot(key, &slot))
		return EINVAL;
	tls_slots[slot].generation = key >> 8;
	tls_slots[slot].value = value;
	return 0;
}

void* __syscall_tls_get(uint32_t key)
{
	uint32_t slot;
	if (!tls_slot(key, &slot) || tls_slots[slot].generation != key >> 8)
		return NULL;
	return (void*) tls_slots[slot].value;
}

int __syscall_tls_delete(uint32_t key)
{
	uint32_t slot;
	if (!tls_slot(key, &slot))
		return EINVAL;
	__atomic_add_fetch(&tls_generation[slot], 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&tls_used[slot], 0, __ATOMIC_RELEASE);
	return 0;
}

// A minimal sleep forces a reschedule; SDL2's N3DS port yields the same way
// (src/thread/n3ds/SDL_syssem.c). What Horizon does with a 0 ns sleep is not
// documented, so 0 is not used.
int sched_yield(void)
{
	svcSleepThread(1);
	return 0;
}
