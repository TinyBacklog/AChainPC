// SPDX-License-Identifier: GPL-2.0-or-later
// Dynarmic::SpinLock guards the exclusive monitor that every recompiled
// LDXR/STXR goes through. Several threads bump a plain counter under the lock;
// any lost update means two of them were inside at once.
#include <iostream>
#include <thread>
#include <vector>
#include <dynarmic/common/spin_lock.h>

int main() {
    constexpr int kThreads = 4, kIterations = 200000;
    Dynarmic::SpinLock lock;
    long counter = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                lock.Lock();
                // Read, yield the core, write back: a broken lock loses updates.
                const long seen = counter;
                if ((i & 1023) == 0) std::this_thread::yield();
                counter = seen + 1;
                lock.Unlock();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (counter != long{kThreads} * kIterations || lock.storage != 0) {
        std::cout << "FAIL counter=" << counter << " storage=" << lock.storage << '\n';
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
