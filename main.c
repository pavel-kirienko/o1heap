#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

#define CANARD_INTERNAL static
#define CANARD_ASSERT(x) assert(x)

/// Shall be an integer power of two and not less than the biggest alignment requirement for the platform.
#ifndef CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT
#   define CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT 16U
#endif

#if (CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT < 4U) || \
    ((CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT & (CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT - 1)) != 0)
#   error "Invalid value of CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT"
#endif

/// An instance of this type is returned by the allocator when memory is allocated.
/// The same instance is fed back to the allocator to free a previously allocated memory block.
/// Both the pointer and the amount shall be kept intact, otherwise it's an UB.
typedef struct
{
    void* pointer;  ///< NULL indicates that the allocation request could not be served (out of memory).
    size_t amount;  ///< May be larger than requested.
} AllocatedMemory;

typedef struct CanardMemoryFreeList
{
    struct CanardMemoryFreeList* next;
    void** root;    ///< Pointer to the first block in the list. Each free block contains a pointer to the next.
} CanardMemoryFreeList;

typedef struct
{
    CanardMemoryFreeList* root; ///< Linked list of free lists; i.e., a linked list of linked lists of free blocks.
    size_t root_block_size;     ///< The root blocks are the smallest. Every next list contains 2x larger blocks.
    size_t total_capacity;      ///< This value is needed for statistics and internal consistency checks.
} CanardMemoryAllocator;

// ---------------------------------------- MEMORY ALLOCATION ----------------------------------------

CANARD_INTERNAL AllocatedMemory doRecursiveAllocation(CanardMemoryFreeList* const free_list,
                                                      const size_t                block_size,
                                                      const size_t                amount)
{
    CANARD_ASSERT(block_size % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0);
    AllocatedMemory out = {NULL, 0U};
    if (free_list != NULL)
    {
        if (amount <= block_size)
        {
            if (free_list->root != NULL)
            {
                out.amount = block_size;
                out.pointer = free_list->root;
                free_list->root = *(free_list->root);
            }
            else
            {
                // MISRA exception: intentional recursive call.
                const AllocatedMemory extension = doRecursiveAllocation(free_list->next,
                                                                        block_size * 2U,
                                                                        block_size * 2U);
                if (extension.pointer != NULL)
                {
                    CANARD_ASSERT((extension.amount >= block_size * 2U) && (extension.amount >= (amount * 2U)));
                    void** const new_root = (void**)extension.pointer;
                    *new_root = NULL;
                    free_list->root = new_root;
                    CANARD_ASSERT((free_list->root != NULL) && (*(free_list->root) == NULL));
                    out.amount = block_size;
                    // MISRA exception: pointer arithmetic is unavoidable -- this is a memory allocator.
                    out.pointer = ((uint8_t*) extension.pointer) + block_size;
                }
            }
        }
        else
        {
            // MISRA exception: intentional recursive call.
            out = doRecursiveAllocation(free_list->next, block_size * 2U, amount);
        }
    }
    CANARD_ASSERT((out.amount == 0) || (out.amount >= amount));
    CANARD_ASSERT(out.amount % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0);
    CANARD_ASSERT(out.amount % block_size == 0);
    CANARD_ASSERT(((size_t) out.pointer) % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0);
    return out;
}


CANARD_INTERNAL AllocatedMemory allocate(CanardMemoryAllocator* const allocator, const size_t amount)
{
    CANARD_ASSERT(allocator != NULL);
    AllocatedMemory out = {NULL, 0U};
    if (amount > 0U)
    {
        out = doRecursiveAllocation(allocator->root, allocator->root_block_size, amount);
    }
    return out;
}

// ---------------------------------------- MEMORY DEALLOCATION ----------------------------------------

CANARD_INTERNAL void acceptFreeBlock(CanardMemoryFreeList* const root,
                                     const size_t                root_block_size,
                                     const void*                 pointer,
                                     const size_t                amount)
{
    CANARD_ASSERT(pointer != NULL);
    CANARD_ASSERT(amount > 0U);
    CANARD_ASSERT(((size_t) pointer) % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0);
    CANARD_ASSERT(amount % root_block_size == 0);

    CanardMemoryFreeList* free_list = root;
    size_t next_block_size = root_block_size;
    while (free_list != NULL)
    {
        next_block_size *= 2U;
        if (amount < next_block_size)
        {
            void** const new_root = (void**)pointer;
            *new_root = free_list->root;
            free_list->root = new_root;
            break;
        }
        free_list = free_list->next;
    }

    // Post-condition: the block has been inserted successfully. If not, it is malformed or the allocator is damaged.
    CANARD_ASSERT(amount < next_block_size);
}

