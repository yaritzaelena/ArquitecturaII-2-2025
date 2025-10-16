#pragma once
#include <cstdint>
#include <array>
#include <functional>

// Estados MESI: Modified, Exclusive, Shared, Invalid
enum class MESI : uint8_t { I=0, S=1, E=2, M=3 };

// Mensajería de bus mínima para coherencia
enum class BusMsg : uint8_t {
  BusRd,     // lectura compartida
  BusRdX,    // lectura con propiedad (para escribir)
  BusUpgr,   // upgrade S->M
  Data,      // datos de respuesta (32 B)
  Flush,     // write-back de una línea sucia
  Inv        // invalidación a terceros
};

struct BusTransaction {
  BusMsg    type;
  uint64_t  addr;
  const uint8_t* payload; // 32B si Data/Flush, null si no aplica
  uint32_t  size;         // normalmente 32
  int       src_pe;       // PE emisor
};

// Métricas por PE (requeridas en la especificación)
struct CacheMetrics {
  uint64_t loads=0, stores=0, misses=0, invalidations=0;
  uint64_t busRd=0, busRdX=0, busUpgr=0, flush=0;
  uint64_t mesi_trans[4][4] = {{0}};
  uint64_t rw_accesses=0; // registro total de accesos (R/W)
};

// Línea y set de la caché (2 vías)
struct CacheLine {
  bool     valid=false;
  bool     dirty=false;
  MESI     state=MESI::I;
  uint64_t tag=0;
  std::array<uint8_t,32> data{};
};

struct Set {
  CacheLine way[2];
  uint8_t   lru=0; // 0 -> way0 es LRU; 1 -> way1 es LRU
};
