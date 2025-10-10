#pragma once
#include "MesiTypes.hpp"
#include "adapters/InterconnectAdapter.hpp"
#include <cstdint>
#include <optional>

class MESICache {
public:
  // Parámetros fijos (especificación): 16 líneas totales, 2-way, línea de 32B
  static constexpr int kSets = 8;
  static constexpr int kWays = 2;
  static constexpr int kLineSize = 32;
  static constexpr int kOffsetBits = 5; // 32B -> 5 bits
  static constexpr int kIndexBits  = 3; // 8 sets -> 3 bits

  explicit MESICache(int pe_id, const MesiBusIface& bus);

  // Carga/almacenamiento de 8 bytes alineados dentro de la línea.
  // Devuelven true si fue hit y la operación se completó.
  // Si devuelven false => se emitió una petición de bus; reintentar luego del Data.
  bool load(uint64_t addr, void* out8);
  bool store(uint64_t addr, const void* in8);

  // Llamado por el interconnect cuando llega Data/Flush para este solicitante
  void onDataResponse(uint64_t addr, const uint8_t lineData[32], bool shared);

  // Snoop entrante (el interconnect lo invoca en todos los demás caches)
  void onSnoop(const BusTransaction& t);

  // Métricas y depuración
  const CacheMetrics& stats() const { return metrics_; }

private:
  int pe_id_;
  MesiBusIface bus_;
  Set sets_[kSets]{};
  CacheMetrics metrics_{};

  struct Lookup { bool hit; int way; CacheLine* line; };
  static uint32_t idx(uint64_t addr) { return (addr >> kOffsetBits) & ((1u<<kIndexBits)-1); }
  static uint64_t tag(uint64_t addr) { return addr >> (kOffsetBits + kIndexBits); }
  static uint32_t off(uint64_t addr) { return addr & (kLineSize-1); }

  Lookup lookupLine(uint64_t addr);
  void   touchLRU(uint32_t s, int way_mru);
  int    victimWay(uint32_t s) const;
  void   recordTrans(MESI from, MESI to);

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
