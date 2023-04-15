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
// Copyright (c) 2020 Pavel Kirienko
// Authors: Pavel Kirienko <pavel.kirienko@zubax.com>

#include "internal.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>

namespace
{
constexpr std::size_t KiB = 1024U;
constexpr std::size_t MiB = KiB * KiB;

template <typename T>
auto log2Floor(const T& x) -> std::enable_if_t<std::is_integral_v<T>, std::uint8_t>
{
    std::size_t  tmp = x;
    std::uint8_t y   = 0;
    while (tmp > 1U)
    {
        tmp >>= 1U;
        y++;
    }
    return y;
}

auto getRandomByte()
{
    static std::random_device                           rd;
    static std::mt19937                                 gen(rd());
    static std::uniform_int_distribution<std::uint16_t> dis(0, 255U);
    return static_cast<std::byte>(dis(gen));
}

auto init(void* const base, const std::size_t size)
{
    using internal::Fragment;

    // Fill the beginning of the arena with random bytes (the entire arena may be too slow to fill).
    std::generate_n(reinterpret_cast<std::byte*>(base), std::min<std::size_t>(1 * MiB, size), getRandomByte);

    const auto heap = reinterpret_cast<internal::O1HeapInstance*>(o1heapInit(base, size));

    if (heap != nullptr)
    {
        REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

        heap->validate();

        REQUIRE(heap->nonempty_bin_mask > 0U);
        REQUIRE((heap->nonempty_bin_mask & (heap->nonempty_bin_mask - 1U)) == 0);
        for (auto i = 0U; i < std::size(heap->bins); i++)
        {
            const std::size_t min = Fragment::SizeMin << i;
            const std::size_t max = (Fragment::SizeMin << i) * 2U - 1U;
            if ((heap->nonempty_bin_mask & (1ULL << i)) == 0U)
            {
                REQUIRE(heap->bins.at(i) == nullptr);
            }
            else
            {
                REQUIRE(heap->bins.at(i) != nullptr);
                REQUIRE(heap->bins.at(i)->header.size >= min);
                REQUIRE(heap->bins.at(i)->header.size <= max);
            }
        }

        REQUIRE(heap->diagnostics.capacity < size);
        REQUIRE(heap->diagnostics.capacity <= Fragment::SizeMax);
        REQUIRE(heap->diagnostics.capacity >= Fragment::SizeMin);
        REQUIRE(heap->diagnostics.allocated == 0);
        REQUIRE(heap->diagnostics.oom_count == 0);
        REQUIRE(heap->diagnostics.peak_allocated == 0);
        REQUIRE(heap->diagnostics.peak_request_size == 0);

        const auto root_fragment = heap->bins.at(log2Floor(heap->nonempty_bin_mask));
        REQUIRE(root_fragment != nullptr);
        REQUIRE(root_fragment->next_free == nullptr);
        REQUIRE(root_fragment->prev_free == nullptr);
        REQUIRE(!root_fragment->header.used);
        REQUIRE(root_fragment->header.size == heap->diagnostics.capacity);
        REQUIRE(root_fragment->header.next == nullptr);
        REQUIRE(root_fragment->header.prev == nullptr);
    }
    return heap;
}

}  // namespace

TEST_CASE("General: init")
{
    using internal::Fragment;

    std::cout << "sizeof(void*)=" << sizeof(void*) << "; sizeof(O1HeapInstance)=" << sizeof(internal::O1HeapInstance)
              << std::endl;

    alignas(128) std::array<std::byte, 10'000U> arena{};

    REQUIRE(nullptr == init(nullptr, 0U));
    REQUIRE(nullptr == init(arena.data(), 0U));
    REQUIRE(nullptr == init(arena.data(), 99U));  // Too small.

    // Check various offsets and sizes to make sure the initialization is done correctly in all cases.
    for (auto offset = 0U; offset < 7U; offset++)
    {
        for (auto size = 99U; size < 5100U; size += 111U)
        {
            REQUIRE(arena.size() >= size);
            auto heap = init(arena.data() + offset, size - offset);
            if (heap != nullptr)
            {
                REQUIRE(size >= sizeof(internal::O1HeapInstance) + Fragment::SizeMin);
                REQUIRE(reinterpret_cast<std::size_t>(heap) >= reinterpret_cast<std::size_t>(arena.data()));
                REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);
                REQUIRE(heap->doInvariantsHold());
            }
        }
    }
}

