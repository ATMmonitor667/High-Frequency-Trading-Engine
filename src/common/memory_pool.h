#pragma once

#include <cstdint>
#include <new>
#include <vector>

#include "types.h"

namespace hft {

template<typename T, std::uint64_t poolSize>
class MemoryPool {
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        bool  isUsed = false;
        Slot* next   = nullptr;
    };

    Slot*             freeSlot = nullptr;
    std::vector<Slot> slots;
    std::size_t       used = 0;

public:
    MemoryPool() : slots(poolSize) {
        if (poolSize == 0) return;
        for (std::uint64_t i = 0; i + 1 < poolSize; ++i)
            slots[i].next = &slots[i + 1];
        slots[poolSize - 1].next = nullptr;
        freeSlot = &slots[0];
    }

    T* allocate(uint64_t id, Side side, double price, long qty) {
        if (!freeSlot) return nullptr;
        Slot* slot = freeSlot;
        freeSlot = freeSlot->next;
        slot->isUsed = true;
        ++used;
        return new (slot->storage) T(id, side, price, qty);
    }

    void release(T* ptr) {
        if (!ptr) return;
        for (std::uint64_t i = 0; i < poolSize; ++i) {
            T* inSlot = reinterpret_cast<T*>(slots[i].storage);
            if (slots[i].isUsed && inSlot == ptr) {
                ptr->~T();
                slots[i].isUsed = false;
                slots[i].next = freeSlot;
                freeSlot = &slots[i];
                --used;
                return;
            }
        }
    }
};

}  // namespace hft
