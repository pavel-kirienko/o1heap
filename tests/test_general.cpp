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
#include "catch.hpp"
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
    // Fill the beginning of the arena with random bytes (the entire arena may be too slow to fill).
    std::generate_n(reinterpret_cast<std::byte*>(base), std::min<std::size_t>(10'000U, size), getRandomByte);

    const auto heap = reinterpret_cast<internal::O1HeapInstance*>(
        o1heapInit(base, size, critical_section_enter, critical_section_leave));

    if (heap != nullptr)
    {
        REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

        REQUIRE(heap->critical_section_enter == critical_section_enter);
        REQUIRE(heap->critical_section_leave == critical_section_leave);

        // std::cout << "arena_size=" << size << "; "
        //           << "capacity=" << heap->diagnostics.capacity << "; "
        //           << "nonempty_bin_mask=" << heap->nonempty_bin_mask << std::endl;
        REQUIRE(heap->nonempty_bin_mask > 0U);
        REQUIRE((heap->nonempty_bin_mask & (heap->nonempty_bin_mask - 1U)) == 0);
        for (auto i = 0U; i < std::size(heap->bins); i++)
        {
            const std::size_t min = internal::FragmentSizeMin << i;
            const std::size_t max = (internal::FragmentSizeMin << i) * 2U - 1U;
            if ((heap->nonempty_bin_mask & (1ULL << i)) == 0U)
            {
                REQUIRE(heap->bins[i] == nullptr);
            }
            else
            {
                REQUIRE(heap->bins[i] != nullptr);
                REQUIRE(heap->bins[i]->header.size >= min);
                REQUIRE(heap->bins[i]->header.size <= max);
                // std::cout << "bin[" << i << "]=" << heap->bins[i] << " (" << min << ".." << max << ")" << std::endl;
            }
        }

        REQUIRE(heap->diagnostics.capacity < size);
        REQUIRE(heap->diagnostics.capacity <= internal::FragmentSizeMax);
        REQUIRE(heap->diagnostics.capacity > internal::FragmentSizeMin);
        REQUIRE(heap->diagnostics.allocated == 0);
        REQUIRE(heap->diagnostics.oom_count == 0);
        REQUIRE(heap->diagnostics.peak_allocated == 0);
        REQUIRE(heap->diagnostics.peak_request_size == 0);

        const auto root_fragment = heap->bins[log2Floor(heap->nonempty_bin_mask)];
        REQUIRE(root_fragment != nullptr);
        REQUIRE(root_fragment->next_free == nullptr);
        REQUIRE(!root_fragment->header.used);
        REQUIRE(root_fragment->header.size == heap->diagnostics.capacity);
        REQUIRE(root_fragment->header.next == nullptr);
        REQUIRE(root_fragment->header.prev == nullptr);
    }
    return heap;
}

auto allocate(internal::O1HeapInstance* const handle, const size_t amount)
{
    return o1heapAllocate(reinterpret_cast<O1HeapInstance*>(handle), amount);
}

auto deallocate(internal::O1HeapInstance* const handle, void* const pointer)
{
    return o1heapFree(reinterpret_cast<O1HeapInstance*>(handle), pointer);
}

auto getDiagnostics(const internal::O1HeapInstance* const handle)
{
    return o1heapGetDiagnostics(reinterpret_cast<const O1HeapInstance*>(handle));
}

}  // namespace