TEST_CASE("General: allocate: OOM")
{
    constexpr auto                   MiB256    = MiB * 256U;
    constexpr auto                   ArenaSize = MiB256 + MiB;
    const std::shared_ptr<std::byte> arena(static_cast<std::byte*>(std::aligned_alloc(64U, ArenaSize)), &std::free);

    auto heap = init(arena.get(), ArenaSize);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->getDiagnostics().capacity > ArenaSize - 1024U);
    REQUIRE(heap->getDiagnostics().capacity < ArenaSize);
    REQUIRE(heap->getDiagnostics().oom_count == 0);

    REQUIRE(nullptr == heap->allocate(ArenaSize));  // Too large
    REQUIRE(heap->getDiagnostics().oom_count == 1);

    REQUIRE(nullptr == heap->allocate(ArenaSize - O1HEAP_ALIGNMENT));  // Too large
    REQUIRE(heap->getDiagnostics().oom_count == 2);

    REQUIRE(nullptr == heap->allocate(heap->diagnostics.capacity - O1HEAP_ALIGNMENT + 1U));  // Too large
    REQUIRE(heap->getDiagnostics().oom_count == 3);

    REQUIRE(nullptr == heap->allocate(ArenaSize * 10U));  // Too large
    REQUIRE(heap->getDiagnostics().oom_count == 4);

    REQUIRE(nullptr == heap->allocate(0));           // Nothing to allocate
    REQUIRE(heap->getDiagnostics().oom_count == 4);  // Not incremented! Zero allocation is not an OOM.

    REQUIRE(heap->getDiagnostics().peak_allocated == 0);
    REQUIRE(heap->getDiagnostics().allocated == 0);
    REQUIRE(heap->getDiagnostics().peak_request_size == ArenaSize * 10U);

    REQUIRE(nullptr != heap->allocate(MiB256 - O1HEAP_ALIGNMENT));  // Maximum possible allocation.
    REQUIRE(heap->getDiagnostics().oom_count == 4);                 // OOM counter not incremented.
    REQUIRE(heap->getDiagnostics().peak_allocated == MiB256);
    REQUIRE(heap->getDiagnostics().allocated == MiB256);
    REQUIRE(heap->getDiagnostics().peak_request_size == ArenaSize * 10U);  // Same size -- that one was unsuccessful.

    REQUIRE(heap->doInvariantsHold());
}

TEST_CASE("General: allocate: smallest")
{
    using internal::Fragment;

    constexpr auto                   ArenaSize = MiB * 300U;
    const std::shared_ptr<std::byte> arena(static_cast<std::byte*>(std::aligned_alloc(64U, ArenaSize)), &std::free);

    auto heap = init(arena.get(), ArenaSize);
    REQUIRE(heap != nullptr);

    void* const mem = heap->allocate(1U);
    REQUIRE(mem != nullptr);
    REQUIRE(heap->getDiagnostics().oom_count == 0);
    REQUIRE(heap->getDiagnostics().peak_allocated == Fragment::SizeMin);
    REQUIRE(heap->getDiagnostics().allocated == Fragment::SizeMin);
    REQUIRE(heap->getDiagnostics().peak_request_size == 1);

    auto& frag = Fragment::constructFromAllocatedMemory(mem);
    REQUIRE(frag.header.size == (O1HEAP_ALIGNMENT * 2U));
    REQUIRE(frag.header.next != nullptr);
    REQUIRE(frag.header.prev == nullptr);
    REQUIRE(frag.header.used);
    REQUIRE(frag.header.next->header.size == (heap->diagnostics.capacity - frag.header.size));
    REQUIRE(!frag.header.next->header.used);

    heap->free(mem);
    REQUIRE(heap->doInvariantsHold());
}

