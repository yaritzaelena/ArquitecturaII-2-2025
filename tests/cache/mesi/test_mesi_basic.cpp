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

  // --- Caches conectadas al bus (SIN adapter) ---
  MESICache c0(0, bus);
  MESICache c1(1, bus);
  bus.connect(&c0);
  bus.connect(&c1);

  // --- Inicializa una línea en DRAM ---
  uint64_t addr = 0x100; // alguna dirección
  uint64_t base = addr & ~((uint64_t)MESICache::kLineSize - 1);
  auto& mem = bus.dram();
  for (int i = 0; i < (int)MESICache::kLineSize; ++i)
    mem[base + i] = static_cast<uint8_t>(i);

  // 1) LOAD en c0 => miss -> BusRd -> Data -> reintenta
  uint64_t out=0;
  bool ok = c0.load(addr, &out);
  assert(!ok);      // primera llamada debe hacer miss y emitir BusRd
  ok = c0.load(addr, &out);
  assert(ok);       // ahora hit

  // 2) LOAD en c1 del mismo addr => S en ambos
  ok = c1.load(addr, &out);
  assert(!ok);      // miss inicial
  ok = c1.load(addr, &out);
  assert(ok);       // hit después del Data

  // 3) STORE en c0 => si estaba S, emite BusUpgr y pasa a M; c1 invalida por snoop
  uint64_t val = 0xAABBCCDDEEFF0011ULL;
  ok = c0.store(addr, &val);
  assert(ok);

  // 4) Fuerza una evicción en el MISMO set para provocar Flush si la víctima estaba M
  //    OJO: que sea el mismo índice depende de tu mapeo (kIndexBits). Con line=32,
  //    0x100 (256) salta típicamente al mismo set si kIndexBits >= 3. Ajusta si hace falta.
  uint64_t v = 0x1122334455667788ULL;
  uint64_t addr1 = base + 0x100; // otro tag, idealmente mismo índice
  uint64_t addr2 = base + 0x200; // otro tag, mismo índice

  ok = c0.store(addr1, &v);  // miss -> BusRdX -> write -> M
  assert(!ok);
  ok = c0.store(addr1, &v);  // hit -> M
  assert(ok);

  ok = c0.store(addr2, &v);  // miss -> posible evicción en 2-way
  assert(!ok);
  ok = c0.store(addr2, &v);  // hit
  assert(ok);

  // --- Métricas ---
  const auto& s0 = c0.stats();
  const auto& s1 = c1.stats();
  std::printf(
    "PE0: loads=%llu stores=%llu misses=%llu inv=%llu rd=%llu rdx=%llu upg=%llu flush=%llu\n",
    (unsigned long long)s0.loads, (unsigned long long)s0.stores, (unsigned long long)s0.cache_misses,
    (unsigned long long)s0.invalidations, (unsigned long long)s0.busRd, (unsigned long long)s0.busRdX,
    (unsigned long long)s0.busUpgr, (unsigned long long)s0.flush
  );
  std::printf(
    "PE1: loads=%llu stores=%llu misses=%llu inv=%llu rd=%llu rdx=%llu upg=%llu flush=%llu\n",
    (unsigned long long)s1.loads, (unsigned long long)s1.stores, (unsigned long long)s1.cache_misses,
    (unsigned long long)s1.invalidations, (unsigned long long)s1.busRd, (unsigned long long)s1.busRdX,
    (unsigned long long)s1.busUpgr, (unsigned long long)s1.flush
  );

  // Debe haber al menos un Flush por la evicción de una línea en M
  std::printf("PE0 (post-eviction): flush=%llu (esperado >= 1)\n",
    (unsigned long long)s0.flush);
  assert(s0.flush >= 1);

  std::puts("OK basic MESI test");
  return 0;
}
