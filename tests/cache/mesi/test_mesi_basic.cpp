#include <cassert>
#include <cstdio>
#include <vector>
#include <array>
#include <cstring>
#include "../../../src/memory/cache/mesi/MESICache.hpp"

struct MiniBus {
  std::vector<MESICache*> caches;
  std::array<uint8_t, 4096> mem{}; // memoria simple

  void connect(MESICache* c) { caches.push_back(c); }

  void emit(const BusTransaction& t) {
    // Snoops a todos menos al emisor
    bool shared=false;
    std::array<uint8_t,32> wb{};
    for (auto* c: caches) {
      if (!c) continue;
      if (c == caches[t.src_pe]) continue;
      c->onSnoop(t);
    }
    if (t.type==BusMsg::Flush && t.payload) {
      // Escribe en memoria (write-back)
      uint64_t base = t.addr & ~((uint64_t)MESICache::kLineSize-1);
      std::memcpy(mem.data()+base, t.payload, 32);
    }
    if (t.type==BusMsg::BusRd || t.type==BusMsg::BusRdX) {
      // Determinar si hay compartición (heurística: si algún peer tenía S/E/M, onSnoop ya degradó)
      // Leer línea de memoria y responder Data
      uint64_t base = t.addr & ~((uint64_t)MESICache::kLineSize-1);
      std::array<uint8_t,32> line{};
      std::memcpy(line.data(), mem.data()+base, 32);
      if (t.type==BusMsg::BusRdX) {
        // invalidar a todos (ya ocurrió por snoop), shared=false
        shared=false;
      } else {
        // BusRd: si alguien tenía la línea (S/E/M), asumimos shared=true
        // (un bus real lo sabría por señales; aquí simplificamos)
        shared=true;
      }
      caches[t.src_pe]->onDataResponse(t.addr, line.data(), shared);
    }
  }
};

int main() {
  // Construye dos caches con un minibús
  MiniBus bus;
  MesiBusIface i0, i1;
  i0.emit = [&](const BusTransaction& t){ bus.emit(t); };
  i1.emit = [&](const BusTransaction& t){ bus.emit(t); };

  MESICache c0(0, i0);
  MESICache c1(1, i1);
  bus.connect(&c0);
  bus.connect(&c1);

  // Inicializa memoria en minibús
  uint64_t addr = 0x100; // cae dentro de una línea
  uint64_t base = addr & ~((uint64_t)MESICache::kLineSize-1);
  for (int i=0;i<32;++i) bus.mem[base+i] = (uint8_t)i;

  // 1) LOAD en c0 => miss -> BusRd -> Data -> reintenta
  uint64_t out=0;
  bool ok = c0.load(addr, &out);
  assert(!ok); // primera llamada hace miss
  ok = c0.load(addr, &out);
  assert(ok);

  // 2) LOAD en c1 del mismo addr => S en ambos
  ok = c1.load(addr, &out); // miss
  assert(!ok);
  ok = c1.load(addr, &out); // hit tras Data
  assert(ok);

  // 3) STORE en c0 => BusUpgr -> M (c1 invalida por snoop)
  uint64_t val=0xAABBCCDDEEFF0011ULL;
  ok = c0.store(addr, &val); // hit en S => Upgr
  assert(ok);

  // 4) Evicción: fuerza Flush (write-back) en el MISMO set

  uint64_t v = 0xAABBCCDDEEFF0011ULL;

  // Mantén el mismo índice sumando múltiplos de 32*8 = 256 (0x100)
  uint64_t addr1 = base + 0x100; // +256 -> mismo índice, otro tag
  uint64_t addr2 = base + 0x200; // +512 -> mismo índice, otro tag

  // 1) Ensucia addr1 (miss -> BusRdX -> write -> M)
  ok = c0.store(addr1, &v);   // miss
  assert(!ok);
  ok = c0.store(addr1, &v);   // hit -> M
  assert(ok);

  // 2) Ensucia addr2: tercera línea en el mismo set de un caché 2-way => evicción.
  //    Si la víctima estaba M => Flush++
  ok = c0.store(addr2, &v);   // miss
  assert(!ok);
  ok = c0.store(addr2, &v);   // hit
  assert(ok);

  // Métricas y verificación
  const auto& s0 = c0.stats();
  const auto& s1 = c1.stats();
  std::printf(
    "PE0: loads=%llu stores=%llu misses=%llu inv=%llu rd=%llu rdx=%llu upg=%llu flush=%llu\n",
    (unsigned long long)s0.loads, (unsigned long long)s0.stores, (unsigned long long)s0.misses,
    (unsigned long long)s0.invalidations, (unsigned long long)s0.busRd, (unsigned long long)s0.busRdX,
    (unsigned long long)s0.busUpgr, (unsigned long long)s0.flush
  );
  std::printf(
    "PE1: loads=%llu stores=%llu misses=%llu inv=%llu rd=%llu rdx=%llu upg=%llu flush=%llu\n",
    (unsigned long long)s1.loads, (unsigned long long)s1.stores, (unsigned long long)s1.misses,
    (unsigned long long)s1.invalidations, (unsigned long long)s1.busRd, (unsigned long long)s1.busRdX,
    (unsigned long long)s1.busUpgr, (unsigned long long)s1.flush
  );

  std::printf("PE0 (post-eviction): flush=%llu (esperado >= 1)\n",
    (unsigned long long)s0.flush);
  assert(s0.flush >= 1);

  std::printf("OK basic MESI test\n");
  return 0;

}