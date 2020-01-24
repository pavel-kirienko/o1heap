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

#ifndef O1HEAP_TESTS_INTERNAL_HPP_INCLUDED
#define O1HEAP_TESTS_INTERNAL_HPP_INCLUDED

#include <o1heap.h>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <exception>

/// Definitions that are not exposed by the library but that are needed for testing.
/// Please keep them in sync with the library by manually updating as necessary.
namespace internal
{
extern "C" {
bool         isPowerOf2(const std::size_t x);
std::uint8_t log2Floor(const std::size_t x);
std::uint8_t log2Ceil(const std::size_t x);
std::size_t  pow2(const std::uint8_t power);
void         invokeHook(const O1HeapHook hook);
}

constexpr auto SmallestFragmentSize = O1HEAP_ALIGNMENT * 2U;

struct Fragment;

struct FragmentHeader final
{
    Fragment*   next = nullptr;
    Fragment*   prev = nullptr;
    std::size_t size = 0U;
    bool        used = false;
};

struct Fragment final
{
    FragmentHeader header;
    Fragment*      next_free = nullptr;

    static const Fragment& constructFromAllocatedMemory(const void* const memory)
    {
        if ((memory == nullptr) || (reinterpret_cast<std::size_t>(memory) <= O1HEAP_ALIGNMENT) ||
            (reinterpret_cast<std::size_t>(memory) % O1HEAP_ALIGNMENT) != 0U)
        {
            throw std::invalid_argument("Invalid pointer");
        }
        return *reinterpret_cast<const Fragment*>(
            reinterpret_cast<const void*>(reinterpret_cast<const std::byte*>(memory) - O1HEAP_ALIGNMENT));
    }
};

struct O1HeapInstance final
{
    std::array<Fragment*, sizeof(std::size_t) * 8U> bins{};

    std::size_t nonempty_bin_mask = 0;

    O1HeapHook critical_section_enter = nullptr;
    O1HeapHook critical_section_leave = nullptr;

    O1HeapDiagnostics diagnostics{};
};

}  // namespace internal

#endif  // O1HEAP_TESTS_INTERNAL_HPP_INCLUDED
