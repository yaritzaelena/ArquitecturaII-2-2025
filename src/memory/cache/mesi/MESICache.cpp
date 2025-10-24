#include "MESICache.hpp"
#include "MesiDebug.hpp"
#include <cstring>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "MesiInterconnect.hpp"

/*
 * MESICache
 * =========
 * Controlador de cache L1 privado por PE con coherencia MESI (M/E/S/I),
 * 2 vías por set, línea de 32B, políticas write-allocate + write-back.
 * - Emite transacciones al bus: BusRd, BusRdX, BusUpgr, Flush (y opcional Inv).
 * - Responde snoops de otros PEs en onSnoop(...).
 * - onDataResponse(...) instala la línea en E o S según el bit "shared".
 * - load/store devuelven false en miss o falta de exclusividad para que
 *   el puerto (IMemoryPort) reintente cuando llegue la respuesta de datos.
 * - Métricas: loads/stores, rw_accesses, cache_misses, invalidations, busRd/Upgr/RdX/Flush
 *   y matriz/lista de transiciones MESI para análisis.
 */

MESICache::MESICache(int pe_id, MesiInterconnect& bus)
  : pe_id_(pe_id), bus_(&bus) {}

/* hasLine(addr)
 * -------------
 * Devuelve true si existe en el set correspondiente una línea válida con el tag
 * buscado y con estado distinto de I (Invalid).
 */
bool MESICache::hasLine(uint64_t addr) const {
    const uint32_t s = idx(addr);
    const uint64_t t = tag(addr);
    for (int w = 0; w < kWays; ++w) {
        const auto& L = sets_[s].way[w];
        if (L.valid && L.tag == t && L.state != MESI::I)
            return true;
    }
    return false;
}

/* lookupLine(addr)
 * ----------------
 * Busca una línea por (set, tag). Si la encuentra y no está en I, retorna hit=true,
 * la vía y el puntero a la línea. Si no, retorna hit=false.
 */
auto MESICache::lookupLine(uint64_t addr) -> Lookup {
    uint32_t s = idx(addr);
    uint64_t t = tag(addr);
    for (int w = 0; w < kWays; ++w) {
        auto& L = sets_[s].way[w];
        if (L.valid && L.tag == t && L.state != MESI::I)
            return {true, w, &L};
    }
    return {false, -1, nullptr};
}

/* touchLRU(s, way_mru)
 * --------------------
 * Marca la vía usada como MRU. Para 2 vías, un bit/índice por set es suficiente.
 */
void MESICache::touchLRU(uint32_t s, int way_mru) {
    sets_[s].lru = (way_mru == 0) ? 1 : 0;
}

/* victimWay(s)
 * ------------
 * Devuelve la vía víctima (la menos recientemente usada) para un set.
 */
int MESICache::victimWay(uint32_t s) const {
    return sets_[s].lru == 0 ? 0 : 1;
}

/* recordTrans(from, to)
 * ---------------------
 * Registra una transición de estado MESI en una matriz de conteo
 * y también en una lista legible tipo "MESI: 1->3" (para CSV/gráficas).
 */
void MESICache::recordTrans(MESI from, MESI to) {
    metrics_.mesi_trans[(int)from][(int)to]++;
    std::stringstream ss;
    ss << "MESI: " << (int)from << "->" << (int)to;
    metrics_.mesi_transitions.push_back(ss.str());
}

/* Emisores de mensajes al bus
 * ---------------------------
 * Actualizan métricas y llaman bus_->emit(...) con el tipo de mensaje.
 */
void MESICache::emitBusRd(uint64_t addr) {
    metrics_.busRd++;
    assert(bus_);
    bus_->emit({BusMsg::BusRd, addr, nullptr, 0, pe_id_});
}

void MESICache::emitBusRdX(uint64_t addr) {
    metrics_.busRdX++;
    assert(bus_);
    bus_->emit({BusMsg::BusRdX, addr, nullptr, 0, pe_id_});
}

void MESICache::emitBusUpgr(uint64_t addr) {
    metrics_.busUpgr++;
    assert(bus_);
    bus_->emit({BusMsg::BusUpgr, addr, nullptr, 0, pe_id_});
}

