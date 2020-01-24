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

#include "o1heap.h"
#include <assert.h>

// ---------------------------------------- BUILD CONFIGURATION OPTIONS ----------------------------------------

/// The assertion macro defaults to the standard assert().
/// It can be overridden to manually suppress assertion checks or use a different error handling policy.
#ifndef O1HEAP_ASSERT
// Intentional violation of MISRA: the assertion check macro cannot be replaced with a function definition.
#    define O1HEAP_ASSERT(x) assert(x)  // NOSONAR
#endif

/// Branch probability annotations are used to improve the worst case execution time (WCET). They are entirely optional.
/// A stock implementation is provided for GCC/Clang; for other compilers it defaults to nothing.
/// If you are using a different compiler, consider overriding this value.
#ifndef O1HEAP_LIKELY
#    if defined(__GNUC__) || defined(__clang__)
// Intentional violation of MISRA: branch hinting macro cannot be replaced with a function definition.
#        define O1HEAP_LIKELY(x) __builtin_expect((x), 1)  // NOSONAR
#    else
#        define O1HEAP_LIKELY(x) x
#    endif
#endif

/// This option is used for testing only. Do not use in production.
#if defined(O1HEAP_EXPOSE_INTERNALS) && O1HEAP_EXPOSE_INTERNALS
#    define O1HEAP_PRIVATE
#else
#    define O1HEAP_PRIVATE static inline
#endif

// ---------------------------------------- INTERNAL DEFINITIONS ----------------------------------------

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#    error "Unsupported language: ISO C99 or a newer version is required."
#endif

#if __STDC_VERSION__ < 201112L
// Intentional violation of MISRA: static assertion macro cannot be replaced with a function definition.
#    define static_assert(x, ...) typedef char _static_assert_gl(_static_assertion_, __LINE__)[(x) ? 1 : -1]  // NOSONAR
#    define _static_assert_gl(a, b) _static_assert_gl_impl(a, b)                                              // NOSONAR
// Intentional violation of MISRA: the paste operator ## cannot be avoided in this context.
#    define _static_assert_gl_impl(a, b) a##b  // NOSONAR
#endif

#define SMALLEST_FRAGMENT_SIZE (O1HEAP_ALIGNMENT * 2U)

/// Subtraction of the pointer size is a very basic heuristic needed to reduce the number of unnecessary bins.
/// Normally we should subtract log2(SMALLEST_BIN_CAPACITY) but log2 is bulky to compute using the preprocessor only.
#define NUM_BINS_MAX (sizeof(size_t) * 8U - sizeof(void*))

static_assert((O1HEAP_ALIGNMENT & (O1HEAP_ALIGNMENT - 1U)) == 0U, "The alignment shall be an integer power of 2");
static_assert((SMALLEST_FRAGMENT_SIZE & (SMALLEST_FRAGMENT_SIZE - 1U)) == 0U,
              "The smallest fragment size shall be an integer power of 2");

typedef struct Fragment Fragment;

typedef struct FragmentHeader
{
    struct Fragment* next;
    struct Fragment* prev;
    size_t           size;
    bool             used;
} FragmentHeader;
static_assert(sizeof(FragmentHeader) <= O1HEAP_ALIGNMENT, "Memory layout error");

struct Fragment
{
    FragmentHeader header;
    // Everything past the header may spill over into the allocatable space. The header survives across alloc/free.
    struct Fragment* next_free;
};
static_assert(sizeof(Fragment) <= SMALLEST_FRAGMENT_SIZE, "Memory layout error");

struct O1HeapInstance
{
    Fragment* bins[NUM_BINS_MAX];  ///< Smallest fragments are in the bin at index 0.
    size_t    nonempty_bin_mask;   ///< Bit 1 represents a non-empty bin; bit 0 for smallest bins.

    O1HeapHook critical_section_enter;
    O1HeapHook critical_section_leave;

    O1HeapDiagnostics diagnostics;
};

