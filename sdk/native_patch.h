#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace swml4::patch {

struct Registers { // in stack order; rsp isn't included, handlers must not move the stack
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax, flags;
};
using Handler = void (*)(Registers&);
struct XmmRegisters { // xmm0 to xmm5, four floats each. Changes stick.
    float xmm[6][4];
};
using XmmHandler = void (*)(Registers&, XmmRegisters&);

inline uint8_t* allocate_near(uintptr_t target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    for (uintptr_t distance = step; distance < 0x70000000; distance += step)
        for (uintptr_t address : {target - distance, target + distance}) {
            address &= ~(step - 1);
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(memory);
        }
    return nullptr;
}

inline bool write_code(void* at, const void* bytes, size_t size) {
    DWORD old;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

inline bool mid_hook(uint8_t* at, const uint8_t* expected, size_t size, Handler handler) {
    if (!at || !handler || size < 5 || size > 32 || memcmp(at, expected, size) != 0) return false;
    uint8_t* cave = allocate_near(reinterpret_cast<uintptr_t>(at), 256);
    if (!cave) return false;
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    memcpy(p, expected, size);
    p += size;
    emit({0x9C});                                                       // pushfq
    emit({0x50, 0x53, 0x51, 0x52, 0x55, 0x56, 0x57});
    emit({0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
    emit({0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});             // push r12 r13 r14 r15
    emit({0x48, 0x89, 0xE1});                                           // mov rcx, rsp (Registers*)
    emit({0x48, 0x89, 0xE5});                                           // mov rbp, rsp
    emit({0x48, 0x83, 0xEC, 0x60});                                     // sub rsp, 0x60
    emit({0x48, 0x83, 0xE4, 0xF0});                                     // and rsp, -16
    for (uint8_t i = 0; i < 6; ++i)                                     // movups [rsp+i*16], xmmi
        emit({0x0F, 0x11, uint8_t(0x44 | i << 3), 0x24, uint8_t(i * 16)});
    emit({0x48, 0x89, 0xE2});
    emit({0x48, 0x83, 0xEC, 0x20});                                     // sub rsp, 0x20 (shadow space)
    emit({0x48, 0xB8});                                                 // mov rax, handler
    memcpy(p, &handler, 8);
    p += 8;
    emit({0xFF, 0xD0});                                                 // call rax
    emit({0x48, 0x83, 0xC4, 0x20});                                     // add rsp, 0x20
    for (uint8_t i = 0; i < 6; ++i)                                     // movups xmmi, [rsp+i*16]
        emit({0x0F, 0x10, uint8_t(0x44 | i << 3), 0x24, uint8_t(i * 16)});
    emit({0x48, 0x89, 0xEC});                                           // mov rsp, rbp
    emit({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C});             // pop r15 r14 r13 r12
    emit({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58});
    emit({0x5F, 0x5E, 0x5D, 0x5A, 0x59, 0x5B, 0x58});
    emit({0x9D});                                                       // popfq
    const auto back = reinterpret_cast<intptr_t>(at + size) - reinterpret_cast<intptr_t>(p + 5);
    emit({0xE9});
    memcpy(p, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), cave, 256);

    uint8_t jump[32];
    memset(jump, 0x90, sizeof jump); // nop out whatever the 5-byte jump leaves over
    const auto to = reinterpret_cast<intptr_t>(cave) - reinterpret_cast<intptr_t>(at + 5);
    jump[0] = 0xE9;
    memcpy(jump + 1, &to, 4);
    return write_code(at, jump, size);
}
inline bool mid_hook(uint8_t* at, const uint8_t* expected, size_t size, XmmHandler handler) {
    return mid_hook(at, expected, size, reinterpret_cast<Handler>(handler));
}

} // namespace swml4::patch
