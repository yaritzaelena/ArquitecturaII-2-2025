#include "SharedMemory.h"
#include "src/memory/cache/mesi/MESICache.hpp"
#include "PE/pe/pe.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <chrono>
#include <fstream>  // Para CSV

// -----------------------------------------------------------------------------
// Bus mínimo compatible con tu estructura actual
// -----------------------------------------------------------------------------
class SimpleBus {
public:
    std::vector<MESICache*> caches;

    void connectCache(MESICache* c) {
        caches.push_back(c);
    }

    void broadcast(const BusTransaction& t) {
        const char* action = "";
        switch(t.type) {
            case BusMsg::BusRd: action = "BusRd"; break;
            case BusMsg::BusRdX: action = "BusRdX"; break;
            case BusMsg::BusUpgr: action = "BusUpgr"; break;
            case BusMsg::Flush: action = "Flush"; break;
            case BusMsg::Inv: action = "Inv"; break;
        }
        std::cout << "[BUS] Accion: " << action << " @0x"
                  << std::hex << t.addr << std::dec << "\n";

        for (auto c : caches) c->onSnoop(t);
    }
};

// -----------------------------------------------------------------------------
// Stepping: imprime estado y pausa
// -----------------------------------------------------------------------------
bool stepping_enabled = true;

void step_prompt(const std::string& desc) {
    if (!stepping_enabled) return;
    std::cout << "\n========== " << desc << " ==========\n";
    std::cout << "Presione ENTER para continuar...\n";
    std::cin.get();
}

// -----------------------------------------------------------------------------
// Callback para recibir respuestas de SharedMemory
// -----------------------------------------------------------------------------
auto print_response = [](MessageP resp){
    if (!resp) return;
    switch(resp->type) {
        case MessageType::READ_RESP:
            std::cout << "[MEM RESP] READ @0x" << std::hex 
                      << resp->payload.read_resp.address 
                      << " size=" << std::dec << resp->payload.read_resp.size 
                      << " status=" << static_cast<int>(resp->payload.read_resp.status) << "\n";
            break;
        case MessageType::WRITE_RESP:
            std::cout << "[MEM RESP] WRITE @0x" << std::hex 
                      << resp->payload.write_resp.address 
                      << " status=" << static_cast<int>(resp->payload.write_resp.status) << "\n";
            break;
        default: break;
    }
};

// -----------------------------------------------------------------------------
// Funciones auxiliares para leer/escribir memoria y notificar bus/caches
// -----------------------------------------------------------------------------
void write_mem(SharedMemory &mem, SimpleBus& bus, MESICache& cache, uint64_t addr, const std::vector<uint8_t>& data) {
    auto write_msg = std::make_shared<Message>(MessageType::WRITE_MEM, 0, -1);
    write_msg->payload.write_mem.address = addr;
    write_msg->payload.write_mem.size = static_cast<uint32_t>(data.size());
    write_msg->data_write = data;
    mem.handle_message(write_msg, print_response);

    if (!cache.store(addr, data.data())) {
        BusTransaction t{BusMsg::BusRdX, addr, nullptr, 0, 0};
        bus.broadcast(t);
        cache.onDataResponse(addr, data.data(), false);
    }
}

void read_mem(SharedMemory &mem, SimpleBus& bus, MESICache& cache, uint64_t addr, uint32_t size) {
    auto read_msg = std::make_shared<Message>(MessageType::READ_MEM, 0, -1);
    read_msg->payload.read_mem.address = addr;
    read_msg->payload.read_mem.size = size;
    mem.handle_message(read_msg, print_response);

    uint8_t buffer[32] = {0};
    if (!cache.load(addr, buffer)) {
        BusTransaction t{BusMsg::BusRd, addr, nullptr, 0, 0};
        bus.broadcast(t);
        cache.onDataResponse(addr, buffer, false);
    }
}

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main() {
    std::cout << "=== PRUEBA INTEGRADA PE + CACHE + MEMORIA (Automática) ===\n";

    SharedMemory shared_mem;
    SimpleBus bus;

    std::vector<std::unique_ptr<MESICache>> caches;
    std::vector<std::unique_ptr<PE>> pes;

    for (int i = 0; i < 2; ++i) {
        MesiBusIface iface;
        iface.emit = [&](const BusTransaction& t){ bus.broadcast(t); };

        caches.push_back(std::make_unique<MESICache>(i, iface));
        bus.connectCache(caches.back().get());

        pes.push_back(std::make_unique<PE>(i, nullptr));
    }

    // -------------------------------------------------------------------------
    // CICLOS AUTOMÁTICOS
    // -------------------------------------------------------------------------
    // C1: PE0 escribe
    std::cout << "\n[CICLO 1] PE0 escribe en direccion 0x00\n";
    write_mem(shared_mem, bus, *caches[0], 0x00, {42,0,0,0,0,0,0,0});
    for (auto& c : caches) c->dumpCacheState(std::cout);
    step_prompt("Fin de ciclo 1");

    // C2: PE1 lee
    std::cout << "\n[CICLO 2] PE1 lee direccion 0x00\n";
    read_mem(shared_mem, bus, *caches[1], 0x00, 8);
    for (auto& c : caches) c->dumpCacheState(std::cout);
    step_prompt("Fin de ciclo 2");

    // C3: PE0 sobrescribe
    std::cout << "\n[CICLO 3] PE0 sobrescribe el valor\n";
    write_mem(shared_mem, bus, *caches[0], 0x00, {99,0,0,0,0,0,0,0});
    for (auto& c : caches) c->dumpCacheState(std::cout);
    step_prompt("Fin de ciclo 3");

    // C4: PE1 lee nuevamente
    std::cout << "\n[CICLO 4] PE1 vuelve a leer\n";
    read_mem(shared_mem, bus, *caches[1], 0x00, 8);
    for (auto& c : caches) c->dumpCacheState(std::cout);
    step_prompt("Fin de ciclo 4");

    // -------------------------------------------------------------------------
    // Guardar estadísticas finales en CSV
    // -------------------------------------------------------------------------
    std::ofstream csv_file("cache_stats.csv");
    csv_file << "PE,Loads,Stores,RW_Accesses,Cache_Misses,Invalidations,BusRd,BusRdX,BusUpgr,Flush,Transitions\n";
    for (int i = 0; i < caches.size(); ++i) {
        auto& m = caches[i]->stats();
        std::string transitions;
        for (const auto& t : m.mesi_transitions) {
            if (!transitions.empty()) transitions += "; ";
            transitions += t;
        }
        csv_file << i << ","
                 << m.loads << ","
                 << m.stores << ","
                 << m.rw_accesses << ","
                 << m.cache_misses << ","
                 << m.invalidations << ","
                 << m.busRd << ","
                 << m.busRdX << ","
                 << m.busUpgr << ","
                 << m.flush << ","
                 << "\"" << transitions << "\"\n";
    }
    csv_file.close();

    shared_mem.dump_stats();
    return 0;
}
