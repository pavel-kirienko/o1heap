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
//
// READ THE DOCUMENTATION IN README.md.

#ifndef O1HEAP_H_INCLUDED
#define O1HEAP_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The definition is private, so the user code can only operate on pointers. This is done to enforce encapsulation.
typedef struct O1HeapInstance O1HeapInstance;

/// A hook function invoked by the allocator. NULL hooks are silently not invoked.
typedef void (*O1HeapHook)(void);

/// The guaranteed alignment depends on the platform and is characterized as follows (the list may not be exhaustive):
///
///      Pointer size   | Alignment of allocated memory | Metadata overhead
///      [bits]         | [bits]                        | [bytes/allocation]
///     ----------------+-------------------------------+--------------------
///      16             | 64                            | 8
///      32             | 128                           | 16
///      64             | 256                           | 32
///
#define O1HEAP_ALIGNMENT (sizeof(void*) * 4U)

/// Runtime diagnostic information. This information can be used to facilitate runtime self-testing,
/// as required by certain safety-critical development guidelines.
/// If assertion checks are not disabled, the library will perform automatic runtime self-diagnostics that trigger
/// an assertion failure if a heap corruption is detected.
/// TODO: INVARIANT CHECKER!
typedef struct
{
    /// The total amount of memory available for serving the allocation requests.
    /// The maximum allocation size is (capacity - O1HEAP_ALIGNMENT).
    /// This parameter does not include the overhead used up by @ref O1HeapInstance and arena alignment.
    /// This parameter is constant.
    size_t capacity;

    /// The amount of memory that is currently allocated, including the per-fragment overhead and size alignment.
    /// For example, if the application requested a fragment of size 1 byte, the value reported here may be 64 bytes.
    size_t allocated;

    /// The maximum value of allocated_bytes seen since initialization. This parameter is never decreased.
    size_t peak_allocated;

    /// The largest amount of memory that the allocator has attempted to allocate (perhaps unsuccessfully)
    /// since initialization (not including the rounding and the allocator's own per-fragment overhead,
    /// so the total is larger). This parameter is never decreased. The initial value is zero.
    size_t peak_request_size;

    /// The number of times an allocation request could not be completed due to the lack of memory or
    /// excessive fragmentation. OOM stands for "out of memory". This parameter is never decreased.
    uint64_t oom_count;
} O1HeapDiagnostics;

/// The arena start pointer and/or its size may be implicitly adjusted to enforce correct alignment.
/// To avoid this, use a large alignment.
///
/// The heap capacity cannot exceed approx. (SIZE_MAX/2); if the arena size allows for a larger heap,
/// the excess will be silently truncated away. This is not a realistic use case because a typical
/// application is unlikely to be able to dedicate that much of the address space for the heap.
///
/// The critical section enter/leave callbacks will be invoked when the allocator performs an atomic transaction.
/// There is exactly one atomic transaction per allocation/deallocation; i.e., each callback is invoked once for
/// allocation and once for deallocation. It is guaranteed that a critical section will never be entered recursively.
/// The callbacks are never invoked from the initialization function itself.
/// Either or both of the callbacks may be NULL if such functionality is not needed.
///
/// The function initializes a new heap instance allocated in the provided arena, taking some of its space for its
/// own needs (normally about 40..600 bytes depending on the architecture, but this parameter is not characterized).
/// A pointer to the newly initialized instance is returned.
///
/// If the provided space is insufficient, or became insufficient after the pointer and size have been aligned,
/// NULL is returned.
///
/// An initialized instance does not hold any resources. Therefore, if the instance is no longer needed,
/// it can be discarded without any de-initialization procedures.
///
/// The time complexity is unspecified.
O1HeapInstance* o1heapInit(void* const      base,
                           const size_t     size,
                           const O1HeapHook critical_section_enter,
                           const O1HeapHook critical_section_leave);

/// The semantics follows malloc() with additional guarantees the full list of which is provided below.
///
/// If the allocation request is served successfully, a pointer to the newly allocated memory fragment is returned.
/// The returned pointer is guaranteed to be aligned at @ref O1HEAP_ALIGNMENT.
///
/// If the allocation request cannot be served due to the lack of memory or its excessive fragmentation,
/// a NULL pointer is returned.
///
/// The function is executed in constant time (unless the critical section management hooks are not constant-time).
/// The allocated memory is NOT zero-filled because zero-filling is a variable-complexity operation.
///
/// The function invokes critical_section_enter and critical_section_leave exactly once each.
/// It is guaranteed that critical_section_enter is invoked before critical_section_leave.
void* o1heapAllocate(O1HeapInstance* const handle, const size_t amount);

/// The semantics follows free() with additional guarantees the full list of which is provided below.
///
/// If the pointer does not point to a previously allocated block, the behavior is undefined.
/// The library contains a set of reasonably reliable yet non-intrusive heuristics that are intended to
/// detect whether the supplied pointer is valid. If the pointer is proven to be invalid,
/// an assertion failure is triggered (unless disabled) and no further actions are performed
/// (i.e., if assertion checks are disabled, passing an invalid pointer is likely to result in a silent no-op).
/// Said invalid pointer detection heuristics are not perfect: a false-negative is possible, in which case
/// a heap corruption may occur. The heuristics are guaranteed to never yield a false-positive (i.e., a valid
/// pointer cannot be rejected). It is expected that the heuristics are sufficiently robust to detect a
/// vast majority of such errors.
///
/// The freed memory will be automatically merged with adjacent free fragments, if any.
///
/// The function is executed in constant time (unless the critical section management hooks are not constant-time).
///
/// The function may invoke critical_section_enter and critical_section_leave at most once each.
/// It is guaranteed that critical_section_enter is invoked before critical_section_leave.
/// It is guaranteed that critical_section_enter is invoked the same number of times as critical_section_leave.
void o1heapFree(O1HeapInstance* const handle, void* const pointer);

/// Returns the diagnostic information, see @ref O1HeapDiagnostics.
/// This function merely copies the structure from an internal storage, so it is fast to return.
/// It invokes critical_section_enter once (unless NULL) and then critical_section_leave once (unless NULL).
/// If the handle pointer is NULL, the behavior is undefined.
O1HeapDiagnostics o1heapGetDiagnostics(const O1HeapInstance* const handle);

#ifdef __cplusplus
}
#endif
#endif  // O1HEAP_H_INCLUDED
