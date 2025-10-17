#pragma once
#include <vector>
#include <array>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cassert>

#include "MESICache.hpp"         // BusTransaction, BusMsg, kLineSize

class MesiInterconnect {
public:
  explicit MesiInterconnect(size_t dram_bytes);
  void connect(MESICache* cache);
  // Crea el iface para una caché con id 'id'
  // - emit: cache -> bus
  // - registerSnoopSink: cache -> bus (registra callback)
 // MesiBusIface makeInterface(int id);
  void emit(const BusTransaction& t);    
  // acceso a DRAM simulada (inicialización de datos) --- TRABAJANDO EN INTEGRACION CON MEM
  std::vector<uint8_t>& dram()       { return dram_; }
  const std::vector<uint8_t>& dram() const { return dram_; }
  void attachCachePtr(int id, MESICache* c);

private:
  // por-id
  std::vector<std::function<void(const BusTransaction&)>> snoop_sinks_; // callbacks de snoop
  std::vector<MESICache*> caches_;   
  std::vector<uint8_t>    dram_;
  std::unordered_map<uint64_t, std::array<uint8_t, MESICache::kLineSize>> last_flush_;
  std::recursive_mutex mtx_; 
  static inline uint64_t base_(uint64_t a) {
    return a & ~((uint64_t)MESICache::kLineSize - 1);
  }

   // implementación real
  bool any_other_has_line_(int except_id, uint64_t addr) const;
  void snoop_others_(const BusTransaction& t);
};
