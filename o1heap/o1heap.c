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
#include <stdbool.h>

// ---------------------------------------- BUILD CONFIGURATION OPTIONS ----------------------------------------

/// The assertion macro defaults to the standard assert().
/// It can be overridden to manually suppress assertion checks or use a different error handling policy.
#ifndef O1HEAP_ASSERT
#   define O1HEAP_ASSERT(x) assert(x)
#endif

/// Branch probability annotations are used to improve the worst case execution time (WCET). They are entirely optional.
/// A stock implementation is provided for GCC/Clang; for other compilers it defaults to nothing.
/// If you are using a different compiler, consider overriding this value.
#ifndef O1HEAP_LIKELY
#   if defined(__GNUC__) || defined(__clang__)
#       define O1HEAP_LIKELY(x) __builtin_expect((x), 1)
#   else
#       define O1HEAP_LIKELY(x) x
#   endif
#endif

/// This option is used to facilitate unit testing. It is not recommended for use in production.
#ifndef O1HEAP_SELF_TEST
#   define O1HEAP_SELF_TEST 0
#endif

// ---------------------------------------- INTERNAL DEFINITIONS ----------------------------------------

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#   error "Unsupported language: ISO C99 or a newer version is required."
#endif

#if __STDC_VERSION__ < 201112L
#   define static_assert(x, ...) typedef char _static_assert_glue(_static_assertion_, __LINE__)[(x) ? 1 : -1]
#   define _static_assert_glue(a, b) _static_assert_glue_impl(a, b)
#   define _static_assert_glue_impl(a, b) a##b
#endif

#define SMALLEST_BLOCK_SIZE (O1HEAP_ALIGNMENT * 2U)

/// Subtraction of the pointer size is a very basic heuristic needed to reduce the number of unnecessary bins.
/// Normally we should subtract log2(SMALLEST_BIN_CAPACITY) but log2 is bulky to compute using the preprocessor only.
#define NUM_BINS_MAX (sizeof(size_t) * 8U - sizeof(void*))

static_assert((O1HEAP_ALIGNMENT & (O1HEAP_ALIGNMENT - 1U)) == 0U, "The alignment shall be an integer power of 2");
static_assert((SMALLEST_BLOCK_SIZE & (SMALLEST_BLOCK_SIZE - 1U)) == 0U,
              "The smallest block size shall be an integer power of 2");

typedef struct Block Block;

typedef struct BlockHeader
{
    struct Block* next;
    struct Block* prev;
    size_t        size;
    bool          used;
} BlockHeader;
static_assert(sizeof(BlockHeader) <= O1HEAP_ALIGNMENT, "Memory layout error");

struct Block
{
    BlockHeader header;
    // Everything past the header may spill over into the allocatable space. The header survives across alloc/free.
    struct Block* next_free;
};
static_assert(sizeof(Block) <= SMALLEST_BLOCK_SIZE, "Memory layout error");

struct O1HeapInstance
{
    Block* bins[NUM_BINS_MAX];   ///< Smallest blocks are in the bin at index 0.
    size_t nonempty_bin_mask;    ///< Bit 1 represents a non-empty bin; bit 0 for smallest bins.
    size_t total_heap_size;      ///< The total amount of allocatable memory, including the per-block overhead.

    O1HeapHook critical_section_enter;
    O1HeapHook critical_section_leave;
};

/// True if the argument is an integer power of two or zero.
static inline bool isPowerOf2(const size_t x)
{
    return (x & (x - 1U)) == 0U;
}

/// Special case: if the argument is zero, returns zero.
static inline uint8_t log2Floor(const size_t x)
{
    size_t tmp = x;
    uint8_t y = 0;
    while (tmp > 1U)
    {
        tmp >>= 1U;
        y++;
    }
    return y;
}

/// Special case: if the argument is zero, returns zero.
static inline uint8_t log2Ceil(const size_t x)
{
    return (uint8_t) (log2Floor(x) + (isPowerOf2(x) ? 0U : 1U));
}

static inline uint8_t computeBinIndex(const size_t block_size)
{
    O1HEAP_ASSERT(block_size >= SMALLEST_BLOCK_SIZE);
    O1HEAP_ASSERT(block_size % SMALLEST_BLOCK_SIZE == 0U);
    const uint8_t lg = log2Ceil(block_size / SMALLEST_BLOCK_SIZE);
    O1HEAP_ASSERT(lg < NUM_BINS_MAX);
    return lg;
}

/// Raise 2 into the specified power.
static inline size_t pow2(const uint8_t power)
{
    return ((size_t) 1U) << power;
}

static inline void invokeHook(const O1HeapHook hook)
{
    if (O1HEAP_LIKELY(hook != NULL))
    {
        hook();
    }
}

// ---------------------------------------- PUBLIC API IMPLEMENTATION ----------------------------------------

