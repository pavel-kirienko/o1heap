# O(1) heap

[![Build Status](https://travis-ci.org/pavel-kirienko/o1heap.svg?branch=master)](https://travis-ci.org/pavel-kirienko/o1heap)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=pavel-kirienko_o1heap&metric=alert_status)](https://sonarcloud.io/dashboard?id=pavel-kirienko_o1heap)
[![Reliability Rating](https://sonarcloud.io/api/project_badges/measure?project=pavel-kirienko_o1heap&metric=reliability_rating)](https://sonarcloud.io/dashboard?id=pavel-kirienko_o1heap)

## Description

O1heap is a highly deterministic constant-complexity memory allocator designed for hard real-time embedded systems.
The name stands for *O(1) heap*.

The allocator offers
a constant worst-case execution time (WCET) and
a bounded worst-case memory fragmentation (consumption) (WCMC).
The allocator allows the designer to statically prove its temporal and spatial properties for a given application,
which makes it suitable for use in safety-critical systems.

The codebase is implemented in C99/C11 following MISRA C:2012, with several intended deviations which are unavoidable
due to the fact that a memory allocator has to rely on inherently unsafe operations to fulfill its purpose.
The codebase is extremely compact (<500 SLoC) and is therefore trivial to validate manually.

The allocator is designed to be portable across all conventional architectures, from 8-bit to 64-bit systems.
Multi-threaded environments are supported with the help of external synchronization hooks provided by the application.

## Design

This implementation derives or is based on the ideas presented in:

- "Timing-Predictable Memory Allocation In Hard Real-Time Systems" -- J. Herter, 2014.
- "An algorithm with constant execution time for dynamic storage allocation" -- T. Ogasawara, 1995.

The allocator implements the Half-Fit algorithm proposed by Ogasawara
with a crucial modification -- memory is allocated in fragments of size:

s(r) = 2<sup>ceil(log<sub>2</sub>(r+h))</sup>-h

Where *r* is the requested allocation size and *h* is the fixed per-allocation overhead imposed by the allocator
for memory management needs.
The size of the overhead *h* is represented in the codebase as `O1HEAP_ALIGNMENT`,
because it also dictates the allocated memory pointer alignment.
The rounding up to the power of 2 is done to ensure that WCMC is bounded [Herter 2014].
This results in an increased memory consumption in the average case, but this is found to be tolerable because this
implementation is intended for highly deterministic real-time systems where the average-case performance metrics
are less relevant than in general-purpose applications.

The caching-related issues are considered in the design: the core Half-Fit algorithm is inherently optimized to
minimize the number of random memory accesses; furthermore, the allocation strategy favors least recently used memory
fragments to minimize cache misses in the application.

The allocation and deallocation routines are strictly linear and contain neither loops nor recursive calls.
In order to further improve the real-time performance, manual branch hinting is used,
allowing the compiler to generate code that is optimized for the longest path, thus reducing WCET.

TODO: document how to compute the memory requirements.

## Usage

### Integration

Copy the files `o1heap.c` and `o1heap.h` (find them under `o1heap/`) into your project tree.
Either keep them in the same directory, or make sure that the directory that contains the header
is added to the set of include look-up paths.

Dedicate a memory arena for the heap, and pass a pointer to it along with its size to the initialization function
`o1heapInit(...)`.
In the case of concurrent environments, also pass pointers to the synchronization locking/unlocking functions
-- they will be invoked by the library to facilitate atomic transactions.
Alternatively, some applications (where possible) might benefit from using a separate heap per thread to avoid
the synchronization overhead and reduce contention.

Allocate and deallocate memory using `o1heapAllocate(...)` and `o1heapDeallocate()`.
Their semantics are compatible with `malloc()` and `free()` plus additional behavioral guarantees
(constant timing, bounded fragmentation, protections against double-free and heap corruption in `free()`).

### Build configuration options

The preprocessor options given below can be overridden (e.g., using the `-D` compiler flag, depending on the compiler)
to fine-tune the implementation.
None of them are mandatory to use.

#### O1HEAP_ASSERT(x)

The macro `O1HEAP_ASSERT(x)` can be defined to customize the assertion handling or to disable it.
To disable assertion checks, the macro should expand into `(void)(x)`.

If not specified, the macro expands into the standard assertion check macro `assert(x)` as defined in `<assert.h>`.

#### O1HEAP_LIKELY(x)

Some of the conditional branching statements are equipped with this annotation to hint the compiler that
the generated code should be optimized for the case where the corresponding branch is taken.
The macro should expand into a compiler-specific branch weighting intrinsic,
or into the original expression `(x)` if no such hinting is desired.

If not specified, the macro expands as follows:

- For some well-known compilers the macro automatically expands into appropriate branch weighting intrinsics.
For example, for GCC, Clang, and ARM Compiler, it expands into `__builtin_expect((x), 1)`.
- For other compilers it expands into the original expression with no modifications: `(x)`.

## Development

### Coding conventions

The codebase shall follow the [Zubax C/C++ Coding Conventions](https://kb.zubax.com/x/84Ah).
The compliance is enforced with the help of the scripts located under `scripts/`.

### Testing

Please refer to the continuous integration configuration to see how to invoke the tests.

### Static analysis

Find the respective scripts under `scripts/`.

### MISRA compliance

MISRA compliance is enforced with the help of the following tools:

- Clang-Tidy, invokable via the script under `scripts/`.
- SonarCloud, invoked as part of the continuous integration build.

Every intentional deviation shall be documented and justified in-place using the following notation,
followed by the appropriate static analyser warning suppression statement:

```c
// Intentional violation of MISRA: <valid reason here>
// NOSONAR
// NOLINT
```

The list of intentional deviations can be obtained by simply searching the codebase for the above comments.

## License

The library is available under the terms of the MIT License.
Please find it in the file `LICENSE`.
