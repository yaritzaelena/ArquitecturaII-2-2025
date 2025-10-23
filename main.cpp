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
#include "../src/utils/Stepper.hpp"   // ✅ Nuevo include
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
// ============================= MODO DOT =============================
int run_dot_mode(size_t N) {
  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;

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

  for (size_t i=0; i<N; ++i) {
    shm_write_double(shm, baseA + i*8, double(i+1));
    shm_write_double(shm, baseB + i*8, 0.5*double(i+1));
  }
  shm_write_double(shm, o0, 0.0);
  shm_write_double(shm, o1, 0.0);
  shm_write_double(shm, o2, 0.0);
  shm_write_double(shm, o3, 0.0);

  MESICache c0(0,bus), c1(1,bus), c2(2,bus), c3(3,bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0,bus,&pm0), mp1(c1,bus,&pm1), mp2(c2,bus,&pm2), mp3(c3,bus,&pm3);

  Program prog = make_dot_program();
  PE pe0(0,&mp0), pe1(1,&mp1), pe2(2,&mp2), pe3(3,&mp3);
  pe0.load_program(prog); pe1.load_program(prog); pe2.load_program(prog); pe3.load_program(prog);

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

  std::thread t0([&]{ pe0.run(0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

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

  // ---------- Exportar métricas a CSV ----------
  std::ofstream csv("cache_stats.csv");
  csv << "PE,Loads,Stores,RW_Accesses,Cache_Misses,Invalidations,"
         "BusRd,BusRdX,BusUpgr,Flush,Transitions\n";

  auto write_cache = [&](int pe, const MESICache& cache) {
      const auto& s = cache.stats();
      csv << pe << ","
          << s.loads << ","
          << s.stores << ","
          << (s.loads + s.stores) << ","
          << s.cache_misses << ","
          << s.invalidations << ","
          << s.busRd << ","
          << s.busRdX << ","
          << s.busUpgr << ","
          << s.flush << ",\""
          << cache.transition_log() << "\"\n";
  };

  write_cache(0, c0);
  write_cache(1, c1);
  write_cache(2, c2);
  write_cache(3, c3);
  csv.close();
  std::cout << "✅ Métricas exportadas a cache_stats.csv\n";

  if (std::abs(result-expected) < 1e-9*std::max(1.0, std::abs(expected))) {
    std::puts("PASS dotprod with MESI");
    return 0;
  } else {
    std::puts("FAIL dotprod with MESI");
    return 1;
  }
}


// ============================= MODO DEMO (BUS STEPPING) =============================
int run_demo_mode(size_t N, bool stepping) {
  std::cout << "\n===== DEMO: Visualizacion de coherencia MESI =====\n";
  std::cout << "Vector size N = " << N << "\n";
  if (stepping) std::cout << "Presione Siguiente evento para avanzar entre eventos del BUS...\n\n";

  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;

  const uint64_t baseA = 0;
  const uint64_t baseB = baseA + N*8;
  const uint64_t baseP = MEM_BYTES - 4*LINE;
  const uint64_t o0 = baseP + 0*LINE;
  const uint64_t o1 = baseP + 1*LINE;
  const uint64_t o2 = baseP + 2*LINE;
  const uint64_t o3 = baseP + 3*LINE;

  SharedMemory shm;
  MesiInterconnect bus(0);
  bus.set_shared_memory(&shm);

  // Stepper para visualización del BUS
  Stepper step;
  step.enabled = stepping;
  bus.set_stepper(&step);

  for (size_t i=0; i<N; ++i) {
    shm_write_double(shm, baseA + i*8, double(i+1));
    shm_write_double(shm, baseB + i*8, 0.5*double(i+1));
  }

  MESICache c0(0,bus), c1(1,bus), c2(2,bus), c3(3,bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0,bus,&pm0), mp1(c1,bus,&pm1), mp2(c2,bus,&pm2), mp3(c3,bus,&pm3);

  Program prog = make_dot_program();
  PE pe0(0,&mp0), pe1(1,&mp1), pe2(2,&mp2), pe3(3,&mp3);
  pe0.load_program(prog); pe1.load_program(prog); pe2.load_program(prog); pe3.load_program(prog);

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

  std::thread t0([&]{ pe0.run(0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

  double p0 = shm_read_double(shm, o0);
  double p1 = shm_read_double(shm, o1);
  double p2 = shm_read_double(shm, o2);
  double p3 = shm_read_double(shm, o3);
  double result = p0 + p1 + p2 + p3;
  double expected = 0.5 * (double(N)*(N+1)*(2.0*N+1)/6.0);

  // ---------- Exportar métricas a CSV ----------
std::ofstream csv("cache_stats.csv");
csv << "PE,Loads,Stores,RW_Accesses,Cache_Misses,Invalidations,"
       "BusRd,BusRdX,BusUpgr,Flush,Transitions\n";

auto write_cache = [&](int pe, const MESICache& cache) {
    const auto& s = cache.stats();
    csv << pe << ","
        << s.loads << ","
        << s.stores << ","
        << (s.loads + s.stores) << ","
        << s.cache_misses << ","
        << s.invalidations << ","
        << s.busRd << ","
        << s.busRdX << ","
        << s.busUpgr << ","
        << s.flush << ",\""
        << cache.transition_log() << "\"\n";
};

write_cache(0, c0);
write_cache(1, c1);
write_cache(2, c2);
write_cache(3, c3);
csv.close();

std::cout << "✅ Métricas exportadas";


  return 0;
}

// ============================= MAIN =============================
int main(int argc, char** argv) {
  std::string mode = "dot";
  size_t N = 248;
  bool stepping = true;

  for (int i=1;i<argc;++i) {
    std::string a(argv[i]);
    if      (a.rfind("--mode=",0)==0) mode = a.substr(7);
    else if (a.rfind("--N=",0)==0)    N = std::stoul(a.substr(4));
    else if (a=="--nostep")           stepping = false;
  }

  if (mode == "dot")  return run_dot_mode(N);
  if (mode == "demo") return run_demo_mode(N, stepping);

  std::fprintf(stderr,"Uso: %s [--mode=dot|demo] [--N=248] [--nostep]\n", argv[0]);
  return 1;
}
