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

#include "internal.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>

namespace cs
{
namespace
{
volatile std::uint64_t g_cnt_enter = 0;
volatile std::uint64_t g_cnt_leave = 0;

void ensureNotInside()
{
    REQUIRE(g_cnt_enter == g_cnt_leave);
}

void enter()
{
    ensureNotInside();
    g_cnt_enter++;
}

void leave()
{
    g_cnt_leave++;
    ensureNotInside();
}

void resetCounters()
{
    ensureNotInside();
    g_cnt_enter = 0;
    g_cnt_leave = 0;
}

void validateAndReset(const std::uint64_t cnt)
{
    REQUIRE(g_cnt_enter == cnt);
    REQUIRE(g_cnt_leave == cnt);
    resetCounters();
}

}  // namespace
}  // namespace cs

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

auto init(void* const       base,
          const std::size_t size,
          const O1HeapHook  critical_section_enter = nullptr,
          const O1HeapHook  critical_section_leave = nullptr)
{
    using internal::Fragment;

    // Fill the beginning of the arena with random bytes (the entire arena may be too slow to fill).
    std::generate_n(reinterpret_cast<std::byte*>(base), std::min<std::size_t>(10'000U, size), getRandomByte);

    const auto heap = reinterpret_cast<internal::O1HeapInstance*>(
        o1heapInit(base, size, critical_section_enter, critical_section_leave));

    if (heap != nullptr)
    {
        REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

        REQUIRE(heap->critical_section_enter == critical_section_enter);
        REQUIRE(heap->critical_section_leave == critical_section_leave);

        heap->validateInvariants();

        // std::cout << "arena_size=" << size << "; "
        //           << "capacity=" << heap->diagnostics.capacity << "; "
        //           << "nonempty_bin_mask=" << heap->nonempty_bin_mask << std::endl;
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

    REQUIRE(nullptr == init(nullptr, 0U, nullptr, nullptr));
    REQUIRE(nullptr == init(nullptr, 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == init(arena.data(), 0U, nullptr, nullptr));
    REQUIRE(nullptr == init(arena.data(), 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == init(arena.data(), 99U, nullptr, nullptr));  // Too small.
    REQUIRE(nullptr == init(arena.data(), 99U, &cs::enter, &cs::leave));

    // Check various offsets and sizes to make sure the initialization is done correctly in all cases.
    for (auto offset = 0U; offset < 7U; offset++)
    {
        for (auto size = 99U; size < 5100U; size += 111U)
        {
            REQUIRE(arena.size() >= size);
            auto heap = init(arena.data() + offset,
                             size - offset,
                             (offset % 2U == 0U) ? &cs::enter : nullptr,
                             (offset % 4U == 0U) ? &cs::leave : nullptr);
            if (heap == nullptr)
            {
                REQUIRE(size <= sizeof(internal::O1HeapInstance) * 2U + Fragment::SizeMin * 2U);
            }
            else
            {
                REQUIRE(size >= sizeof(internal::O1HeapInstance) + Fragment::SizeMin);
                REQUIRE(reinterpret_cast<std::size_t>(heap) >= reinterpret_cast<std::size_t>(arena.data()));
                REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);
            }
        }
    }

    REQUIRE(cs::g_cnt_enter == 0);
    REQUIRE(cs::g_cnt_leave == 0);
}

TEST_CASE("General: allocate: OOM")
{
    cs::resetCounters();

    constexpr auto               MiB256    = MiB * 256U;
    constexpr auto               ArenaSize = MiB256 + MiB;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays

    auto heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->getDiagnostics().capacity > ArenaSize - 1024U);
    REQUIRE(heap->getDiagnostics().capacity < ArenaSize);
    REQUIRE(heap->getDiagnostics().oom_count == 0);
    REQUIRE(cs::g_cnt_enter == 3);
    REQUIRE(cs::g_cnt_enter == 3);

    REQUIRE(nullptr == heap->allocate(ArenaSize));  // Too large
    REQUIRE(heap->getDiagnostics().oom_count == 1);
    REQUIRE(cs::g_cnt_enter == 5);
    REQUIRE(cs::g_cnt_enter == 5);

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
}

TEST_CASE("General: allocate: smallest")
{
    using internal::Fragment;

    cs::resetCounters();

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays

    auto heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);

    void* const mem = heap->allocate(1U);
    REQUIRE(((cs::g_cnt_enter == 1) && (cs::g_cnt_leave == 1)));
    REQUIRE(mem != nullptr);
    REQUIRE(heap->getDiagnostics().oom_count == 0);
    REQUIRE(heap->getDiagnostics().peak_allocated == Fragment::SizeMin);
    REQUIRE(heap->getDiagnostics().allocated == Fragment::SizeMin);
    REQUIRE(heap->getDiagnostics().peak_request_size == 1);
    REQUIRE(((cs::g_cnt_enter == 5) && (cs::g_cnt_leave == 5)));

    auto& frag = Fragment::constructFromAllocatedMemory(mem);
    REQUIRE(frag.header.size == (O1HEAP_ALIGNMENT * 2U));
    REQUIRE(frag.header.next != nullptr);
    REQUIRE(frag.header.prev == nullptr);
    REQUIRE(frag.header.used);
    REQUIRE(frag.header.next->header.size == (heap->diagnostics.capacity - frag.header.size));
    REQUIRE(!frag.header.next->header.used);

    heap->free(mem);
}

TEST_CASE("General: allocate: size_t overflow")
{
    using internal::Fragment;

    constexpr auto size_max = std::numeric_limits<std::size_t>::max();

    cs::resetCounters();

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays

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
}

TEST_CASE("General: free")
{
    using internal::Fragment;

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays

    auto heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);

    REQUIRE(nullptr == heap->allocate(0U));
    REQUIRE(heap->diagnostics.allocated == 0U);
    heap->free(nullptr);
    REQUIRE(heap->diagnostics.peak_allocated == 0U);
    REQUIRE(heap->diagnostics.peak_request_size == 0U);
    REQUIRE(heap->diagnostics.oom_count == 0U);

    cs::resetCounters();

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
        cs::validateAndReset(1);
        heap->matchFragments(reference);
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
            cs::validateAndReset(1);
        }
        else
        {
            heap->free(p);
            cs::validateAndReset(0);
        }
        REQUIRE(heap->diagnostics.allocated == allocated);
        REQUIRE(heap->diagnostics.peak_allocated == peak_allocated);
        REQUIRE(heap->diagnostics.peak_request_size == peak_request_size);
        heap->matchFragments(reference);
    };

    constexpr auto X = true;   // used
    constexpr auto O = false;  // free

    auto a = alloc(32U,
                   {
                       {X, 64U},
                       {O, 0U},
                   });
    auto b = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });
    auto c = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });
    auto d = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });
    auto e = alloc(1024U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {X, 2048U},
                       {O, 0U},
                   });
    auto f = alloc(512U,
                   {
                       {X, 64U},    // a
                       {X, 64U},    // b
                       {X, 64U},    // c
                       {X, 64U},    // d
                       {X, 2048U},  // e
                       {X, 1024U},  // f
                       {O, 0U},
                   });
    dealloc(b,
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},    // c
                {X, 64U},    // d
                {X, 2048U},  // e
                {X, 1024U},  // f
                {O, 0U},
            });
    dealloc(a,
            {
                {O, 128U},   // joined right
                {X, 64U},    // c
                {X, 64U},    // d
                {X, 2048U},  // e
                {X, 1024U},  // f
                {O, 0U},
            });
    dealloc(c,
            {
                {O, 192U},   // joined left
                {X, 64U},    // d
                {X, 2048U},  // e
                {X, 1024U},  // f
                {O, 0U},
            });
    dealloc(e,
            {
                {O, 192U},
                {X, 64U},  // d
                {O, 2048U},
                {X, 1024U},  // f
                {O, 0U},
            });
    auto g = alloc(400U,
                   {
                       {O, 192U},
                       {X, 64U},   // d
                       {X, 512U},  // g
                       {O, 1536U},
                       {X, 1024U},  // f
                       {O, 0U},
                   });
    dealloc(f,
            {
                {O, 192U},
                {X, 64U},   // d
                {X, 512U},  // g
                {O, 0U},    // joined left & right
            });
    dealloc(d,
            {
                {O, 256U},
                {X, 512U},  // g
                {O, 0U},
            });
    auto h = alloc(200U,
                   {
                       {X, 256U},  // h
                       {X, 512U},  // g
                       {O, 0U},
                   });
    auto i = alloc(32U,
                   {
                       {X, 256U},  // h
                       {X, 512U},  // g
                       {X, 64U},   // i
                       {O, 0U},
                   });
    dealloc(g,
            {
                {X, 256U},  // h
                {O, 512U},
                {X, 64U},  // i
                {O, 0U},
            });
    dealloc(h,
            {
                {O, 768U},
                {X, 64U},  // i
                {O, 0U},
            });
    dealloc(i,
            {
                {O, 0U},  // All heap is free.
            });

    REQUIRE(heap->diagnostics.allocated == 0U);
    REQUIRE(heap->diagnostics.peak_allocated == 3328U);
    REQUIRE(heap->diagnostics.peak_request_size == 1024U);
    REQUIRE(heap->diagnostics.oom_count == 0U);
}

