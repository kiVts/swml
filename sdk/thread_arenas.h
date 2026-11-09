#pragma once

#include <windows.h>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

class ThreadArenas {
public:
    static constexpr size_t kMaxRequest = size_t(1) << 20;
    static constexpr int kClasses = 64;

    // Reserves `arenas` slices of 8 GB of address space each (2 or 1 GB if that can't be reserved).
    bool reserve(int arenas)
    {
        for (size_t gb : {8, 2, 1}) {
            size_t slice = gb << 30;
            void* b = VirtualAlloc(nullptr, slice * size_t(arenas), MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
            if (!b) continue;
            base_ = static_cast<uint8_t*>(b);
            slice_ = slice;
            count_ = arenas;
            arenas_ = std::make_unique<Arena[]>(size_t(arenas));
            for (int i = 0; i < arenas; i++) {
                arenas_[i].bump = arenas_[i].committed = base_ + slice * size_t(i);
                arenas_[i].end = base_ + slice * size_t(i + 1);
            }
            limit_ = base_ + slice * size_t(arenas);
            return true;
        }
        return false;
    }
    bool on() const { return limit_ > base_; }
    const void* base() const { return base_; }
    size_t slice() const { return slice_; }
    int count() const { return count_; }
    bool owns(const void* p) const { return p >= base_ && p < limit_; }

    // Block from arena `index`, or null (too big / slice full).
    void* alloc(int index, size_t n)
    {
        if (n > kMaxRequest) return nullptr;
        if (n == 0) n = 1;
        int c = class_of(n + sizeof(Header));
        size_t size = class_size(c);
        Arena& a = arenas_[index];
        AcquireSRWLockExclusive(&a.lock);
        void* block = a.free_list[c];
        if (block) {
            a.free_list[c] = *static_cast<void**>(block);
        } else {
            if (a.bump + size > a.committed) {
                size_t grow = std::max(kCommitStep, (size + kCommitStep - 1) & ~(kCommitStep - 1));
                if (a.committed + grow > a.end || !VirtualAlloc(a.committed, grow, MEM_COMMIT, PAGE_READWRITE)) {
                    ReleaseSRWLockExclusive(&a.lock);
                    return nullptr;
                }
                a.committed += grow;
            }
            block = a.bump;
            a.bump += size;
        }
        a.in_use += size;
        a.allocs++;
        ReleaseSRWLockExclusive(&a.lock);
        auto* h = static_cast<Header*>(block);
        h->size = n;
        h->cls = uint32_t(c);
        return h + 1;
    }

    // p must be owns(p). Any thread.
    void free(void* p)
    {
        Header* h = header(p);
        int c = int(h->cls);
        Arena& a = arenas_[size_t(reinterpret_cast<uint8_t*>(h) - base_) / slice_];
        AcquireSRWLockExclusive(&a.lock);
        *reinterpret_cast<void**>(h) = a.free_list[c];
        a.free_list[c] = h;
        a.in_use -= class_size(c);
        ReleaseSRWLockExclusive(&a.lock);
    }

    size_t size(const void* p) const { return size_t(header(p)->size); }
    size_t capacity(const void* p) const { return class_size(int(header(p)->cls)) - sizeof(Header); }
    // Grow / shrink within the block's class; false = doesn't fit.
    bool resize_in_place(void* p, size_t n)
    {
        if (n == 0 || n > capacity(p)) return false;
        header(p)->size = n;
        return true;
    }

    void stats(size_t& in_use, size_t& committed, unsigned long long& allocs) const
    {
        in_use = committed = 0;
        allocs = 0;
        for (int i = 0; i < count_; i++) {
            Arena& a = arenas_[i];
            AcquireSRWLockShared(&a.lock);
            in_use += a.in_use;
            committed += size_t(a.committed - (a.end - slice_));
            allocs += a.allocs;
            ReleaseSRWLockShared(&a.lock);
        }
    }

    // Block size (header included) -> class: 16..64 in steps of 16, then 4 steps per power of two (all multiples
    // of 16).
    static int class_of(size_t total)
    {
        if (total <= 64) return int((total + 15) / 16) - 1;
        int e = int(std::bit_width(total - 1)) - 1; // total in (2^e, 2^(e+1)]
        int sub = int(((total - 1) >> (e - 2)) & 3);
        return 4 + (e - 6) * 4 + sub;
    }
    static size_t class_size(int c)
    {
        if (c < 4) return size_t(c + 1) * 16;
        int e = (c - 4) / 4 + 6, sub = (c - 4) % 4;
        return (size_t(1) << e) + size_t(sub + 1) * (size_t(1) << (e - 2));
    }

private:
    static constexpr size_t kCommitStep = size_t(1) << 20;
    struct Header {
        uint64_t size; // requested
        uint32_t cls;
        uint32_t pad;
    };
    static_assert(sizeof(Header) == 16);
    struct Arena {
        mutable SRWLOCK lock = SRWLOCK_INIT;
        uint8_t* bump = nullptr;
        uint8_t* committed = nullptr;
        uint8_t* end = nullptr;
        void* free_list[kClasses] = {};
        size_t in_use = 0;
        unsigned long long allocs = 0;
    };
    static Header* header(const void* p) { return const_cast<Header*>(static_cast<const Header*>(p) - 1); }

    uint8_t* base_ = nullptr;
    uint8_t* limit_ = nullptr;
    size_t slice_ = 0;
    int count_ = 0;
    std::unique_ptr<Arena[]> arenas_;
};
