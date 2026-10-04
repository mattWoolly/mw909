// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Heap-activity spy for the realtime tests. While a Scope is alive on a
// thread, every allocation and release made on that thread is counted:
// operator new / delete (replaced in AllocSpy.cpp) and, on Linux, malloc /
// calloc / realloc / free / posix_memalign / aligned_alloc / memalign (wrapped
// at link time, because JUCE's HeapBlock calls malloc directly).

#pragma once

namespace allocspy {

// True when the C allocator is intercepted as well as operator new.
bool wrapsMalloc() noexcept;

long allocations() noexcept;    // counted since the last reset()
long deallocations() noexcept;
void reset() noexcept;

// Marks the current thread as "the audio thread" for its lifetime.
struct Scope {
    Scope() noexcept;
    ~Scope() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace allocspy
