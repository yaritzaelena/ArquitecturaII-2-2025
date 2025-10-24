#pragma once
#include "MesiTypes.hpp"
#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>
#include <sstream>

class MesiInterconnect; 

class MESICache {
public:
    struct Lookup { bool hit; int way; CacheLine* line; };

    // Parámetros fijos (especificación): 16 líneas totales, 2-way, línea de 32B
    static constexpr int kSets = 8;
    static constexpr int kWays = 2;
    static constexpr int kLineSize = 32;
    static constexpr int kOffsetBits = 5; // 32B -> 5 bits
    static constexpr int kIndexBits  = 3; // 8 sets -> 3 bits

    MESICache(int pe_id, MesiInterconnect& bus);

    // Carga/almacenamiento de 8 bytes alineados dentro de la línea.
    // Devuelven true si fue hit y la operación se completó.
    // Si devuelven false => se emitió una petición de bus; reintentar luego del Data.
    bool load(uint64_t addr, void* out8);
    bool store(uint64_t addr, const void* in8);

    Lookup lookupLine(uint64_t addr);
    bool hasLine(uint64_t addr) const;

    // Llamado por el interconnect cuando llega Data/Flush para este solicitante
    void onDataResponse(uint64_t addr, const uint8_t lineData[32], bool shared);

    // Snoop entrante (el interconnect lo invoca en todos los demás caches)
    void onSnoop(const BusTransaction& t);

    void dumpCacheState(std::ostream& os) const;

    // Métricas y depuración
    struct CacheMetrics {
        int cache_misses = 0;
        int invalidations = 0;
        int loads = 0;
        int stores = 0;
        int rw_accesses = 0;
        int busRd = 0;
        int busRdX = 0;
        int busUpgr = 0;
        int flush = 0;
        int mesi_trans[4][4] = {{0}};
        std::vector<std::string> mesi_transitions; // historial legible
    };

    const CacheMetrics& stats() const { return metrics_; }

    void dumpStats(std::ostream& os) const {
        os << "\n=== Estadísticas Cache PE" << pe_id_ << " ===\n";
        os << "Cache misses: " << metrics_.cache_misses << "\n";
        os << "Invalidaciones: " << metrics_.invalidations << "\n";
        os << "Loads: " << metrics_.loads << "\n";
        os << "Stores: " << metrics_.stores << "\n";
        os << "RW Accesses: " << metrics_.rw_accesses << "\n";
        os << "BusRd: " << metrics_.busRd 
           << ", BusRdX: " << metrics_.busRdX
           << ", BusUpgr: " << metrics_.busUpgr 
           << ", Flush: " << metrics_.flush << "\n";
        os << "Transiciones MESI:\n";
        for (const auto& t : metrics_.mesi_transitions)
            os << "  " << t << "\n";
    }

    // 🔹 Nuevo: método para exportar las transiciones al CSV
    std::string transition_log() const {
        std::ostringstream oss;
        for (size_t i = 0; i < metrics_.mesi_transitions.size(); ++i) {
            oss << metrics_.mesi_transitions[i];
            if (i + 1 < metrics_.mesi_transitions.size()) oss << "; ";
        }
        return oss.str();
    }

private:
    int pe_id_ = -1;
    MesiInterconnect* bus_ = nullptr;
    Set sets_[kSets]{};
    CacheMetrics metrics_;

    static uint32_t idx(uint64_t addr) { return (addr >> kOffsetBits) & ((1u<<kIndexBits)-1); }
    static uint64_t tag(uint64_t addr) { return addr >> (kOffsetBits + kIndexBits); }
    static uint32_t off(uint64_t addr) { return addr & (kLineSize-1); }

    void touchLRU(uint32_t s, int way_mru);
    int  victimWay(uint32_t s) const;
    void recordTrans(MESI from, MESI to);

    // Instalar o reemplazar línea (evict con Flush si M)
    void installLine(uint64_t addr, const uint8_t data[32], MESI st);

    // helpers R/W de 8 bytes
    void write8(CacheLine& line, uint32_t line_off, const void* in8);
    void read8(const CacheLine& line, uint32_t line_off, void* out8) const;

    // Emisiones de bus (atajos)
    void emitBusRd(uint64_t addr);
    void emitBusRdX(uint64_t addr);
    void emitBusUpgr(uint64_t addr);
    void emitFlush(uint64_t addr, const uint8_t data[32]);
    void emitInv(uint64_t addr); // opcional (puede usarlo el bus)
};
