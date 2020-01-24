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
            const std::size_t min = internal::SmallestBlockSize << i;
            const std::size_t max = (internal::SmallestBlockSize << i) * 2U - 1U;
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

}  // namespace

TEST_CASE("General, init")
{
    std::cout << "sizeof(void*)=" << sizeof(void*) << "; sizeof(O1HeapInstance)=" << sizeof(internal::O1HeapInstance)
              << std::endl;

    alignas(128) std::array<std::uint8_t, 1024U> arena{};

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
            auto heap = init(arena.data() + offset,
                             size,
                             (offset % 2U == 0U) ? &cs::enter : nullptr,
                             (offset % 4U == 0U) ? &cs::leave : nullptr);
            if (heap == nullptr)
            {
                REQUIRE(size <= sizeof(internal::O1HeapInstance) * 2U + internal::SmallestBlockSize * 2U);
            }
            else
            {
                REQUIRE(size >= sizeof(internal::O1HeapInstance) + internal::SmallestBlockSize);
                REQUIRE(reinterpret_cast<std::size_t>(heap) >= reinterpret_cast<std::size_t>(arena.data()));
                REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);
            }
        }
    }

    REQUIRE(cs::g_cnt_enter == 0);
    REQUIRE(cs::g_cnt_leave == 0);
}
