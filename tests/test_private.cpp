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

TEST_CASE("Private: isPowerOf2")
{
    using internal::isPowerOf2;
    REQUIRE(isPowerOf2(0));  // Special case.
    REQUIRE(isPowerOf2(1));  // 2**0
    REQUIRE(isPowerOf2(2));  // 2**1
    REQUIRE(!isPowerOf2(3));
    REQUIRE(isPowerOf2(4));
    REQUIRE(!isPowerOf2(5));
    REQUIRE(!isPowerOf2(6));
    REQUIRE(!isPowerOf2(7));
    REQUIRE(isPowerOf2(8));
    REQUIRE(!isPowerOf2(9));
}

TEST_CASE("Private: log2")
{
    using internal::log2Floor;
    using internal::log2Ceil;
    REQUIRE(log2Floor(0) == 0);
    REQUIRE(log2Floor(1) == 0);
    REQUIRE(log2Floor(2) == 1);
    REQUIRE(log2Floor(3) == 1);
    REQUIRE(log2Floor(4) == 2);
    REQUIRE(log2Floor(30) == 4);
    REQUIRE(log2Floor(60) == 5);
    REQUIRE(log2Floor(64) == 6);

    REQUIRE(log2Ceil(0) == 0);
    REQUIRE(log2Ceil(1) == 0);
    REQUIRE(log2Ceil(2) == 1);
    REQUIRE(log2Ceil(3) == 2);
    REQUIRE(log2Ceil(4) == 2);
    REQUIRE(log2Ceil(30) == 5);
    REQUIRE(log2Ceil(60) == 6);
    REQUIRE(log2Ceil(64) == 6);
}

TEST_CASE("Private: pow2")
{
    using internal::pow2;
    REQUIRE(pow2(0) == 1);
    REQUIRE(pow2(1) == 2);
    REQUIRE(pow2(2) == 4);
    REQUIRE(pow2(3) == 8);
    REQUIRE(pow2(4) == 16);
    REQUIRE(pow2(5) == 32);
    REQUIRE(pow2(6) == 64);
    REQUIRE(pow2(7) == 128);
    REQUIRE(pow2(8) == 256);
    REQUIRE(pow2(9) == 512);
}
