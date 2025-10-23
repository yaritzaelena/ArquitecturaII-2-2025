#include <vector>
#include <array>
#include <thread>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <memory>

#include "../src/MesiInterconnect.hpp"
#include "../src/memory/SharedMemory.h"
#include "../src/memory/cache/mesi/MESICache.hpp"
#include "../PE/pe/pe.hpp"

// ---------------- Métricas simples por puerto ----------------
struct PortMetrics { uint64_t loads=0, stores=0; };

// ---------------- IMemoryPort respaldado por la caché MESI ----------------
class MesiMemoryPort : public IMemoryPort {
public:
  MesiMemoryPort(MESICache& c, MesiInterconnect& ic, PortMetrics* pm=nullptr)
  : cache_(c), ic_(ic), pm_(pm) {}

  uint64_t load64(uint64_t addr) override {
    if (pm_) pm_->loads++;
    uint64_t u = 0;
    while (!cache_.load(addr, &u)) { /* bus síncrono: segundo intento ya es hit */ }
    return u;
  }
  void store64(uint64_t addr, uint64_t val) override {
    if (pm_) pm_->stores++;
    while (!cache_.store(addr, &val)) { /* write-allocate */ }
  }
  void service() override { /* vacío para bus síncrono */ }

private:
  MESICache&        cache_;
  MesiInterconnect& ic_;
  PortMetrics*      pm_;
};

// ---------------- Helpers SharedMemory (8B) ----------------
static inline void shm_write_double(SharedMemory& shm, uint64_t addr, double v) {
  uint64_t u; std::memcpy(&u, &v, 8);
  auto req = std::make_shared<Message>(MessageType::WRITE_MEM, -1, -1);
  req->payload.write_mem.address = static_cast<uint32_t>(addr);
  req->payload.write_mem.size    = 8;
  req->data_write.resize(8);
  for (int i=0;i<8;++i) req->data_write[i] = static_cast<uint8_t>((u >> (i*8)) & 0xFF);
  shm.handle_message(req, [&](MessageP){});
}
static inline double shm_read_double(SharedMemory& shm, uint64_t addr) {
  auto req = std::make_shared<Message>(MessageType::READ_MEM, -1, -1);
  req->payload.read_mem.address = static_cast<uint32_t>(addr);
  req->payload.read_mem.size    = 8;
  double d = 0.0;
  shm.handle_message(req, [&](MessageP resp){
    if (resp && resp->type == MessageType::READ_RESP &&
        resp->payload.read_resp.status && resp->read_resp_data.size() == 8) {
      std::memcpy(&d, resp->read_resp_data.data(), 8);
    }
  });
  return d;
}

// ---------------- Programa de dot product (mini-ISA) ----------------
static Program make_dot_program() {
  Program p;
  // R0=i, R1=baseA, R2=baseB, R3=acc, R5=partial_out, R7=limit; temporales R4,R6
  p.push_back({Op::LEA,   4, 1, 0, 3}); // R4 = &A[i]
  p.push_back({Op::LEA,   6, 2, 0, 3}); // R6 = &B[i]
  p.push_back({Op::LOAD,  4, 4, 0, 0}); // A[i]
  p.push_back({Op::LOAD,  6, 6, 0, 0}); // B[i]
  p.push_back({Op::FMUL,  4, 4, 6, 0}); // t = A[i]*B[i]
  p.push_back({Op::FADD,  3, 3, 4, 0}); // acc += t
  p.push_back({Op::INC,   0, 0, 0, 0});
  p.push_back({Op::DEC,   7, 0, 0, 0});
  p.push_back({Op::JNZ,   7, 0, 0,-8});
  p.push_back({Op::STORE, 3, 5, 0, 0});
  p.push_back({Op::HALT,  0, 0, 0, 0});
  return p;
}

