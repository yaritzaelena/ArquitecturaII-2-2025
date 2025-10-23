#pragma once
#include <vector>
#include <array>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <functional>
#include "../src/memory/SharedMemory.h" 
#include "../src/memory/cache/mesi/MESICache.hpp"         // BusTransaction, BusMsg, kLineSize

class MesiInterconnect {
public:
  explicit MesiInterconnect(size_t /*dram_bytes*/); 
  void set_shared_memory(SharedMemory* shm) { shm_ = shm; }
  void connect(MESICache* cache);

  void emit(const BusTransaction& t);    
  void attachCachePtr(int id, MESICache* c);

private:
  // por-id
  std::vector<std::function<void(const BusTransaction&)>> snoop_sinks_; // callbacks de snoop
  std::vector<MESICache*> caches_;   
  std::unordered_map<uint64_t, std::array<uint8_t, MESICache::kLineSize>> last_flush_;
  std::recursive_mutex mtx_; 

  //dirección base de una línea de caché.
  SharedMemory* shm_ = nullptr;
  static inline uint64_t base_(uint64_t a) {
    return a & ~((uint64_t)MESICache::kLineSize - 1);
  }

  void read_line_from_mem_(uint64_t base_addr, uint8_t out[32]);
  void write_line_to_mem_(uint64_t base_addr, const uint8_t in[32]);

   // implementación real
  bool any_other_has_line_(int except_id, uint64_t addr) const;
  void snoop_others_(const BusTransaction& t);
};