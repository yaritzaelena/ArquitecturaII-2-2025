#include <cassert>
#include <cstdio>
#include <vector>
#include <array>
#include <cstring>

#include "../src/memory/cache/mesi/MesiInterconnect.hpp"
#include "../src/memory/cache/mesi/MESICache.hpp"

int main() {
  // --- Bus + DRAM ---
  MesiInterconnect bus(/*dram_bytes=*/ 1<<20); // 1 MiB
  auto& dram = bus.dram();

  // --- Dos L1$ conectadas al bus (SIN adapter) ---
  MESICache c0(0, bus);
  MESICache c1(1, bus);
  bus.connect(&c0);
  bus.connect(&c1);

  // --- Inicializa una línea en DRAM ---
  const uint64_t addr = 0x200;
  const uint64_t line_base = addr & ~((uint64_t)MESICache::kLineSize - 1);
  for (int i = 0; i < (int)MESICache::kLineSize; ++i)
    dram[line_base + i] = (uint8_t)0x11;

  // --- 1) c0 STORE -> M (primer intento miss->BusRdX, segundo intento hit) ---
  uint64_t v = 0xDEADBEEFCAFEBABEULL;
  bool ok = c0.store(addr, &v);   // miss: emite BusRdX
  assert(!ok);
  ok = c0.store(addr, &v);        // hit: estado M
  assert(ok);

  // --- 2) c1 LOAD del mismo addr -> c0 Flush + downgrade M->S; c1 instala S ---
  uint64_t out = 0;
  ok = c1.load(addr, &out);       // miss: emite BusRd, c0 hace Flush en snoop
  assert(!ok);
  ok = c1.load(addr, &out);       // hit tras Data
  assert(ok);

  // --- Verifica que DRAM tenga el valor escrito por c0 (write-back del Flush) ---
  uint64_t memval = 0;
  const uint32_t off = addr & (MESICache::kLineSize - 1); // offset dentro de la línea
  std::memcpy(&memval, dram.data() + line_base + off, 8);
  assert(memval == v);

  std::puts("OK M->S downgrade with Flush");
  return 0;
}