TEST_CASE("General: allocate: size_t overflow")
{
    using internal::Fragment;

    constexpr auto size_max = std::numeric_limits<std::size_t>::max();

    constexpr auto                   ArenaSize = MiB * 300U;
    const std::shared_ptr<std::byte> arena(static_cast<std::byte*>(std::aligned_alloc(64U, ArenaSize)), &std::free);

    auto heap = init(arena.get(), ArenaSize);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->diagnostics.capacity > (ArenaSize - 1024U));
    REQUIRE(heap->diagnostics.capacity < ArenaSize);
    for (auto i = 1U; i <= 2U; i++)
    {
        REQUIRE(nullptr == heap->allocate(size_max / i));
        REQUIRE(nullptr == heap->allocate(size_max / i + 1U));  // May overflow to 0.
        REQUIRE(nullptr == heap->allocate(size_max / i - 1U));
        REQUIRE(nullptr == heap->allocate(Fragment::SizeMax - O1HEAP_ALIGNMENT + 1U));
    }

    // Over-commit the arena -- it is SMALLER than the size we're providing; it's an UB but for a test it's acceptable.
    heap = init(arena.get(), size_max);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->diagnostics.capacity == Fragment::SizeMax);
    for (auto i = 1U; i <= 2U; i++)
    {
        REQUIRE(nullptr == heap->allocate(size_max / i));
        REQUIRE(nullptr == heap->allocate(size_max / i + 1U));
        REQUIRE(nullptr == heap->allocate(size_max / i - 1U));
        REQUIRE(nullptr == heap->allocate(Fragment::SizeMax - O1HEAP_ALIGNMENT + 1U));
    }

    // Make sure the max-sized fragments are allocatable.
    void* const mem = heap->allocate(Fragment::SizeMax - O1HEAP_ALIGNMENT);
    REQUIRE(mem != nullptr);

    auto& frag = Fragment::constructFromAllocatedMemory(mem);
    REQUIRE(frag.header.size == Fragment::SizeMax);
    REQUIRE(frag.header.next == nullptr);
    REQUIRE(frag.header.prev == nullptr);
    REQUIRE(frag.header.used);

    REQUIRE(heap->getDiagnostics().peak_allocated == Fragment::SizeMax);
    REQUIRE(heap->getDiagnostics().allocated == Fragment::SizeMax);

    REQUIRE(heap->nonempty_bin_mask == 0);
    REQUIRE(std::all_of(std::begin(heap->bins), std::end(heap->bins), [](auto* p) { return p == nullptr; }));

    REQUIRE(heap->doInvariantsHold());
}

