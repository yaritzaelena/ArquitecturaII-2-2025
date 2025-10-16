#include <cassert>
#include <cstdio>
#include <vector>
#include <array>
#include <cstring>
#include "../../../src/memory/cache/mesi/MESICache.hpp"

struct MiniBus {
  std::vector<MESICache*> caches;
  std::array<uint8_t, 4096> mem{};
  void connect(MESICache* c) { caches.push_back(c); }
  void emit(const BusTransaction& t) {
    // Snoops a todos menos al emisor
    for (auto* c: caches) if (c && c != caches[t.src_pe]) c->onSnoop(t);
    // Write-back si llega Flush
    if (t.type==BusMsg::Flush && t.payload) {
      uint64_t base = t.addr & ~((uint64_t)MESICache::kLineSize-1);
      std::memcpy(mem.data()+base, t.payload, 32);
    }
    // Respuesta de datos
    if (t.type==BusMsg::BusRd || t.type==BusMsg::BusRdX) {
      uint64_t base = t.addr & ~((uint64_t)MESICache::kLineSize-1);
      std::array<uint8_t,32> line{};
      std::memcpy(line.data(), mem.data()+base, 32);
      bool shared = (t.type==BusMsg::BusRd); // simplificado
      caches[t.src_pe]->onDataResponse(t.addr, line.data(), shared);
    }
  }
};

int main() {
  // Crea bus y dos caches
  MiniBus bus;
  MesiBusIface i0, i1;
  i0.emit = [&](const BusTransaction& t){ bus.emit(t); };
  i1.emit = [&](const BusTransaction& t){ bus.emit(t); };

  MESICache c0(0, i0), c1(1, i1);
  bus.connect(&c0); bus.connect(&c1);

  // Inicializa memoria base
  uint64_t addr = 0x200;
  uint64_t base = addr & ~((uint64_t)MESICache::kLineSize-1);
  for (int i=0;i<32;++i) bus.mem[base+i] = (uint8_t)0x11;

  // 1) c0 escribe -> M (miss -> BusRdX, luego hit)
  uint64_t v = 0xDEADBEEFCAFEBABEULL;
  bool ok = c0.store(addr, &v);  // miss
  assert(!ok); ok = c0.store(addr, &v); assert(ok);

  // 2) c1 lee -> fuerza Flush en c0 y downgrade a S
  uint64_t out=0;
  ok = c1.load(addr, &out);  // miss
  assert(!ok); ok = c1.load(addr, &out); assert(ok);

  // Memoria debe tener el valor escrito por c0 (write-back)
  uint64_t memval=0;
  std::memcpy(&memval, bus.mem.data()+base+(addr & 31), 8);
  assert(memval == v);

  std::puts("OK M->S downgrade with Flush");
  return 0;
}