void MESICache::emitFlush(uint64_t addr, const uint8_t* data) {
    metrics_.flush++;
    assert(bus_);
    // En Flush enviamos línea completa (32B)
    bus_->emit({BusMsg::Flush, addr, data, kLineSize, pe_id_});
}

void MESICache::emitInv(uint64_t addr) {
    assert(bus_);
    bus_->emit({BusMsg::Inv, addr, nullptr, 0, pe_id_});
}

/* installLine(addr, data, st)
 * ---------------------------
 * Instala una línea en el set de 'addr' con estado 'st' (E/S/M).
 * - Si no hay vía libre/Invalid, se elige víctima LRU.
 * - Si la víctima está en M, se hace Flush (write-back) antes de sobrescribir.
 * - Copia datos, marca estado/dirty y actualiza LRU.
 *
 */
void MESICache::installLine(uint64_t addr, const uint8_t data[32], MESI st) {
    uint32_t s = idx(addr);
    uint64_t t = tag(addr);
    int way = -1;

    // 1) buscar hueco (vía no válida o en I)
    for (int w = 0; w < kWays; ++w) {
        if (!sets_[s].way[w].valid || sets_[s].way[w].state == MESI::I) {
            way = w;
            break;
        }
    }

    // 2) si no hay hueco, tomar víctima LRU
    if (way == -1) {
        way = victimWay(s);
        auto& V = sets_[s].way[way];
        if (V.valid && V.state == MESI::M) {
            // 🔄 Write-back de la víctima sucia antes de sobrescribir
            // 💡 TIP: la dirección debería ser la base de la línea VÍCTIMA.
            // Actualmente se usa la base derivada de 'addr' (nueva línea):
            emitFlush(addr & ~((uint64_t)kLineSize - 1), V.data.data());
        }
    }

    // 3) instalar nueva línea y estado
    auto& L = sets_[s].way[way];
    L.valid = true;
    L.dirty = (st == MESI::M);
    // Registrar transición desde el estado previo (por defecto suele ser I)
    recordTrans(L.state, st);
    L.state = st;
    L.tag   = t;
    std::memcpy(L.data.data(), data, kLineSize);
    touchLRU(s, way);
}

/* write8/read8
 * ------------
 * Accesos de 8 bytes (útil para double/uint64). write8 marca dirty.
 */
void MESICache::write8(CacheLine& line, uint32_t line_off, const void* in8) {
    line.dirty = true;
    std::memcpy(line.data.data() + line_off, in8, 8);
}

void MESICache::read8(const CacheLine& line, uint32_t line_off, void* out8) const {
    std::memcpy(out8, line.data.data() + line_off, 8);
}

/* load(addr, out8)
 * ----------------
 * Camino de lectura local:
 * - Si hit: lee, toca LRU y retorna true.
 * - Si miss: cuenta miss, emite BusRd y retorna false (el puerto reintenta).
 */
bool MESICache::load(uint64_t addr, void* out8) {
    metrics_.loads++; metrics_.rw_accesses++;
    auto L = lookupLine(addr);
    uint32_t s = idx(addr);
    uint32_t o = off(addr);

    if (L.hit) {
        read8(*L.line, o, out8);
        touchLRU(s, L.way);
        return true;
    }

    metrics_.cache_misses++;
    emitBusRd(addr);
    return false;
}

/* store(addr, in8)
 * ----------------
 * Camino de escritura local (write-allocate + write-back):
 * - Si no hay línea o está en I: miss, BusRdX y retorna false (el puerto reintenta).
 * - Si hay línea:
 *     M: escribe directo (M→M).
 *     E: eleva a M (E→M), escribe.
 *     S: emite BusUpgr, eleva a M (S→M), escribe.
 */