TEST_CASE("General: free")
{
    using internal::Fragment;

    alignas(128U) std::array<std::byte, 4096U + sizeof(internal::O1HeapInstance) + O1HEAP_ALIGNMENT - 1U> arena{};
    auto heap = init(arena.data(), std::size(arena));
    REQUIRE(heap != nullptr);

    REQUIRE(nullptr == heap->allocate(0U));
    REQUIRE(heap->diagnostics.allocated == 0U);
    heap->free(nullptr);
    REQUIRE(heap->diagnostics.peak_allocated == 0U);
    REQUIRE(heap->diagnostics.peak_request_size == 0U);
    REQUIRE(heap->diagnostics.oom_count == 0U);

    std::size_t allocated         = 0U;
    std::size_t peak_allocated    = 0U;
    std::size_t peak_request_size = 0U;

    const auto alloc = [&](const std::size_t amount, const std::vector<std::pair<bool, std::size_t>>& reference) {
        const auto p = heap->allocate(amount);
        if (amount > 0U)
        {
            REQUIRE(p != nullptr);

            // Overwrite all to ensure that the allocator does not make implicit assumptions about the memory use.
            std::generate_n(reinterpret_cast<std::byte*>(p), amount, getRandomByte);

            const auto& frag = Fragment::constructFromAllocatedMemory(p);
            REQUIRE(frag.header.used);
            REQUIRE((frag.header.size & (frag.header.size - 1U)) == 0U);
            REQUIRE(frag.header.size >= (amount + O1HEAP_ALIGNMENT));
            REQUIRE(frag.header.size <= Fragment::SizeMax);

            allocated += frag.header.size;
            peak_allocated    = std::max(peak_allocated, allocated);
            peak_request_size = std::max(peak_request_size, amount);
        }
        else
        {
            REQUIRE(p == nullptr);
        }

        REQUIRE(heap->diagnostics.allocated == allocated);
        REQUIRE(heap->diagnostics.peak_allocated == peak_allocated);
        REQUIRE(heap->diagnostics.peak_request_size == peak_request_size);
        heap->matchFragments(reference);
        REQUIRE(heap->doInvariantsHold());
        return p;
    };

    const auto dealloc = [&](void* const p, const std::vector<std::pair<bool, std::size_t>>& reference) {
        INFO(heap->visualize());
        if (p != nullptr)
        {
            // Overwrite some to ensure that the allocator does not make implicit assumptions about the memory use.
            std::generate_n(reinterpret_cast<std::byte*>(p), O1HEAP_ALIGNMENT, getRandomByte);

            const auto& frag = Fragment::constructFromAllocatedMemory(p);
            REQUIRE(frag.header.used);
            REQUIRE(allocated >= frag.header.size);
            allocated -= frag.header.size;
            heap->free(p);
        }
        else
        {
            heap->free(p);
        }
        REQUIRE(heap->diagnostics.allocated == allocated);
        REQUIRE(heap->diagnostics.peak_allocated == peak_allocated);
        REQUIRE(heap->diagnostics.peak_request_size == peak_request_size);
        heap->matchFragments(reference);
        REQUIRE(heap->doInvariantsHold());
    };

    constexpr auto X = true;   // used
    constexpr auto O = false;  // free

    auto a = alloc(32U,
                   {
                       {X, 64},
                       {O, 4032},
                   });
    auto b = alloc(32U,
                   {
                       {X, 64},
                       {X, 64},
                       {O, 3968},
                   });
    auto c = alloc(32U,
                   {
                       {X, 64},
                       {X, 64},
                       {X, 64},
                       {O, 3904},
                   });
    auto d = alloc(32U,
                   {
                       {X, 64},
                       {X, 64},
                       {X, 64},
                       {X, 64},
                       {O, 3840},
                   });
    auto e = alloc(1024U,
                   {
                       {X, 64},
                       {X, 64},
                       {X, 64},
                       {X, 64},
                       {X, 2048},
                       {O, 1792},
                   });
    auto f = alloc(512U,
                   {
                       {X, 64},    // a
                       {X, 64},    // b
                       {X, 64},    // c
                       {X, 64},    // d
                       {X, 2048},  // e
                       {X, 1024},  // f
                       {O, 768},
                   });
    dealloc(b,
            {
                {X, 64},  // a
                {O, 64},
                {X, 64},    // c
                {X, 64},    // d
                {X, 2048},  // e
                {X, 1024},  // f
                {O, 768},
            });
    dealloc(a,
            {
                {O, 128},   // joined right
                {X, 64},    // c
                {X, 64},    // d
                {X, 2048},  // e
                {X, 1024},  // f
                {O, 768},
            });
    dealloc(c,
            {
                {O, 192},   // joined left
                {X, 64},    // d
                {X, 2048},  // e
                {X, 1024},  // f
                {O, 768},
            });
    dealloc(e,
            {
                {O, 192},
                {X, 64},  // d
                {O, 2048},
                {X, 1024},  // f
                {O, 768},
            });
    auto g = alloc(400U,  // The last block will be taken because it is a better fit.
                   {
                       {O, 192},
                       {X, 64},  // d
                       {O, 2048},
                       {X, 1024},  // f
                       {X, 512},   // g
                       {O, 256},
                   });
    dealloc(f,
            {
                {O, 192},
                {X, 64},    // d
                {O, 3072},  // joined left
                {X, 512},   // g
                {O, 256},
            });
    dealloc(d,
            {
                {O, 3328},  // joined left & right
                {X, 512},   // g
                {O, 256},
            });
    auto h = alloc(200U,
                   {
                       {O, 3328},
                       {X, 512},  // g
                       {X, 256},  // h
                   });
    auto i = alloc(32U,
                   {
                       {X, 64},  // i
                       {O, 3264},
                       {X, 512},  // g
                       {X, 256},  // h
                   });
    dealloc(g,
            {
                {X, 64},  // i
                {O, 3776},
                {X, 256},  // h
            });
    dealloc(h,
            {
                {X, 64},  // i
                {O, 4032},
            });
    dealloc(i,
            {
                {O, 4096},  // All heap is free.
            });

    REQUIRE(heap->diagnostics.capacity == 4096U);
    REQUIRE(heap->diagnostics.allocated == 0U);
    REQUIRE(heap->diagnostics.peak_allocated == 3328U);
    REQUIRE(heap->diagnostics.peak_request_size == 1024U);
    REQUIRE(heap->diagnostics.oom_count == 0U);
    REQUIRE(heap->doInvariantsHold());
}

