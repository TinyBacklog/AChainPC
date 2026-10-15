// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

void EmitSpinLockLock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp, bool waitpkg) {
    Xbyak::Label start, loop;
    code.jmp(start, code.T_NEAR);
    code.L(loop);

    if (waitpkg) {
        // TODO: this is because we lack regalloc - so better to be safe :(
        code.push(Xbyak::util::rax);
        code.push(Xbyak::util::rbx);
        code.push(Xbyak::util::rdx);
        // TODO: This clobbers EAX and EDX did we tell the regalloc?
        // ARM ptr for address-monitoring

        // XBYAK BUG: code.umonitor(ptr); see issue #255
        // replace once xbyak has been fixed
        code.db(0xF3);
        if (ptr.getIdx() >= 8) code.db(0x41);
        code.db(0x0F); code.db(0xAE);
        code.db(uint8_t((3 << 6) | ((6 & 7) << 3) | (ptr.getIdx() & 7)));

        // tmp.bit[0] = 0: C0.1 | Slow Wakup | Better Savings
        // tmp.bit[0] = 1: C0.2 | Fast Wakup | Lesser Savings
        // edx:eax is implicitly used as a 64-bit deadline timestamp
        // Use the maximum so that we use the operating system's maximum
        // allowed wait time within the IA32_UMWAIT_CONTROL register
        // Enter power state designated by tmp and wait for a write to lock_ptr
        code.mov(Xbyak::util::eax, 0xFFFFFFFF);
        code.mov(Xbyak::util::edx, Xbyak::util::eax);
        // TODO: We can only be here because tmp is 1 already - however we repeatedly overwrite it...
        code.mov(Xbyak::util::ebx, 1);
        code.umwait(Xbyak::util::ebx);
        // CF == 1 if we hit the OS-timeout in IA32_UMWAIT_CONTROL without a write
        // CF == 0 if we exited the wait for any other reason
        code.pop(Xbyak::util::rdx);
        code.pop(Xbyak::util::rbx);
        code.pop(Xbyak::util::rax);
    } else {
        code.pause();
    }
    code.L(start);
    code.mov(tmp, 1);
    /*code.lock();*/ code.xchg(code.dword[ptr], tmp);
    code.test(tmp, tmp);
    code.jnz(loop, code.T_NEAR);
}

void EmitSpinLockUnlock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp) {
    code.xor_(tmp, tmp);
    code.xchg(code.dword[ptr], tmp);
    code.mfence();
}

// Lock and Unlock are called from C++ (the exclusive monitor, on every
// recompiled LDXR/STXR), so they use plain atomics: a direct exchange instead
// of a call into generated code, a spin that only reads the word until it looks
// free (so waiting cores do not bounce its cache line), and a release store to
// unlock - on x86 an ordinary store, where the generated unlock paid for an
// XCHG and an MFENCE. The word protocol (0 free, 1 held) is unchanged, so code
// that inlines EmitSpinLockLock/Unlock still interoperates with these.
void SpinLock::Lock() noexcept {
    int* const word = const_cast<int*>(&storage);
    while (__atomic_exchange_n(word, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(word, __ATOMIC_RELAXED) != 0) {
            __builtin_ia32_pause();
        }
    }
}

void SpinLock::Unlock() noexcept {
    __atomic_store_n(const_cast<int*>(&storage), 0, __ATOMIC_RELEASE);
}

}  // namespace Dynarmic
