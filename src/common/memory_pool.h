#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "types.h"

namespace hft {

// Fixed-capacity object pool with O(1) allocate and O(1) release.
//
// Free slots are threaded onto an intrusive singly-linked free list. Because
// `storage` is the first member of Slot, the T* handed out by allocate() is
// bit-identical to the Slot* it lives in, so release() recovers the owning slot
// by pointer arithmetic -- no scan. This keeps the hot path allocation-free and
// branch-light (no malloc, no search).
template<typename T, std::uint64_t poolSize>
class MemoryPool {
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        bool  isUsed = false;
        Slot* next   = nullptr;
    };

    static_assert(offsetof(Slot, storage) == 0,
                  "storage must be the first member so a T* maps back to its Slot");

    Slot*             freeSlot = nullptr;
    std::vector<Slot> slots;
    std::size_t       used = 0;

    Slot* slotOf(T* ptr) noexcept {
        return reinterpret_cast<Slot*>(reinterpret_cast<unsigned char*>(ptr) -
                                       offsetof(Slot, storage));
    }

public:
    MemoryPool() : slots(poolSize) {
        if (poolSize == 0) return;
        for (std::uint64_t i = 0; i + 1 < poolSize; ++i)
            slots[i].next = &slots[i + 1];
        slots[poolSize - 1].next = nullptr;
        freeSlot = &slots[0];
    }

    template<typename... Args>
    T* allocate(Args&&... args) {
        if (!freeSlot) return nullptr;
        Slot* slot = freeSlot;
        freeSlot = freeSlot->next;
        slot->isUsed = true;
        ++used;
        return new (slot->storage) T(std::forward<Args>(args)...);
    }

    void release(T* ptr) {
        if (!ptr) return;
        Slot* slot = slotOf(ptr);
        assert(slot >= slots.data() && slot < slots.data() + poolSize &&
               "pointer does not belong to this pool");
        if (!slot->isUsed) return;  // guard against double free

        ptr->~T();
        slot->isUsed = false;
        slot->next = freeSlot;
        freeSlot = slot;
        --used;
    }

    std::size_t size() const noexcept { return used; }
    std::size_t capacity() const noexcept { return poolSize; }
};

}  // namespace hft
