/*
 * Aleph One for the Nintendo 3DS: correct ARM exception-index lookup.
 *
 * devkitARM's 3dsx.ld (r68) defines the unwind table bounds outside the
 * output section:
 *
 *     .ARM.extab : { *(.ARM.extab* ...) } : rodata
 *     __exidx_start = .;
 *     .ARM.exidx : { *(.ARM.exidx* ...) } : rodata
 *     __exidx_end = .;
 *
 * .ARM.exidx is 4-byte aligned, but when .ARM.extab ends on a 2-byte
 * boundary (it does for Aleph One: 0x00742aa8 + 0x1fe96), __exidx_start is
 * 2 bytes below the table. libgcc's unwinder binary-searches 8-byte entries
 * from __exidx_start, finds nothing valid, and every C++ throw ends in
 * std::terminate(), even with a matching catch one frame up. (GNU ld's own
 * scripts set the symbols inside the section, after alignment.)
 *
 * libgcc asks __gnu_Unwind_Find_exidx() for the table when it is defined (a
 * weak reference meant for dynamic loaders), so provide it and return the
 * real start: __exidx_start rounded up to the section's 4-byte alignment.
 * Correct whether or not the script's symbol happens to be aligned.
 * See docs/3ds/PATCHES.md.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include <stdint.h>

typedef uintptr_t _Unwind_Ptr;

extern const char __exidx_start[];
extern const char __exidx_end[];

_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *nrec);

_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *nrec)
{
	(void) pc;   /* one table for the whole (static) program */
	const uintptr_t start = ((uintptr_t) __exidx_start + 3) & ~(uintptr_t) 3;
	const uintptr_t end = (uintptr_t) __exidx_end;
	*nrec = end > start ? (int) ((end - start) / 8) : 0;
	return start;
}
