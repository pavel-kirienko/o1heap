# o1heap

Add badges.

## Description

This is a highly deterministic constant-complexity memory allocator designed for hard real-time embedded systems.
The name stands for *O(1) heap*.

The allocator offers
a constant worst-case execution time (WCET) and
a bounded worst-case memory fragmentation (consumption) (WCMF).
The allocator allows the designer to statically prove its temporal and spatial properties for a given application,
which makes it suitable for use in safety-critical systems.

The codebase is implemented in C99/C11 following MISRA C:2012, with several intended deviations which are unavoidable
due to the fact that a memory allocator has to rely on inherently unsafe operations to fulfill its purpose.
The codebase is extremely compact (<500 SLoC) and is therefore trivial to validate manually.

The allocator is designed to be portable across all conventional architectures, from 8-bit to 64-bit systems.
Multi-threaded environments are supported with the help of external synchronization hooks provided by the application.

TODO: document how to compute the memory requirements.

## Usage

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
(constant timing and bounded fragmentation).

## License

The library is available under the terms of the MIT License.
Please find it in the file `LICENSE`.