bool MESICache::store(uint64_t addr, const void* in8) {
    metrics_.stores++; metrics_.rw_accesses++;
    auto L = lookupLine(addr);
    uint32_t s = idx(addr);
    uint32_t o = off(addr);

    // Miss o línea inválida: pedir exclusividad vía BusRdX y reintentar luego
    if (!L.hit || L.line->state == MESI::I) {
        metrics_.cache_misses++;
        emitBusRdX(addr);
        return false;
    }

    // Hit: actuar según estado MESI
    switch (L.line->state) {
        case MESI::M:
            // Ya exclusiva y modificable
            write8(*L.line, o, in8);
            touchLRU(s, L.way);
            return true;
        case MESI::E:
            // Elevar E->M y escribir
            recordTrans(MESI::E, MESI::M);
            L.line->state = MESI::M;
            L.line->dirty = true;
            write8(*L.line, o, in8);
            touchLRU(s, L.way);
            return true;
        case MESI::S:
            // Pedir upgrade para invalidar copias ajenas, S->M y escribir
            emitBusUpgr(addr);
            recordTrans(MESI::S, MESI::M);
            L.line->state = MESI::M;
            L.line->dirty = true;
            write8(*L.line, o, in8);
            touchLRU(s, L.way);
            return true;
        case MESI::I:
            // No debería suceder aquí (ya lo cubrimos arriba)
            break;
    }
    return false;
}

/* onDataResponse(addr, lineData, shared)
 * --------------------------------------
 * El bus entrega datos tras BusRd/BusRdX. Si shared=true, instalamos en S;
 * si shared=false, en E. (La escritura local posterior podrá llevar E->M).
 */
void MESICache::onDataResponse(uint64_t addr, const uint8_t lineData[32], bool shared) {
    installLine(addr, lineData, shared ? MESI::S : MESI::E);
}

/* onSnoop(t)
 * ----------
 * Reacción a tráfico de otros PEs sobre nuestra copia:
 * - BusRd   : si estoy en M => Flush y M->S; si en E => E->S.
 * - BusRdX/Inv/BusUpgr: si estoy en M => Flush; si S/E/M => invalidar -> I.
 * Se cuentan invalidaciones y transiciones.
 */
void MESICache::onSnoop(const BusTransaction& t) {
    uint32_t s = idx(t.addr);
    uint64_t ttag = tag(t.addr);
    for (int w = 0; w < kWays; ++w) {
        auto& L = sets_[s].way[w];
        if (!(L.valid && L.tag == ttag)) continue;

        switch (t.type) {
            case BusMsg::BusRd:
                // Otro PE lee: si soy dueño sucio (M), debo proveer datos y degradar a S
                if (L.state == MESI::M) {
                    emitFlush(t.addr, L.data.data());
                    recordTrans(MESI::M, MESI::S);
                    L.state = MESI::S; L.dirty = false;
                } else if (L.state == MESI::E) {
                    // Exclusivo limpio -> compartido
                    recordTrans(MESI::E, MESI::S);
                    L.state = MESI::S;
                }
                break;
            case BusMsg::BusRdX:
            case BusMsg::Inv:
            case BusMsg::BusUpgr:
                // Otro PE quiere exclusividad: si estoy en M, write-back; luego invalidar
                if (L.state == MESI::M) emitFlush(t.addr, L.data.data());
                if (L.state != MESI::I) {
                    metrics_.invalidations++;
                    recordTrans(L.state, MESI::I);
                    L.state = MESI::I;
                    L.dirty = false;
                }
                break;
            default:
                break;
        }
    }
}

/* dumpCacheState(os)
 * ------------------
 * Utilidad de depuración: imprime set/vía con estado MESI, tag y dirty.
 */
void MESICache::dumpCacheState(std::ostream& os) const {
    os << "=== Estado Cache PE" << pe_id_ << " ===\n";
    for (size_t s = 0; s < kSets; ++s) {
        os << "Set " << s << ":\n";
        for (int w = 0; w < kWays; ++w) {
            const auto& L = sets_[s].way[w];
            os << "  Way " << w << ": ";
            if (!L.valid) { os << "Invalid\n"; continue; }

            const char* st_str = "";
            switch (L.state) {
                case MESI::M: st_str = "M"; break;
                case MESI::E: st_str = "E"; break;
                case MESI::S: st_str = "S"; break;
                case MESI::I: st_str = "I"; break;
            }
            os << st_str
               << " Tag:0x" << std::hex << L.tag
               << " Dirty:" << std::dec << L.dirty << "\n";
        }
    }
}

