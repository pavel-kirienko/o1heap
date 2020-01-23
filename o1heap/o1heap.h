// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
// and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Copyright (c) 2020 UAVCAN Development Team
// Authors: Pavel Kirienko <pavel.kirienko@zubax.com>

#ifndef O1HEAP_H_INCLUDED
#define O1HEAP_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The definition is private, so the user code can only operate on pointers. This is done to enforce encapsulation.
typedef struct O1HeapInstance O1HeapInstance;

/// A hook function invoked by the allocator.
typedef void (*O1HeapHook)(void);

/// The guaranteed alignment is 128 bits for 32-bit platforms, 256 bits for 64-bit platforms.
#define O1HEAP_ALIGNMENT (sizeof(void*) * 4U)

/// The heap space may be implicitly truncated to enforce correct alignment unless it is aligned at O1HEAP_ALIGNMENT.
///
/// The critical section enter/leave callbacks are invoked when the allocator performs an atomic transaction.
/// There is exactly one atomic transaction per allocation/deallocation; i.e., each callback is invoked once for
/// allocation and once for deallocation.
/// Either or both of the callbacks may be NULL if such functionality is not needed.
///
/// @returns A pointer to the heap struct instance or NULL. NULL is returned if the provided space is too small.
O1HeapInstance* o1heapInit(void* const base,
                           const size_t size,
                           const O1HeapHook critical_section_enter,
                           const O1HeapHook critical_section_leave);

/// The semantics follows malloc().
/// The returned memory is guaranteed to be aligned at @ref O1HEAP_ALIGNMENT.
/// The returned memory is NOT zero-filled because zero-filling is a variable-complexity operation.
void* o1heapAllocate(O1HeapInstance* const handle, const size_t amount);

/// The semantics follows free().
void o1heapFree(O1HeapInstance* const handle, void* const pointer);

#ifdef __cplusplus
}
#endif
#endif //O1HEAP_H_INCLUDED