CANARD_INTERNAL void deallocate(CanardMemoryAllocator* const allocator, const AllocatedMemory memory)
{
    CANARD_ASSERT(allocator != NULL);
    if ((memory.pointer != NULL) && (memory.amount > 0U) &&
        (((size_t) memory.pointer) % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0))
    {
        acceptFreeBlock(allocator->root, allocator->root_block_size, memory.pointer, memory.amount);
    }
}

// ---------------------------------------- MEMORY ALLOCATOR INITIALIZATION ----------------------------------------

CANARD_INTERNAL bool isPowerOf2(const size_t x)
{
    return (x & (x - 1U)) == 0U;
}

CANARD_INTERNAL size_t floorToPowerOf2(const size_t x)
{
    size_t power = (x > 0U) ? 1U : x;
    while (power < x)
    {
        power *= 2U;
    }
    CANARD_ASSERT(isPowerOf2(power) && (power <= x));
    return power;
}

CANARD_INTERNAL size_t findOptimalRootBlockSize(const size_t min_block_size_hint)
{
    size_t x = min_block_size_hint;
    while ((x < CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT) || !isPowerOf2(x))
    {
        x++;
    }
    CANARD_ASSERT(x >= CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT);
    CANARD_ASSERT(isPowerOf2(x));
    return x;
}

CANARD_INTERNAL CanardMemoryFreeList* constructFreeLists(const size_t   root_block_size,
                                                         uint8_t* const arena,
                                                         size_t* const  inout_arena_size)
{
    CANARD_ASSERT(root_block_size > 0U);
    CANARD_ASSERT(arena != NULL);
    CANARD_ASSERT(inout_arena_size != NULL);

    uint8_t* ptr = arena + *inout_arena_size;
    while ((*inout_arena_size > 0U) && (((size_t) ptr) % sizeof(CanardMemoryFreeList*) != 0))
    {
        ptr--;
        (*inout_arena_size)--;
    }
    CANARD_ASSERT(((size_t) ptr) % sizeof(CanardMemoryFreeList) == 0);

    size_t block_size = floorToPowerOf2(*inout_arena_size);
    CanardMemoryFreeList* root = NULL;
    while ((block_size >= root_block_size) && (*inout_arena_size >= sizeof(CanardMemoryFreeList)))
    {
        CANARD_ASSERT(isPowerOf2(block_size));

        CANARD_ASSERT(((size_t) ptr) % sizeof(CanardMemoryFreeList*) == 0);
        CanardMemoryFreeList* new_root = (CanardMemoryFreeList*) (void*) ptr;
        ptr -= sizeof(CanardMemoryFreeList);
        (*inout_arena_size) -= sizeof(CanardMemoryFreeList);

        new_root->next = root;
        new_root->root = NULL;
        root = new_root;

        block_size /= 2U;
    }

    return root;
}

CANARD_INTERNAL void populateFreeLists(CanardMemoryFreeList* const root,
                                       const size_t                root_block_size,
                                       uint8_t* const              arena,
                                       size_t const                arena_size)
{
    uint8_t* ptr = arena;
    size_t remaining = arena_size;
    // Fill as many large blocks as we can, then switch to the smaller blocks.
    while (remaining >= root_block_size)
    {
        const size_t block_size = floorToPowerOf2(remaining);
        CANARD_ASSERT(block_size >= root_block_size);
        CANARD_ASSERT(block_size <= remaining);
        acceptFreeBlock(root, root_block_size, ptr, block_size);
        ptr += block_size;
        remaining -= block_size;
    }
    CANARD_ASSERT(remaining <= root_block_size);
}

CANARD_INTERNAL CanardMemoryAllocator initMemoryAllocator(void* const  arena,
                                                          const size_t arena_size,
                                                          const size_t min_block_size_hint)
{
    const size_t root_block_size = findOptimalRootBlockSize(min_block_size_hint);
    uint8_t* remaining_arena = (uint8_t*) arena;
    size_t remaining_arena_size = (arena == NULL) ? 0U : arena_size;
    while ((remaining_arena_size > 0U) && (((size_t) remaining_arena) % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT != 0))
    {
        remaining_arena++;
        remaining_arena_size--;
    }
    CANARD_ASSERT(((size_t) remaining_arena) % CANARD_CONFIG_MEMORY_ALLOCATOR_ALIGNMENT == 0);
    CANARD_ASSERT(remaining_arena_size <= arena_size);

    CanardMemoryFreeList* root = NULL;
    if (remaining_arena_size > 0U)
    {
        root = constructFreeLists(root_block_size, remaining_arena, &remaining_arena_size);
        CANARD_ASSERT(remaining_arena_size <= arena_size);  // Overflow check.
        populateFreeLists(root, root_block_size, remaining_arena, remaining_arena_size);
    }

    const CanardMemoryAllocator out = {
        .root            = root,
        .root_block_size = root_block_size,
        .total_capacity  = remaining_arena_size
    };
    return out;
}


int main()
{
    (void) allocate;
    (void) deallocate;
    (void) initMemoryAllocator;
    return 0;
}
