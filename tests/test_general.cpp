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
#include <catch.hpp>
#include <iostream>
#include <limits>
#include <random>
#include <array>

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

}  // namespace
}  // namespace cs

namespace
{
template <typename T>
std::enable_if_t<std::is_integral_v<T>, std::uint8_t> log2Floor(const T& x)
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

std::byte getRandomByte()
{
    static std::random_device                           rd;
    static std::mt19937                                 gen(rd());
    static std::uniform_int_distribution<std::uint16_t> dis(0, 255U);
    return static_cast<std::byte>(dis(gen));
}

auto init(void* const       base,
          const std::size_t size,
          const O1HeapHook  critical_section_enter,
          const O1HeapHook  critical_section_leave)
{
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
            const std::size_t min = internal::SmallestFragmentSize << i;
            const std::size_t max = (internal::SmallestFragmentSize << i) * 2U - 1U;
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

        REQUIRE(heap->diagnostics.allocated == 0);
        REQUIRE(heap->diagnostics.oom_count == 0);
        REQUIRE(heap->diagnostics.peak_allocated == 0);
        REQUIRE(heap->diagnostics.peak_total_request_size == 0);

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

void* allocate(internal::O1HeapInstance* const handle, const size_t amount)
{
    return o1heapAllocate(reinterpret_cast<O1HeapInstance*>(handle), amount);
}

void free(internal::O1HeapInstance* const handle, void* const pointer)
{
    return o1heapFree(reinterpret_cast<O1HeapInstance*>(handle), pointer);
}

O1HeapDiagnostics getDiagnostics(const internal::O1HeapInstance* const handle)
{
    return o1heapGetDiagnostics(reinterpret_cast<const O1HeapInstance*>(handle));
}

}  // namespace

TEST_CASE("General, init")
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

    // Check various offsets and sizes to make sure the initialization is done correctly in all cases.
    for (auto offset = 0U; offset < 7U; offset++)
    {
        for (auto size = 99U; size < 5100U; size += 111U)
        {
            REQUIRE(arena.size() >= size);
            std::generate(std::begin(arena), std::end(arena), getRandomByte);
            auto heap = init(arena.data() + offset,
                             size - offset,
                             (offset % 2U == 0U) ? &cs::enter : nullptr,
                             (offset % 4U == 0U) ? &cs::leave : nullptr);
            if (heap == nullptr)
            {
                REQUIRE(size <= sizeof(internal::O1HeapInstance) * 2U + internal::SmallestFragmentSize * 2U);
            }
            else
            {
                REQUIRE(size >= sizeof(internal::O1HeapInstance) + internal::SmallestFragmentSize);
                REQUIRE(reinterpret_cast<std::size_t>(heap) >= reinterpret_cast<std::size_t>(arena.data()));
                REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);
            }
        }
    }

    REQUIRE(cs::g_cnt_enter == 0);
    REQUIRE(cs::g_cnt_leave == 0);
}

TEST_CASE("General, allocate")
{
    constexpr auto               ArenaSize = 1024ULL * 1024ULL * 300ULL;
    std::shared_ptr<std::byte[]> arena(new std::byte[ArenaSize]);
    std::generate_n(arena.get(), std::min(1024ULL, ArenaSize), getRandomByte);
    auto heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);

    REQUIRE(getDiagnostics(heap).oom_count == 0);
    REQUIRE(nullptr == allocate(heap, ArenaSize));  // Too large
    REQUIRE(getDiagnostics(heap).oom_count == 1);
    REQUIRE(nullptr == allocate(heap, ArenaSize * 10U));                                     // Too large
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max()));             // Check for overflow
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max() - 50U));       // Check for overflow
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max() / 2U));        // Check for overflow
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max() / 2U + 1U));   // Check for overflow
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max() / 2U - 1U));   // Check for overflow
    REQUIRE(nullptr == allocate(heap, std::numeric_limits<std::size_t>::max() / 2U - 50U));  // Check for overflow
    REQUIRE(getDiagnostics(heap).oom_count == 2);
    REQUIRE(nullptr == allocate(heap, 0));  // Nothing to allocate
    REQUIRE(getDiagnostics(heap).oom_count == 2);
    REQUIRE(getDiagnostics(heap).peak_allocated == 0);
    REQUIRE(getDiagnostics(heap).allocated == 0);
    REQUIRE(getDiagnostics(heap).peak_total_request_size > ArenaSize);

    heap = init(arena.get(), ArenaSize, &cs::enter, &cs::leave);
    REQUIRE(heap != nullptr);
    REQUIRE(getDiagnostics(heap).capacity > ArenaSize - 1024U);
    REQUIRE(getDiagnostics(heap).capacity < ArenaSize);
    REQUIRE(getDiagnostics(heap).oom_count == 0);
    REQUIRE(getDiagnostics(heap).peak_allocated == 0);
    REQUIRE(getDiagnostics(heap).allocated == 0);
    REQUIRE(getDiagnostics(heap).peak_total_request_size == 0);

    void* mem = allocate(heap, 1U);
    REQUIRE(mem != nullptr);
    REQUIRE(getDiagnostics(heap).oom_count == 0);
    REQUIRE(getDiagnostics(heap).peak_allocated == internal::SmallestFragmentSize);
    REQUIRE(getDiagnostics(heap).allocated == internal::SmallestFragmentSize);
    REQUIRE(getDiagnostics(heap).peak_total_request_size == internal::SmallestFragmentSize);

    free(heap, nullptr);  // No effect
    free(heap, mem);
    free(heap, nullptr);  // No effect
}
