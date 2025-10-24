#pragma once
#include "MesiTypes.hpp"
#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>
#include <sstream>

class MesiInterconnect; 

/*
 * MESICache (header)
 * ==================
 * Controlador de caché L1 privado por PE con coherencia MESI (M/E/S/I).
 *
 * Parámetros (fijos por especificación del proyecto):
 * - 2-way set associative
 * - 16 líneas totales  => 8 sets x 2 vías
 * - Tamaño de línea: 32 bytes (offset=5 bits)
 *
 * Política:
 * - write-allocate + write-back
 *
 * Interacción con el bus:
 * - Emite BusRd/BusRdX/BusUpgr/Flush según necesidad
 * - Recibe onDataResponse(...) para instalar líneas en E/S
 * - Responde a onSnoop(...) degradando/invalidando y flusheando si está en M
 *
 * API de acceso:
 * - load/store de 8B: devuelven true si hit y se completó; false si se emitió una
 *   operación al bus (el puerto debe reintentar cuando llegue Data).
 *
 * Métricas:
 * - loads, stores, rw_accesses, cache_misses, invalidations, busRd/RdX/Upgr/Flush,
 *   matriz de transiciones MESI y log legible de transiciones (para CSV/gráficas).
 */
class MESICache {
public:
    // Resultado de una búsqueda: ¿hubo hit?, ¿en qué vía?, puntero a la línea.
    struct Lookup { bool hit; int way; CacheLine* line; };

    // Parámetros fijos (especificación): 16 líneas totales, 2-way, línea de 32B
    static constexpr int kSets = 8;
    static constexpr int kWays = 2;
    static constexpr int kLineSize = 32;
    static constexpr int kOffsetBits = 5; // 32B -> 5 bits
    static constexpr int kIndexBits  = 3; // 8 sets -> 3 bits

    // Ctor: id del PE e interconect (bus) por referencia
    MESICache(int pe_id, MesiInterconnect& bus);

    // Carga/almacenamiento de 8 bytes (p.ej., double/uint64_t).
    // Devuelven true si fue hit y la operación se completó.
    // Si devuelven false => se emitió una petición de bus; reintentar luego del Data.
    bool load(uint64_t addr, void* out8);
    bool store(uint64_t addr, const void* in8);

    // Búsqueda por (set,tag). Útil para depuración o comprobaciones locales.
    Lookup lookupLine(uint64_t addr);

    // ¿Existe una copia válida/no-I de esa dirección en esta L1$?
    bool hasLine(uint64_t addr) const;

    // Llamado por el interconnect cuando llega Data/Flush para este solicitante:
    // instala línea en E (exclusive) si 'shared=false' o en S (shared) si 'shared=true'.
    void onDataResponse(uint64_t addr, const uint8_t lineData[32], bool shared);

    // Snoop entrante (el interconnect lo invoca en los demás caches) para eventos
    // BusRd/BusRdX/BusUpgr/Inv realizados por otros PEs.
    void onSnoop(const BusTransaction& t);

    // Dump amigable del estado de la caché (sets, ways, MESI, tag, dirty)
    void dumpCacheState(std::ostream& os) const;

    // Métricas y depuración (se imprimen/guardan en CSV o consola)
    struct CacheMetrics {
        int cache_misses = 0;     // misses totales (load+store)
        int invalidations = 0;    // veces que esta L1$ invalida por snoop/upgrade ajeno
        int loads = 0;            // lecturas locales (intentos)
        int stores = 0;           // escrituras locales (intentos)
        int rw_accesses = 0;      // loads + stores
        int busRd = 0;            // emisiones de BusRd (lectura al bus)
        int busRdX = 0;           // emisiones de BusRdX (lectura con exclusividad)
        int busUpgr = 0;          // emisiones de BusUpgr (upgrade S->M)
        int flush = 0;            // emisiones de Flush (write-back de línea M)
        int mesi_trans[4][4] = {{0}};          // matriz de transición MESI (conteo from->to)
        std::vector<std::string> mesi_transitions; // historial legible ("MESI: 1->3")
    };

    // Lectura inmutable de métricas (para informes/CSV)
    const CacheMetrics& stats() const { return metrics_; }

    // Impresión directa de estadísticas (útil para depurar rápidamente)
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

    // Exporta el log de transiciones como una sola línea (para CSV)
    std::string transition_log() const {
        std::ostringstream oss;
        for (size_t i = 0; i < metrics_.mesi_transitions.size(); ++i) {
            oss << metrics_.mesi_transitions[i];
            if (i + 1 < metrics_.mesi_transitions.size()) oss << "; ";
        }
        return oss.str();
    }

private:
    // Identificador del PE (quién es el dueño de esta L1$)
    int pe_id_ = -1;

    // Puntero al interconect (para emitir y recibir mensajes coherentes)
    MesiInterconnect* bus_ = nullptr;

    // Arreglo de sets (cada set tiene 2 vías y un bit/índice LRU)
    Set sets_[kSets]{};

    // Contador de métricas
    CacheMetrics metrics_;

    // Helpers de direccionamiento para separar offset/index/tag
    static uint32_t idx(uint64_t addr) { return (addr >> kOffsetBits) & ((1u<<kIndexBits)-1); }
    static uint64_t tag(uint64_t addr) { return addr >> (kOffsetBits + kIndexBits); }
    static uint32_t off(uint64_t addr) { return addr & (kLineSize-1); }

    // Marca MRU en el set 's' para la vía 'way_mru' (2-way => basta con un bit/índice)
    void touchLRU(uint32_t s, int way_mru);

    // Devuelve la vía víctima (LRU) del set 's'
    int  victimWay(uint32_t s) const;

    // Registra transición de estado en matriz/log
    void recordTrans(MESI from, MESI to);

    // Instalar o reemplazar línea (si víctima en M => Flush antes de sobrescribir)
    void installLine(uint64_t addr, const uint8_t data[32], MESI st);

    // helpers R/W de 8 bytes dentro de la línea (útil para double/uint64)
    void write8(CacheLine& line, uint32_t line_off, const void* in8);
    void read8(const CacheLine& line, uint32_t line_off, void* out8) const;

    // Emisiones de bus (atajos encapsulados)
    void emitBusRd(uint64_t addr);
    void emitBusRdX(uint64_t addr);
    void emitBusUpgr(uint64_t addr);
    void emitFlush(uint64_t addr, const uint8_t data[32]);
    void emitInv(uint64_t addr); // opcional (si el bus lo requiere)
};