/// True if the argument is an integer power of two or zero.
O1HEAP_PRIVATE bool isPowerOf2(const size_t x);
O1HEAP_PRIVATE bool isPowerOf2(const size_t x)
{
    return (x & (x - 1U)) == 0U;
}

/// Special case: if the argument is zero, returns zero.
O1HEAP_PRIVATE uint8_t log2Floor(const size_t x);
O1HEAP_PRIVATE uint8_t log2Floor(const size_t x)
{
    size_t  tmp = x;
    uint8_t y   = 0;
    while (tmp > 1U)
    {
        tmp >>= 1U;
        y++;
    }
    return y;
}

/// Special case: if the argument is zero, returns zero.
O1HEAP_PRIVATE uint8_t log2Ceil(const size_t x);
O1HEAP_PRIVATE uint8_t log2Ceil(const size_t x)
{
    return (uint8_t)(log2Floor(x) + (isPowerOf2(x) ? 0U : 1U));
}

O1HEAP_PRIVATE uint8_t computeBinIndex(const size_t fragment_size);
O1HEAP_PRIVATE uint8_t computeBinIndex(const size_t fragment_size)
{
    O1HEAP_ASSERT(fragment_size >= SMALLEST_FRAGMENT_SIZE);
    O1HEAP_ASSERT(fragment_size % SMALLEST_FRAGMENT_SIZE == 0U);
    const uint8_t lg = log2Ceil(fragment_size / SMALLEST_FRAGMENT_SIZE);
    O1HEAP_ASSERT(lg < NUM_BINS_MAX);
    return lg;
}

/// Raise 2 into the specified power.
O1HEAP_PRIVATE size_t pow2(const uint8_t power);
O1HEAP_PRIVATE size_t pow2(const uint8_t power)
{
    return ((size_t) 1U) << power;
}

O1HEAP_PRIVATE void invokeHook(const O1HeapHook hook);
O1HEAP_PRIVATE void invokeHook(const O1HeapHook hook)
{
    if (O1HEAP_LIKELY(hook != NULL))
    {
        hook();
    }
}

// ---------------------------------------- PUBLIC API IMPLEMENTATION ----------------------------------------

O1HeapInstance* o1heapInit(void* const      base,
                           const size_t     size,
                           const O1HeapHook critical_section_enter,
                           const O1HeapHook critical_section_leave)
{
    // Align the arena pointer.
    uint8_t* adjusted_base = (uint8_t*) base;
    size_t   adjusted_size = size;
    while (((((size_t) adjusted_base) % O1HEAP_ALIGNMENT) != 0U) && (adjusted_size > 0U) && (adjusted_base != NULL))
    {
        adjusted_base++;
        O1HEAP_ASSERT(adjusted_size > 0U);
        adjusted_size--;
    }

    O1HeapInstance* out = NULL;
    if ((adjusted_base != NULL) &&
        (adjusted_size >= (sizeof(O1HeapInstance) + SMALLEST_FRAGMENT_SIZE + (O1HEAP_ALIGNMENT * 2U))))
    {
        // Allocate the heap metadata structure in the beginning of the arena.
        O1HEAP_ASSERT(((size_t) adjusted_base) % sizeof(O1HeapInstance*) == 0U);
        out = (O1HeapInstance*) (void*) adjusted_base;
        adjusted_base += sizeof(O1HeapInstance);
        adjusted_size -= sizeof(O1HeapInstance);
        out->critical_section_enter = critical_section_enter;
        out->critical_section_leave = critical_section_leave;

        // Align the start of the storage.
        while ((((size_t) adjusted_base) % O1HEAP_ALIGNMENT) != 0U)
        {
            adjusted_base++;
            O1HEAP_ASSERT(adjusted_size > 0U);
            adjusted_size--;
        }

        // Align the size; i.e., truncate the end.
        while ((adjusted_size % SMALLEST_FRAGMENT_SIZE) != 0)
        {
            O1HEAP_ASSERT(adjusted_size > 0U);
            adjusted_size--;
        }

        O1HEAP_ASSERT((adjusted_size % SMALLEST_FRAGMENT_SIZE) == 0);
        O1HEAP_ASSERT(adjusted_size >= SMALLEST_FRAGMENT_SIZE);
        for (size_t i = 0; i < NUM_BINS_MAX; i++)
        {
            out->bins[i] = NULL;
        }

        O1HEAP_ASSERT((((size_t) adjusted_base) % O1HEAP_ALIGNMENT) == 0U);
        O1HEAP_ASSERT((((size_t) adjusted_base) % sizeof(Fragment*)) == 0U);
        Fragment* const root_bin = (Fragment*) (void*) adjusted_base;
        root_bin->header.next    = NULL;
        root_bin->header.prev    = NULL;
        root_bin->header.size    = adjusted_size;
        root_bin->header.used    = false;
        root_bin->next_free      = NULL;

        const uint8_t bin_index = computeBinIndex(adjusted_size);
        O1HEAP_ASSERT(bin_index < NUM_BINS_MAX);
        out->bins[bin_index]   = root_bin;
        out->nonempty_bin_mask = 1U << bin_index;

        O1HEAP_ASSERT(out->nonempty_bin_mask != 0U);

        // Initialize the diagnostics.
        out->diagnostics.capacity          = adjusted_size;
        out->diagnostics.allocated         = 0U;
        out->diagnostics.peak_allocated    = 0U;
        out->diagnostics.peak_request_size = 0U;
        out->diagnostics.oom_count         = 0U;
    }

    return out;
}