#ifdef NDEBUG
TEST_CASE("General: free: heap corruption protection")
{
    using internal::Fragment;

    assert(false);  // Make sure assertion checks are disabled.

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays
    auto                         heap = init(arena.get(), ArenaSize);
    REQUIRE(heap != nullptr);

    const auto alloc = [&](const std::size_t amount, const std::vector<std::pair<bool, std::size_t>>& reference) {
        const auto p = heap->allocate(amount);
        if (p != nullptr)
        {
            // Overwrite all to ensure that the allocator does not make implicit assumptions about the memory use.
            std::generate_n(reinterpret_cast<std::byte*>(p), amount, getRandomByte);
        }
        heap->matchFragments(reference);
        return p;
    };

    const auto dealloc = [&](void* const p, const std::vector<std::pair<bool, std::size_t>>& reference) {
        INFO(heap->visualize());
        heap->free(p);
        heap->matchFragments(reference);
    };

    constexpr auto X = true;   // used
    constexpr auto O = false;  // free

    auto a = alloc(32U,
                   {
                       {X, 64U},
                       {O, 0U},
                   });
    auto b = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });
    auto c = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });
    auto d = alloc(32U,
                   {
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {X, 64U},
                       {O, 0U},
                   });

    CAPTURE(a, d, c, d);

    dealloc(b,
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},  // c
                {X, 64U},  // d
                {O, 0U},
            });

    // DOUBLE FREE
    dealloc(b,
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},  // c
                {X, 64U},  // d
                {O, 0U},
            });

    // BAD POINTERS
    dealloc(reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(a) + 1U),
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},  // c
                {X, 64U},  // d
                {O, 0U},
            });
    dealloc(reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(b) + 8U),
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},  // c
                {X, 64U},  // d
                {O, 0U},
            });
    dealloc(reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(d) - 1U),
            {
                {X, 64U},  // a
                {O, 64U},
                {X, 64U},  // c
                {X, 64U},  // d
                {O, 0U},
            });

    // RANDOM DATA INSIDE HEAP
    for (std::uint32_t i = 0; i < 1000U; i++)
    {
        const auto ptr = reinterpret_cast<std::byte*>(c);
        std::generate_n(ptr, 32U, getRandomByte);
        dealloc(ptr + 16U,
                {
                    {X, 64U},  // a
                    {O, 64U},
                    {X, 64U},  // c
                    {X, 64U},  // d
                    {O, 0U},
                });
    }

    // Deallocate C correctly. Heap is still working.
    dealloc(c,
            {
                {X, 64U},  // a
                {O, 128U},
                {X, 64U},  // d
                {O, 0U},
            });

    // RANDOM DATA OUTSIDE HEAP
    {
        std::array<std::byte, 100U> storage{};
        for (std::uint32_t i = 0; i < 1000U; i++)
        {
            std::generate_n(storage.data(), std::size(storage), getRandomByte);
            dealloc(storage.data(),
                    {
                        {X, 64U},  // a
                        {O, 128U},
                        {X, 64U},  // d
                        {O, 0U},
                    });
        }
    }

    // RANDOM POINTERS
    {
        std::random_device                         rd;
        std::mt19937                               gen(rd());
        std::uniform_int_distribution<std::size_t> dis;
        for (std::uint32_t i = 0; i < 1000U; i++)
        {
            dealloc(reinterpret_cast<void*>(dis(gen)),
                    {
                        {X, 64U},  // a
                        {O, 128U},
                        {X, 64U},  // d
                        {O, 0U},
                    });
        }
    }

    // Deallocate A and D correctly. Heap is still working.
    dealloc(a,
            {
                {O, 192U},
                {X, 64U},  // d
                {O, 0U},
            });
    dealloc(d,
            {
                {O, 0U},  // Empty heap.
            });
}
#endif

/// This test has been empirically tuned to expand its state space coverage.
/// If any new behaviors need to be tested, please consider writing another test instead of changing this one.
TEST_CASE("General: random A")
{
    using internal::Fragment;

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays
    auto                         heap = init(arena.get(), ArenaSize, cs::enter, cs::leave);
    REQUIRE(heap != nullptr);

    std::vector<void*> pointers;

    std::size_t   allocated         = 0U;
    std::size_t   peak_allocated    = 0U;
    std::size_t   peak_request_size = 0U;
    std::uint64_t oom_count         = 0U;

    std::random_device random_device;
    std::mt19937       random_generator(random_device());

    const auto allocate = [&]() {
        std::uniform_int_distribution<std::size_t> dis(0, ArenaSize / 1000U);

        const std::size_t amount = dis(random_generator) + 1U;
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
    };

    const auto deallocate = [&]() {
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
                frag.validateInvariants();
                REQUIRE(allocated >= frag.header.size);
                allocated -= frag.header.size;
            }
            heap->free(ptr);
        }
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

        std::cout << heap->visualize() << std::endl;
    }
}
