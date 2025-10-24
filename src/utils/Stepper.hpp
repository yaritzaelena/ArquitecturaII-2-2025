#pragma once
#include <iostream>
#include <vector>
#include <mutex>
#include "../../src/memory/SharedMemory.h"
#include "../../src/memory/cache/mesi/MESICache.hpp"

// ======================================================
// Stepper: herramienta de depuración interactiva del bus MESI
// ======================================================
struct Stepper {
    bool enabled = true;
    std::mutex mx;

    void pause(const char* tag,
               const std::vector<MESICache*>& caches,
               SharedMemory* shm)
    {
        if (!enabled) return;
        std::lock_guard<std::mutex> lk(mx);

        std::cout << "\n========== EVENTO MESI: " << tag << " ==========\n";
        for (auto* c : caches)
            if (c) c->dumpCacheState(std::cout);
        if (shm) shm->dump_stats(std::cout);

        std::cout << "Presione ENTER para continuar...\n";
        std::cin.get();
    }
};