void* o1heapAllocate(O1HeapInstance* const handle, const size_t amount)
{
    void* out = NULL;
    if (O1HEAP_LIKELY((handle != NULL) && (amount > 0U)))
    {
        // Add the header size and align the allocation size to the power of 2.
        // See "Timing-Predictable Memory Allocation In Hard Real-Time Systems", Joerg Herter, page 27:
        // alignment to the power of 2 guarantees that the worst case external fragmentation is bounded.
        // This comes at the expense of higher memory utilization but it is acceptable.
        const size_t fragment_size = pow2(log2Ceil(amount + O1HEAP_ALIGNMENT));
        O1HEAP_ASSERT(fragment_size >= SMALLEST_FRAGMENT_SIZE);
        O1HEAP_ASSERT(fragment_size >= amount + O1HEAP_ALIGNMENT);
        O1HEAP_ASSERT(isPowerOf2(fragment_size));

        const uint8_t optimal_bin_index = computeBinIndex(fragment_size);
        O1HEAP_ASSERT(optimal_bin_index < NUM_BINS_MAX);
        const size_t candidate_bin_mask = ~(pow2(optimal_bin_index) - 1U);

        invokeHook(handle->critical_section_enter);

        // Find the smallest non-empty bin we can use.
        const size_t suitable_bins     = handle->nonempty_bin_mask & candidate_bin_mask;
        const size_t smallest_bin_mask = suitable_bins & ~(suitable_bins - 1U);  // Clear all bits but the lowest.
        if (O1HEAP_LIKELY(smallest_bin_mask != 0))
        {
            O1HEAP_ASSERT(isPowerOf2(smallest_bin_mask));
            const uint8_t bin_index = log2Floor(smallest_bin_mask);
            O1HEAP_ASSERT(bin_index >= optimal_bin_index);
            O1HEAP_ASSERT(bin_index < NUM_BINS_MAX);

            // The bin we found shall not be empty, otherwise it's a state divergence (memory corruption?).
            Fragment* const frag = handle->bins[bin_index];
            O1HEAP_ASSERT(frag != NULL);
            O1HEAP_ASSERT(frag->header.size >= fragment_size);
            O1HEAP_ASSERT((frag->header.size % SMALLEST_FRAGMENT_SIZE) == 0U);

            // Unlink the fragment we found from the bin because we're going to be using it.
            handle->bins[bin_index] = frag->next_free;

            // Split the fragment if it is too large.
            const size_t leftover = frag->header.size - fragment_size;
            O1HEAP_ASSERT(leftover < handle->diagnostics.capacity);  // Overflow check.
            O1HEAP_ASSERT(leftover % SMALLEST_FRAGMENT_SIZE == 0U);  // Alignment check.
            if (O1HEAP_LIKELY(leftover >= SMALLEST_FRAGMENT_SIZE))
            {
                Fragment* const new_frag = (Fragment*) (void*) (((uint8_t*) frag) + leftover);
                O1HEAP_ASSERT(((size_t) new_frag) % O1HEAP_ALIGNMENT == 0U);
                new_frag->header.size = leftover;
                new_frag->header.used = false;
                // Insert the new split-off fragment into the doubly-linked list of fragments. Needed for merging later.
                new_frag->header.prev = frag;
                new_frag->header.next = frag->header.next;
                frag->header.next     = new_frag;
                if (O1HEAP_LIKELY(new_frag->header.next != NULL))
                {
                    O1HEAP_ASSERT(new_frag->header.next->header.prev == frag);
                    new_frag->header.next->header.prev = new_frag;
                }
                // Insert the new split-off fragment into the bin of the appropriate size.
                const uint8_t new_bin_index = computeBinIndex(leftover);
                new_frag->next_free         = handle->bins[new_bin_index];
                handle->bins[new_bin_index] = new_frag;
                handle->nonempty_bin_mask |= pow2(new_bin_index);
            }

            // Synchronize the mask after all of the perturbations are over.
            if (O1HEAP_LIKELY(handle->bins[bin_index] == NULL))
            {
                handle->nonempty_bin_mask &= ~pow2(bin_index);
            }

            // Update the diagnostics.
            O1HEAP_ASSERT((handle->diagnostics.allocated % SMALLEST_FRAGMENT_SIZE) == 0U);
            handle->diagnostics.allocated += fragment_size;
            if (handle->diagnostics.peak_allocated < handle->diagnostics.allocated)
            {
                handle->diagnostics.peak_allocated = handle->diagnostics.allocated;
            }

            // Finalize the fragment we just allocated.
            O1HEAP_ASSERT(frag->header.size >= amount + O1HEAP_ALIGNMENT);
            frag->header.used = true;
            frag->next_free   = NULL;  // This is not necessary but it works as a canary to detect memory corruption.
            out               = ((uint8_t*) frag) + O1HEAP_ALIGNMENT;
        }
        else  // Unsuccessful allocation -- out of memory.
        {
            handle->diagnostics.oom_count++;
        }

        // Update the diagnostics. The peak fragment size shall be updated even if we were unable to allocate.
        if (handle->diagnostics.peak_request_size < fragment_size)
        {
            handle->diagnostics.peak_request_size = fragment_size;
        }

        invokeHook(handle->critical_section_leave);
    }
    else
    {
        O1HEAP_ASSERT(handle != NULL);
    }
    return out;
}

void o1heapFree(O1HeapInstance* const handle, void* const pointer)
{
    if (O1HEAP_LIKELY((handle != NULL) && (pointer != NULL) && (((size_t) pointer) % O1HEAP_ALIGNMENT == 0U)))
    {
        Fragment* const frag = (Fragment*) (void*) (((uint8_t*) pointer) - O1HEAP_ALIGNMENT);
        O1HEAP_ASSERT(((size_t) frag) % sizeof(Fragment*) == 0U);

        invokeHook(handle->critical_section_enter);

        invokeHook(handle->critical_section_leave);
    }
    else
    {
        O1HEAP_ASSERT(handle != NULL);
        O1HEAP_ASSERT(((size_t) pointer) % O1HEAP_ALIGNMENT == 0U);
    }
}

O1HeapDiagnostics o1heapGetDiagnostics(const O1HeapInstance* const handle)
{
    O1HEAP_ASSERT(handle != NULL);
    invokeHook(handle->critical_section_enter);
    const O1HeapDiagnostics out = handle->diagnostics;
    invokeHook(handle->critical_section_leave);
    return out;
}