// ============================= MODO DOT =============================
int run_dot_mode(size_t N) { // basado en tu dotprod_mesi_main.cpp  :contentReference[oaicite:2]{index=2}
  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;

  // Layout: A[0..N-1], B[0..N-1], parciales en 4 líneas finales
  const uint64_t baseA = 0;
  const uint64_t baseB = baseA + N*8;
  const uint64_t baseP = MEM_BYTES - 4*LINE;
  const uint64_t o0 = baseP + 0*LINE;
  const uint64_t o1 = baseP + 1*LINE;
  const uint64_t o2 = baseP + 2*LINE;
  const uint64_t o3 = baseP + 3*LINE;

  if (!(baseB + N*8 <= baseP)) {
    std::fprintf(stderr, "ERROR: 2N+4 > 512 palabras (4096B). N=%zu no cabe.\n", N);
    return 2;
  }

  SharedMemory shm;
  MesiInterconnect bus(0);
  bus.set_shared_memory(&shm);

  // Inicializa datos
  for (size_t i=0; i<N; ++i) {
    shm_write_double(shm, baseA + i*8, double(i+1));
    shm_write_double(shm, baseB + i*8, 0.5*double(i+1));
  }
  shm_write_double(shm, o0, 0.0);
  shm_write_double(shm, o1, 0.0);
  shm_write_double(shm, o2, 0.0);
  shm_write_double(shm, o3, 0.0);

  // Caches + bus
  MESICache c0(0, bus), c1(1, bus), c2(2, bus), c3(3, bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  // Puertos
  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0,bus,&pm0), mp1(c1,bus,&pm1), mp2(c2,bus,&pm2), mp3(c3,bus,&pm3);

  // PEs + programa
  Program prog = make_dot_program();
  PE pe0(0,&mp0), pe1(1,&mp1), pe2(2,&mp2), pe3(3,&mp3);
  pe0.load_program(prog); pe1.load_program(prog); pe2.load_program(prog); pe3.load_program(prog);

  // Segmentación (con residuos por si N%4 != 0)
  const size_t base_chunk = N/4, rem = N%4;
  auto len_k = [&](int k){ return base_chunk + (k<rem ? 1 : 0); };
  uint64_t aK[4], bK[4], oK[4] = {o0,o1,o2,o3}; size_t off=0;
  for (int k=0;k<4;++k) {
    size_t len = len_k(k);
    aK[k] = baseA + off*8;
    bK[k] = baseB + off*8;
    off += len;
    std::printf("seg%d: A=%llu B=%llu out=%llu len=%zu\n",
      k, (unsigned long long)aK[k], (unsigned long long)bK[k],
      (unsigned long long)oK[k], len);
  }
  pe0.set_segment(aK[0], bK[0], oK[0], len_k(0));
  pe1.set_segment(aK[1], bK[1], oK[1], len_k(1));
  pe2.set_segment(aK[2], bK[2], oK[2], len_k(2));
  pe3.set_segment(aK[3], bK[3], oK[3], len_k(3));

  // Ejecutar
  std::thread t0([&]{ pe0.run(0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

  // Lectura coherente de parciales (usar el puerto!)
  auto load_double_coherent = [&](uint64_t addr)->double {
    uint64_t u = mp0.load64(addr);
    double d; std::memcpy(&d, &u, 8);
    return d;
  };
  const double p0 = load_double_coherent(o0);
  const double p1 = load_double_coherent(o1);
  const double p2 = load_double_coherent(o2);
  const double p3 = load_double_coherent(o3);
  const double result   = p0+p1+p2+p3;
  const double expected = 0.5 * (double(N)*(N+1)*(2.0*N+1)/6.0);

  std::cout << "partials = [" << p0 << ", " << p1 << ", " << p2 << ", " << p3 << "]\n";
  std::cout << "result   = " << result   << "\n";
  std::cout << "expected = " << expected << "\n";

  const auto& s0 = c0.stats();
  std::printf("PE0 stats: loads=%llu stores=%llu misses=%llu inv=%llu rd=%llu rdx=%llu upg=%llu flush=%llu\n",
    (unsigned long long)s0.loads, (unsigned long long)s0.stores, (unsigned long long)s0.cache_misses,
    (unsigned long long)s0.invalidations, (unsigned long long)s0.busRd, (unsigned long long)s0.busRdX,
    (unsigned long long)s0.busUpgr, (unsigned long long)s0.flush);

  std::printf("Port ops  : PE0(l=%llu,s=%llu) PE1(l=%llu,s=%llu) PE2(l=%llu,s=%llu) PE3(l=%llu,s=%llu)\n",
    (unsigned long long)pm0.loads,(unsigned long long)pm0.stores,
    (unsigned long long)pm1.loads,(unsigned long long)pm1.stores,
    (unsigned long long)pm2.loads,(unsigned long long)pm2.stores,
    (unsigned long long)pm3.loads,(unsigned long long)pm3.stores);

  if (std::abs(result-expected) < 1e-9*std::max(1.0, std::abs(expected))) {
    std::puts("PASS dotprod with MESI");
    return 0;
  } else {
    std::puts("FAIL dotprod with MESI");
    return 1;
  }
}

// ============================= MODO DEMO/STEPPING =============================
int run_demo_mode(bool stepping) { // inspirado en tu main.cpp  :contentReference[oaicite:3]{index=3}
  SharedMemory shm;
  MesiInterconnect bus(0);
  bus.set_shared_memory(&shm);

  MESICache c0(0,bus), c1(1,bus);
  bus.connect(&c0); bus.connect(&c1);

  auto pause = [&](const char* msg){
    if (!stepping) return;
    std::cout << "\n========== " << msg << " ==========\nPresione ENTER...\n";
    std::cin.get();
  };

  // Ciclo 1: PE0 escribe 42 @0x00
  std::cout << "\n[CICLO 1] PE0 escribe 42 @0x00\n";
  {
    uint64_t v = 42;
    while (!c0.store(0x00, &v)) {}
    pause("Fin de ciclo 1");
  }

  // Ciclo 2: PE1 lee @0x00
  std::cout << "\n[CICLO 2] PE1 lee @0x00\n";
  {
    uint64_t r=0;
    while (!c1.load(0x00, &r)) {}
    std::cout << "Leido por PE1 = " << r << "\n";
    pause("Fin de ciclo 2");
  }

  // Ciclo 3: PE0 sobrescribe 99 @0x00
  std::cout << "\n[CICLO 3] PE0 sobrescribe 99 @0x00\n";
  {
    uint64_t v = 99;
    while (!c0.store(0x00, &v)) {}
    pause("Fin de ciclo 3");
  }

  // Ciclo 4: PE1 vuelve a leer
  std::cout << "\n[CICLO 4] PE1 vuelve a leer @0x00\n";
  {
    uint64_t r=0;
    while (!c1.load(0x00, &r)) {}
    std::cout << "Leido por PE1 = " << r << "\n";
    pause("Fin de ciclo 4");
  }

  // CSV con estadísticas
  std::ofstream csv("cache_stats.csv");
  csv << "PE,Loads,Stores,RW_Accesses,Cache_Misses,Invalidations,BusRd,BusRdX,BusUpgr,Flush\n";
  const auto& sA = c0.stats();
  const auto& sB = c1.stats();
  csv << 0 << ","<< sA.loads << ","<< sA.stores << ","<< sA.rw_accesses << ","
      << sA.cache_misses << ","<< sA.invalidations << ","
      << sA.busRd << ","<< sA.busRdX << ","<< sA.busUpgr << ","<< sA.flush << "\n";
  csv << 1 << ","<< sB.loads << ","<< sB.stores << ","<< sB.rw_accesses << ","
      << sB.cache_misses << ","<< sB.invalidations << ","
      << sB.busRd << ","<< sB.busRdX << ","<< sB.busUpgr << ","<< sB.flush << "\n";
  csv.close();

  std::cout << "CSV guardado: cache_stats.csv\n";
  return 0;
}

// ============================= MAIN =============================
int main(int argc, char** argv) {
  // CLI: --mode=dot|demo  --N=248  --nostep
  std::string mode = "dot";
  size_t N = 248;
  bool stepping = true;

  for (int i=1;i<argc;++i) {
    std::string a(argv[i]);
    if      (a.rfind("--mode=",0)==0) mode = a.substr(7);
    else if (a.rfind("--N=",0)==0)    N = std::stoul(a.substr(4));
    else if (a=="--nostep")           stepping = false;
  }

  if (mode=="dot")   return run_dot_mode(N);
  if (mode=="demo")  return run_demo_mode(stepping);

  std::fprintf(stderr,"Uso: %s [--mode=dot|demo] [--N=248] [--nostep]\n", argv[0]);
  return 1;
}