/// This test has been empirically tuned to expand its state space coverage.
/// If any new behaviors need to be tested, please consider writing another test instead of changing this one.
TEST_CASE("General: random A")
{
    using internal::Fragment;

    constexpr auto                   ArenaSize = MiB * 300U;
    const std::shared_ptr<std::byte> arena(static_cast<std::byte*>(std::aligned_alloc(64U, ArenaSize)), &std::free);
    std::generate_n(arena.get(), ArenaSize, getRandomByte);  // Random-fill the ENTIRE arena!
    auto heap = init(arena.get(), ArenaSize);
    REQUIRE(heap != nullptr);

    std::vector<void*> pointers;

    std::size_t   allocated         = 0U;
    std::size_t   peak_allocated    = 0U;
    std::size_t   peak_request_size = 0U;
    std::uint64_t oom_count         = 0U;

    std::random_device random_device;
    std::mt19937       random_generator(random_device());

    const auto allocate = [&]() {
        REQUIRE(heap->doInvariantsHold());
        std::uniform_int_distribution<std::size_t> dis(0, ArenaSize / 1000U);

        const std::size_t amount = dis(random_generator);
        const auto        ptr    = heap->allocate(amount);
        if (ptr != nullptr)
        {
            // Overwrite all to ensure that the allocator does not make implicit assumptions about the memory use.
            std::generate_n(reinterpret_cast<std::byte*>(ptr), amount, getRandomByte);
            pointers.push_back(ptr);
            const auto& frag = Fragment::constructFromAllocatedMemory(ptr);
            allocated += frag.header.size;
            peak_allocated = std::max(peak_allocated, allocated);
        }
        else
        {
            if (amount > 0U)
            {
                oom_count++;
            }
        }
        peak_request_size = std::max(peak_request_size, amount);
        REQUIRE(heap->doInvariantsHold());
    };

    const auto deallocate = [&]() {
        REQUIRE(heap->doInvariantsHold());
        if (!pointers.empty())
        {
            std::uniform_int_distribution<decltype(pointers)::difference_type>
                        dis(0, static_cast<decltype(pointers)::difference_type>(std::size(pointers) - 1));
            const auto  it  = std::begin(pointers) + dis(random_generator);
            void* const ptr = *it;
            (void) pointers.erase(it);
            if (ptr != nullptr)
            {
                const auto& frag = Fragment::constructFromAllocatedMemory(ptr);
                frag.validate();
                REQUIRE(allocated >= frag.header.size);
                allocated -= frag.header.size;
            }
            heap->free(ptr);
        }
        REQUIRE(heap->doInvariantsHold());
    };

    // The memory use is growing slowly from zero.
    // We stop the test when it's been running near the max heap utilization for long enough.
    while (heap->diagnostics.oom_count < 1000U)
    {
        for (auto i = 0U; i < 100U; i++)
        {
            allocate();
        }
        for (auto i = 0U; i < 50U; i++)
        {
            deallocate();
        }
        REQUIRE(heap->diagnostics.allocated == allocated);
        REQUIRE(heap->diagnostics.peak_allocated == peak_allocated);
        REQUIRE(heap->diagnostics.peak_request_size == peak_request_size);
        REQUIRE(heap->diagnostics.oom_count == oom_count);
        REQUIRE(heap->doInvariantsHold());

        std::cout << heap->visualize() << std::endl;
    }
}

TEST_CASE("General: invariant checker")
{
    using internal::Fragment;

    alignas(128U) std::array<std::byte, 4096U + sizeof(internal::O1HeapInstance) + O1HEAP_ALIGNMENT - 1U> arena{};
    auto heap = init(arena.data(), std::size(arena));
    REQUIRE(heap != nullptr);
    REQUIRE(heap->doInvariantsHold());
    auto& dg = heap->diagnostics;

    dg.capacity++;
    REQUIRE(!heap->doInvariantsHold());
    dg.capacity--;
    REQUIRE(heap->doInvariantsHold());

    dg.allocated += Fragment::SizeMin;
    REQUIRE(!heap->doInvariantsHold());
    dg.peak_allocated += Fragment::SizeMin;
    REQUIRE(!heap->doInvariantsHold());
    dg.peak_request_size += 1;
    REQUIRE(heap->doInvariantsHold());
    dg.peak_allocated--;
    REQUIRE(!heap->doInvariantsHold());
    dg.peak_allocated++;
    dg.allocated -= Fragment::SizeMin;
    REQUIRE(heap->doInvariantsHold());
    dg.allocated++;
    REQUIRE(!heap->doInvariantsHold());
    dg.allocated--;
    REQUIRE(heap->doInvariantsHold());

    dg.peak_allocated = dg.capacity + 1U;
    REQUIRE(!heap->doInvariantsHold());
    dg.peak_allocated = dg.capacity;
    REQUIRE(heap->doInvariantsHold());

    dg.peak_request_size = dg.capacity;
    REQUIRE(!heap->doInvariantsHold());
    dg.oom_count++;
    REQUIRE(heap->doInvariantsHold());
}
