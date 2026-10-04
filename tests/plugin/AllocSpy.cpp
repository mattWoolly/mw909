// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The one global allocator replacement of this test binary. See AllocSpy.h.

#include "AllocSpy.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local int armed = 0;
std::atomic<long> allocCount{0};
std::atomic<long> freeCount{0};

inline void noteAlloc() noexcept {
    if (armed > 0) allocCount.fetch_add(1, std::memory_order_relaxed);
}
inline void noteFree() noexcept {
    if (armed > 0) freeCount.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

#if defined(MW909_TEST_WRAPS_MALLOC)

// With -Wl,--wrap=X every reference to X in this link resolves to __wrap_X and
// __real_X names the C library's own function.
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);
int __real_posix_memalign(void**, std::size_t, std::size_t);
void* __real_aligned_alloc(std::size_t, std::size_t);
void* __real_memalign(std::size_t, std::size_t);

void* __wrap_malloc(std::size_t n) {
    noteAlloc();
    return __real_malloc(n);
}
void* __wrap_calloc(std::size_t count, std::size_t size) {
    noteAlloc();
    return __real_calloc(count, size);
}
void* __wrap_realloc(void* p, std::size_t n) {
    noteAlloc();
    return __real_realloc(p, n);
}
void __wrap_free(void* p) {
    if (p != nullptr) noteFree();
    __real_free(p);
}
int __wrap_posix_memalign(void** out, std::size_t alignment, std::size_t n) {
    noteAlloc();
    return __real_posix_memalign(out, alignment, n);
}
void* __wrap_aligned_alloc(std::size_t alignment, std::size_t n) {
    noteAlloc();
    return __real_aligned_alloc(alignment, n);
}
void* __wrap_memalign(std::size_t alignment, std::size_t n) {
    noteAlloc();
    return __real_memalign(alignment, n);
}
}  // extern "C"

namespace {
// operator new / delete go straight to the real allocator so each C++
// allocation is counted once, not once here and again in the wrapper.
void* rawAlloc(std::size_t n) noexcept { return __real_malloc(n != 0 ? n : 1); }
void* rawAlignedAlloc(std::size_t n, std::size_t alignment) noexcept {
    void* p = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return __real_posix_memalign(&p, alignment, n != 0 ? n : 1) == 0 ? p : nullptr;
}
void rawFree(void* p) noexcept { __real_free(p); }
}  // namespace

#else

namespace {
void* rawAlloc(std::size_t n) noexcept { return std::malloc(n != 0 ? n : 1); }
void* rawAlignedAlloc(std::size_t n, std::size_t alignment) noexcept {
    void* p = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return posix_memalign(&p, alignment, n != 0 ? n : 1) == 0 ? p : nullptr;
}
void rawFree(void* p) noexcept { std::free(p); }
}  // namespace

#endif

void* operator new(std::size_t n) {
    noteAlloc();
    if (void* p = rawAlloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    noteAlloc();
    if (void* p = rawAlloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, std::align_val_t alignment) {
    noteAlloc();
    if (void* p = rawAlignedAlloc(n, static_cast<std::size_t>(alignment))) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t alignment) {
    noteAlloc();
    if (void* p = rawAlignedAlloc(n, static_cast<std::size_t>(alignment))) return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    noteAlloc();
    return rawAlloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    noteAlloc();
    return rawAlloc(n);
}

void operator delete(void* p) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete[](void* p) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete(void* p, std::size_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (p != nullptr) noteFree();
    rawFree(p);
}

namespace allocspy {

bool wrapsMalloc() noexcept {
#if defined(MW909_TEST_WRAPS_MALLOC)
    return true;
#else
    return false;
#endif
}

long allocations() noexcept { return allocCount.load(std::memory_order_relaxed); }
long deallocations() noexcept { return freeCount.load(std::memory_order_relaxed); }
void reset() noexcept {
    allocCount.store(0, std::memory_order_relaxed);
    freeCount.store(0, std::memory_order_relaxed);
}

Scope::Scope() noexcept { ++armed; }
Scope::~Scope() noexcept { --armed; }

}  // namespace allocspy