TEST_CASE("General: init")
{
    std::cout << "sizeof(void*)=" << sizeof(void*) << "; sizeof(O1HeapInstance)=" << sizeof(internal::O1HeapInstance)
              << std::endl;

    alignas(128) std::array<std::byte, 10'000U> arena{};

    REQUIRE(nullptr == init(nullptr, 0U, nullptr, nullptr));
    REQUIRE(nullptr == init(nullptr, 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == init(arena.data(), 0U, nullptr, nullptr));
    REQUIRE(nullptr == init(arena.data(), 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == init(arena.data(), 99U, nullptr, nullptr));  // Too small.
    REQUIRE(nullptr == init(arena.data(), 99U, &cs::enter, &cs::leave));

    // TODO: test O1HEAP_CAPACITY_LIMIT

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
                REQUIRE(size <= sizeof(internal::O1HeapInstance) * 2U + internal::FragmentSizeMin * 2U);
            }
            else
            {
                REQUIRE(size >= sizeof(internal::O1HeapInstance) + internal::FragmentSizeMin);
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
    REQUIRE(getDiagnostics(heap).capacity > ArenaSize - 1024U);
    REQUIRE(getDiagnostics(heap).capacity < ArenaSize);
    REQUIRE(getDiagnostics(heap).oom_count == 0);
    REQUIRE(cs::g_cnt_enter == 3);
    REQUIRE(cs::g_cnt_enter == 3);

    REQUIRE(nullptr == allocate(heap, ArenaSize));  // Too large
    REQUIRE(getDiagnostics(heap).oom_count == 1);
    REQUIRE(cs::g_cnt_enter == 5);
    REQUIRE(cs::g_cnt_enter == 5);

    REQUIRE(nullptr == allocate(heap, ArenaSize - O1HEAP_ALIGNMENT));  // Too large
    REQUIRE(getDiagnostics(heap).oom_count == 2);

    REQUIRE(nullptr == allocate(heap, heap->diagnostics.capacity - O1HEAP_ALIGNMENT + 1U));  // Too large
    REQUIRE(getDiagnostics(heap).oom_count == 3);

    REQUIRE(nullptr == allocate(heap, ArenaSize * 10U));  // Too large
    REQUIRE(getDiagnostics(heap).oom_count == 4);

    REQUIRE(nullptr == allocate(heap, 0));         // Nothing to allocate
    REQUIRE(getDiagnostics(heap).oom_count == 4);  // Not incremented! Zero allocation is not an OOM.

    REQUIRE(getDiagnostics(heap).peak_allocated == 0);
    REQUIRE(getDiagnostics(heap).allocated == 0);
    REQUIRE(getDiagnostics(heap).peak_request_size == ArenaSize * 10U);

    REQUIRE(nullptr != allocate(heap, MiB256 - O1HEAP_ALIGNMENT));  // Maximum possible allocation.
    REQUIRE(getDiagnostics(heap).oom_count == 4);                   // OOM counter not incremented.
    REQUIRE(getDiagnostics(heap).peak_allocated == MiB256);
    REQUIRE(getDiagnostics(heap).allocated == MiB256);
    REQUIRE(getDiagnostics(heap).peak_request_size == ArenaSize * 10U);  // Same size -- that one was unsuccessful.
}

TEST_CASE("General: allocate: smallest")
{
    cs::resetCounters();

    constexpr auto               ArenaSize = MiB * 300U;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);  // NOLINT avoid-c-arrays

    auto heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);

    void* const mem = allocate(heap, 1U);
    REQUIRE(((cs::g_cnt_enter == 1) && (cs::g_cnt_leave == 1)));
    REQUIRE(mem != nullptr);
    REQUIRE(getDiagnostics(heap).oom_count == 0);
    REQUIRE(getDiagnostics(heap).peak_allocated == internal::FragmentSizeMin);
    REQUIRE(getDiagnostics(heap).allocated == internal::FragmentSizeMin);
    REQUIRE(getDiagnostics(heap).peak_request_size == 1);
    REQUIRE(((cs::g_cnt_enter == 5) && (cs::g_cnt_leave == 5)));

    auto frag = internal::Fragment::constructFromAllocatedMemory(mem);
    REQUIRE(frag.header.size == (O1HEAP_ALIGNMENT * 2U));
    REQUIRE(frag.header.next != nullptr);
    REQUIRE(frag.header.prev == nullptr);
    REQUIRE(frag.header.used);
    REQUIRE(frag.header.next->header.size == (heap->diagnostics.capacity - frag.header.size));
    REQUIRE(!frag.header.next->header.used);

    deallocate(heap, mem);
}

TEST_CASE("General: allocate: size_t overflow")
{
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
        REQUIRE(nullptr == allocate(heap, size_max / i));
        REQUIRE(nullptr == allocate(heap, size_max / i + 1U));  // May overflow to 0.
        REQUIRE(nullptr == allocate(heap, size_max / i - 1U));
        REQUIRE(nullptr == allocate(heap, internal::FragmentSizeMax - O1HEAP_ALIGNMENT + 1U));
    }

    // Over-commit the arena -- it is SMALLER than the size we're providing; it's an UB but for a test it's acceptable.
    heap = init(arena.get(), size_max);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->diagnostics.capacity == internal::FragmentSizeMax);
    for (auto i = 1U; i <= 2U; i++)
    {
        REQUIRE(nullptr == allocate(heap, size_max / i));
        REQUIRE(nullptr == allocate(heap, size_max / i + 1U));
        REQUIRE(nullptr == allocate(heap, size_max / i - 1U));
        REQUIRE(nullptr == allocate(heap, internal::FragmentSizeMax - O1HEAP_ALIGNMENT + 1U));
    }

    // Make sure the max-sized fragments are allocatable.
    void* const mem = allocate(heap, internal::FragmentSizeMax - O1HEAP_ALIGNMENT);
    REQUIRE(mem != nullptr);

    auto frag = internal::Fragment::constructFromAllocatedMemory(mem);
    REQUIRE(frag.header.size == internal::FragmentSizeMax);
    REQUIRE(frag.header.next == nullptr);
    REQUIRE(frag.header.prev == nullptr);
    REQUIRE(frag.header.used);

    REQUIRE(getDiagnostics(heap).peak_allocated == internal::FragmentSizeMax);
    REQUIRE(getDiagnostics(heap).allocated == internal::FragmentSizeMax);

    REQUIRE(heap->nonempty_bin_mask == 0);
    REQUIRE(std::all_of(std::begin(heap->bins), std::end(heap->bins), [](auto* p) { return p == nullptr; }));
}