O1HeapInstance* o1heapInit(void* const base,
                           const size_t size,
                           const O1HeapHook critical_section_enter,
                           const O1HeapHook critical_section_leave)
{
    // Align the arena pointer.
    uint8_t* adjusted_base = (uint8_t*) base;
    size_t adjusted_size = size;
    while ((((size_t) adjusted_base) % sizeof(O1HeapInstance*) != 0U) && (adjusted_size > 0U) &&
           (adjusted_base != NULL))
    {
        adjusted_base++;
        O1HEAP_ASSERT(adjusted_size > 0U);
        adjusted_size--;
    }

    O1HeapInstance* out = NULL;
    if ((adjusted_base != NULL) &&
        (adjusted_size >= (sizeof(O1HeapInstance) + SMALLEST_BLOCK_SIZE + O1HEAP_ALIGNMENT * 2U)))
    {
        // Allocate the heap metadata structure in the beginning of the arena.
        O1HEAP_ASSERT(((size_t) adjusted_base) % sizeof(O1HeapInstance*) == 0U);
        out = (O1HeapInstance*) (void*) adjusted_base;
        adjusted_base += sizeof(O1HeapInstance);
        adjusted_size -= sizeof(O1HeapInstance);
        out->critical_section_enter = critical_section_enter;
        out->critical_section_leave = critical_section_leave;

        // Align the start of the storage.
        while (((size_t) adjusted_base) % O1HEAP_ALIGNMENT != 0U)
        {
            adjusted_base++;
            O1HEAP_ASSERT(adjusted_size > 0U);
            adjusted_size--;
        }

        // Align the size; i.e., truncate the end.
        while ((adjusted_size % SMALLEST_BLOCK_SIZE) != 0)
        {
            O1HEAP_ASSERT(adjusted_size > 0U);
            adjusted_size--;
        }

        O1HEAP_ASSERT(adjusted_size % SMALLEST_BLOCK_SIZE == 0);
        O1HEAP_ASSERT(adjusted_size >= SMALLEST_BLOCK_SIZE);
        out->total_heap_size = adjusted_size;
        for (size_t i = 0; i < NUM_BINS_MAX; i++)
        {
            out->bins[i] = NULL;
        }

        O1HEAP_ASSERT(((size_t) adjusted_base) % O1HEAP_ALIGNMENT == 0U);
        O1HEAP_ASSERT(((size_t) adjusted_base) % sizeof(Block*) == 0U);
        Block* const root_bin = (Block*) (void*) adjusted_base;
        root_bin->header.next = NULL;
        root_bin->header.prev = NULL;
        root_bin->header.size = adjusted_size;
        root_bin->header.used = false;
        root_bin->next_free = NULL;

        const uint8_t bin_index = computeBinIndex(adjusted_size);
        O1HEAP_ASSERT(bin_index < NUM_BINS_MAX);
        out->bins[bin_index] = root_bin;
        out->nonempty_bin_mask = 1U << bin_index;

        O1HEAP_ASSERT(out->nonempty_bin_mask != 0U);
        O1HEAP_ASSERT(out->total_heap_size >= SMALLEST_BLOCK_SIZE);
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
        const size_t block_size = pow2(log2Ceil(amount + O1HEAP_ALIGNMENT));
        O1HEAP_ASSERT(block_size >= SMALLEST_BLOCK_SIZE);
        O1HEAP_ASSERT(block_size >= amount + O1HEAP_ALIGNMENT);
        O1HEAP_ASSERT(isPowerOf2(block_size));

        const uint8_t optimal_bin_index = computeBinIndex(block_size);
        O1HEAP_ASSERT(optimal_bin_index < NUM_BINS_MAX);
        const size_t candidate_bin_mask = ~(pow2(optimal_bin_index) - 1U);

        invokeHook(handle->critical_section_enter);

        // Find the smallest non-empty bin we can use.
        const size_t suitable_bins = handle->nonempty_bin_mask & candidate_bin_mask;
        const size_t smallest_bin_mask = suitable_bins & ~(suitable_bins - 1U);  // Clear all bits but the lowest.
        if (O1HEAP_LIKELY(smallest_bin_mask != 0))  // Otherwise, the allocation request cannot be served -- no memory.
        {
            O1HEAP_ASSERT(isPowerOf2(smallest_bin_mask));
            const uint8_t bin_index = log2Floor(smallest_bin_mask);
            O1HEAP_ASSERT(bin_index >= optimal_bin_index);
            O1HEAP_ASSERT(bin_index < NUM_BINS_MAX);

            // The bin we found shall not be empty, otherwise it's a state divergence (memory corruption?)
            Block* const blk = handle->bins[bin_index];
            O1HEAP_ASSERT(blk != NULL);
            O1HEAP_ASSERT(blk->header.size >= block_size);
            O1HEAP_ASSERT((blk->header.size % SMALLEST_BLOCK_SIZE) == 0U);

            // Unlink the block we found from the bin because we're going to be using it.
            handle->bins[bin_index] = blk->next_free;

            // Split the block if it is too large.
            const size_t leftover = blk->header.size - block_size;
            O1HEAP_ASSERT(leftover < handle->total_heap_size);      // Overflow check.
            O1HEAP_ASSERT(leftover % SMALLEST_BLOCK_SIZE == 0U);    // Alignment check.
            if (O1HEAP_LIKELY(leftover >= SMALLEST_BLOCK_SIZE))     // Annotated as likely to optimize the worst case.
            {
                Block* const new_blk = (Block*) (void*) (((uint8_t*) blk) + leftover);
                O1HEAP_ASSERT(((size_t) new_blk) % O1HEAP_ALIGNMENT == 0U);
                new_blk->header.size = leftover;
                new_blk->header.used = false;
                // Insert the new split-off block into the doubly-linked list of blocks. Needed for merging later.
                new_blk->header.prev = blk;
                new_blk->header.next = blk->header.next;
                blk->header.next = new_blk;
                if (O1HEAP_LIKELY(new_blk->header.next != NULL))
                {
                    O1HEAP_ASSERT(new_blk->header.next->header.prev == blk);
                    new_blk->header.next->header.prev = new_blk;
                }
                // Insert the new split-off block into the bin of the appropriate size.
                const uint8_t new_bin_index = computeBinIndex(leftover);
                new_blk->next_free = handle->bins[new_bin_index];
                handle->bins[new_bin_index] = new_blk;
                handle->nonempty_bin_mask |= pow2(new_bin_index);
            }

            // Synchronize the mask after all of the perturbations are over.
            if (O1HEAP_LIKELY(handle->bins[bin_index] == NULL))
            {
                handle->nonempty_bin_mask &= ~pow2(bin_index);
            }

            // TODO: update the diagnostic info.

            // Finalize the block we just allocated.
            O1HEAP_ASSERT(blk->header.size >= amount + O1HEAP_ALIGNMENT);
            blk->header.used = true;
            blk->next_free = NULL;  // This is not necessary but it works as a canary to detect memory corruption.
            out = ((uint8_t*) blk) + O1HEAP_ALIGNMENT;
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
        Block* const blk = (Block*) (void*) (((uint8_t*) pointer) - O1HEAP_ALIGNMENT);
        O1HEAP_ASSERT(((size_t) blk) % sizeof(Block*) == 0U);

        invokeHook(handle->critical_section_enter);

        invokeHook(handle->critical_section_leave);
    }
    else
    {
        O1HEAP_ASSERT(handle != NULL);
        O1HEAP_ASSERT(((size_t) pointer) % O1HEAP_ALIGNMENT == 0U);
    }
}

// ---------------------------------------- SELF TEST FUNCTION ----------------------------------------

#if O1HEAP_SELF_TEST

void o1heapSelfTest(void);

void o1heapSelfTest(void)
{
    O1HEAP_ASSERT(isPowerOf2(0));     // Special case.
    O1HEAP_ASSERT(isPowerOf2(1));     // 2**0
    O1HEAP_ASSERT(isPowerOf2(2));     // 2**1
    O1HEAP_ASSERT(!isPowerOf2(3));
    O1HEAP_ASSERT(isPowerOf2(4));
    O1HEAP_ASSERT(!isPowerOf2(5));
    O1HEAP_ASSERT(!isPowerOf2(6));
    O1HEAP_ASSERT(!isPowerOf2(7));
    O1HEAP_ASSERT(isPowerOf2(8));
    O1HEAP_ASSERT(!isPowerOf2(9));

    O1HEAP_ASSERT(log2Floor(0) == 0);
    O1HEAP_ASSERT(log2Floor(1) == 0);
    O1HEAP_ASSERT(log2Floor(2) == 1);
    O1HEAP_ASSERT(log2Floor(3) == 1);
    O1HEAP_ASSERT(log2Floor(4) == 2);
    O1HEAP_ASSERT(log2Floor(30) == 4);
    O1HEAP_ASSERT(log2Floor(60) == 5);
    O1HEAP_ASSERT(log2Floor(64) == 6);

    O1HEAP_ASSERT(log2Ceil(0) == 0);
    O1HEAP_ASSERT(log2Ceil(1) == 0);
    O1HEAP_ASSERT(log2Ceil(2) == 1);
    O1HEAP_ASSERT(log2Ceil(3) == 2);
    O1HEAP_ASSERT(log2Ceil(4) == 2);
    O1HEAP_ASSERT(log2Ceil(30) == 5);
    O1HEAP_ASSERT(log2Ceil(60) == 6);
    O1HEAP_ASSERT(log2Ceil(64) == 6);

    O1HEAP_ASSERT(pow2(0) == 1);
    O1HEAP_ASSERT(pow2(1) == 2);
    O1HEAP_ASSERT(pow2(2) == 4);
    O1HEAP_ASSERT(pow2(3) == 8);
    O1HEAP_ASSERT(pow2(4) == 16);
    O1HEAP_ASSERT(pow2(5) == 32);
    O1HEAP_ASSERT(pow2(6) == 64);
    O1HEAP_ASSERT(pow2(7) == 128);
    O1HEAP_ASSERT(pow2(8) == 256);
    O1HEAP_ASSERT(pow2(9) == 512);

    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 1U) == 0);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 2U) == 1);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 3U) == 2);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 4U) == 2);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 5U) == 3);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 6U) == 3);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 7U) == 3);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 8U) == 3);
    O1HEAP_ASSERT(computeBinIndex(SMALLEST_BLOCK_SIZE * 9U) == 4);
}

#endif
