// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Implementation of 64-bit atomic helper functions for PowerPC 32-bit (Broadway / Wii)
#include <stdint.h>

extern "C" {

static uint32_t atomic_lock = 0;

static inline void acquire_lock() {
    uint32_t tmp;
    __asm__ __volatile__(
        "1: lwarx %0, 0, %1\n"
        "   cmpwi %0, 0\n"
        "   bne- 1b\n"
        "   stwcx. %2, 0, %1\n"
        "   bne- 1b\n"
        "   isync\n"
        : "=&r"(tmp)
        : "r"(&atomic_lock), "r"(1)
        : "cr0", "memory"
    );
}

static inline void release_lock() {
    __asm__ __volatile__(
        "eieio\n"
        : : : "memory"
    );
    atomic_lock = 0;
}

uint64_t __atomic_load_8(const volatile void *ptr, int memorder) {
    acquire_lock();
    uint64_t val = *(const volatile uint64_t *)ptr;
    release_lock();
    return val;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder) {
    acquire_lock();
    *(volatile uint64_t *)ptr = val;
    release_lock();
}

uint64_t __atomic_fetch_add_8(volatile void *ptr, uint64_t val, int memorder) {
    acquire_lock();
    uint64_t prev = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = prev + val;
    release_lock();
    return prev;
}

uint64_t __atomic_fetch_sub_8(volatile void *ptr, uint64_t val, int memorder) {
    acquire_lock();
    uint64_t prev = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = prev - val;
    release_lock();
    return prev;
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder) {
    acquire_lock();
    uint64_t prev = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = val;
    release_lock();
    return prev;
}

bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired, bool weak, int success_memorder, int failure_memorder) {
    acquire_lock();
    uint64_t cur = *(volatile uint64_t *)ptr;
    uint64_t exp = *(uint64_t *)expected;
    if (cur == exp) {
        *(volatile uint64_t *)ptr = desired;
        release_lock();
        return true;
    } else {
        *(uint64_t *)expected = cur;
        release_lock();
        return false;
    }
}

} // extern "C"
