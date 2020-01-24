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

#include <o1heap.h>
#include <catch.hpp>
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

}
}

TEST_CASE("General, init")
{
    alignas(128) std::array<std::uint8_t, 1024U> arena{};

    REQUIRE(nullptr == o1heapInit(nullptr, 0U, nullptr, nullptr));
    REQUIRE(nullptr == o1heapInit(nullptr, 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == o1heapInit(arena.data(), 0U, nullptr, nullptr));
    REQUIRE(nullptr == o1heapInit(arena.data(), 0U, &cs::enter, &cs::leave));
    REQUIRE(nullptr == o1heapInit(arena.data(), 99U, nullptr, nullptr));        // Too small.
    REQUIRE(nullptr == o1heapInit(arena.data(), 99U, &cs::enter, &cs::leave));

    auto heap = o1heapInit(arena.data() + 1U, 1000U, nullptr, &cs::leave);
    REQUIRE(reinterpret_cast<std::size_t>(heap) > reinterpret_cast<std::size_t>(arena.data() + 1U));
    REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

    heap = o1heapInit(arena.data() + 1U, 1000U, &cs::enter, nullptr);
    REQUIRE(reinterpret_cast<std::size_t>(heap) > reinterpret_cast<std::size_t>(arena.data() + 1U));
    REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

    heap = o1heapInit(arena.data() + 1U, 1000U, &cs::enter, &cs::leave);
    REQUIRE(reinterpret_cast<std::size_t>(heap) > reinterpret_cast<std::size_t>(arena.data() + 1U));
    REQUIRE(reinterpret_cast<std::size_t>(heap) % O1HEAP_ALIGNMENT == 0U);

    REQUIRE(cs::g_cnt_enter == 0);
    REQUIRE(cs::g_cnt_leave == 0);
}
