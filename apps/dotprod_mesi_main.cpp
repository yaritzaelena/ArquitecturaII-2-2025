#include <vector>
#include <array>
#include <thread>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <iostream>
#include "../src/memory/cache/mesi/MesiInterconnect.hpp"
#include "../src/memory/cache/mesi/MESICache.hpp"
#include "../PE/pe/pe.hpp"

// ---------------- Métricas simples por puerto ----------------
struct PortMetrics { uint64_t loads=0, stores=0; };

// ---------------- IMemoryPort respaldado por la caché MESI ----------------
class MesiMemoryPort : public IMemoryPort {
public:
  MesiMemoryPort(MESICache& cache, MesiInterconnect& ic, PortMetrics* pm=nullptr)
  : cache_(cache), ic_(ic), pm_(pm) {}

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

// ---------------- Programa de dot product (mini-ISA) ----------------
static Program make_dot_program() {
  Program p;
  // Convención: R0=i, R1=baseA, R2=baseB, R3=acc, R5=partial_out, R7=limit; temporales R4,R6
  p.push_back({Op::LEA,   4, 1, 0, 3});   // R4 = &A[i] = R1 + (R0<<3)
  p.push_back({Op::LEA,   6, 2, 0, 3});   // R6 = &B[i] = R2 + (R0<<3)
  p.push_back({Op::LOAD,  4, 4, 0, 0});   // R4 = A[i]
  p.push_back({Op::LOAD,  6, 6, 0, 0});   // R6 = B[i]
  p.push_back({Op::FMUL,  4, 4, 6, 0});   // R4 = A[i] * B[i]
  p.push_back({Op::FADD,  3, 3, 4, 0});   // acc += R4
  p.push_back({Op::INC,   0, 0, 0, 0});   // i++
  p.push_back({Op::DEC,   7, 0, 0, 0});   // limit--
  p.push_back({Op::JNZ,   7, 0, 0,-8});   // loop mientras R7 != 0
  p.push_back({Op::STORE, 3, 5, 0, 0});   // [partial_out] = acc
  p.push_back({Op::HALT,  0, 0, 0, 0});
  return p;
}

// ---------------- Helpers para escribir/leer double crudo en DRAM ----------------
static inline void dram_write_double(std::vector<uint8_t>& m, size_t addr, double v) {
  uint64_t u; std::memcpy(&u, &v, 8);
  for (int i = 0; i < 8; ++i) m[addr + i] = static_cast<uint8_t>((u >> (i * 8)) & 0xFF);
}

int main() {
  // --- Parámetros ---
  // Si quieres cumplir estrictamente la espec (512 palabras), usa N <= 252 (p.ej., N=252).
  const size_t N = 1024; // puedes bajar a 252 para ajustarte a 512 palabras
  const size_t baseA = 0;
  const size_t baseB = baseA + N * 8;
  const size_t baseP = baseB + N * 8;              // base de parciales
  const size_t strideP = MESICache::kLineSize;      // 32B por parcial (evita false sharing)
  const size_t SPEC_MIN_BYTES = 512 * 8;            // mínimo de la espec: 512 palabras de 64b
  const size_t BYTES_NEEDED = baseP + 4 * strideP;  // A+B + 4 parciales (en líneas separadas)
  const size_t DRAM_BYTES = std::max(BYTES_NEEDED, SPEC_MIN_BYTES);

  const size_t chunk = N / 4;            // 4 PEs → N/4 c/u
  assert(chunk > 0);

  // --- Interconnect + DRAM ---
  MesiInterconnect bus(DRAM_BYTES);

  // Inicializa A = [1..N], B = [0.5,1.0,1.5,...], parciales en 0
  auto& dram = bus.dram();
  for (size_t i = 0; i < N; ++i) {
    dram_write_double(dram, baseA + i * 8, double(i + 1));
    dram_write_double(dram, baseB + i * 8, 0.5 * double(i + 1));
  }
  for (int k = 0; k < 4; ++k) dram_write_double(dram, baseP + k * strideP, 0.0);

  // --- Caches MESI + conexión al bus ---
  MESICache c0(0, bus), c1(1, bus), c2(2, bus), c3(3, bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  // --- Puertos de memoria (uno por PE, sobre su L1$) ---
  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0, bus, &pm0), mp1(c1, bus, &pm1), mp2(c2, bus, &pm2), mp3(c3, bus, &pm3);

  // --- PEs + programa ---
  Program prog = make_dot_program();
  PE pe0(0, &mp0), pe1(1, &mp1), pe2(2, &mp2), pe3(3, &mp3);
  pe0.load_program(prog); pe1.load_program(prog);
  pe2.load_program(prog); pe3.load_program(prog);

  // Segmentos: 4 tramos contiguos
  const size_t a0 = baseA + 0 * chunk * 8, b0 = baseB + 0 * chunk * 8, o0 = baseP + 0 * strideP;
  const size_t a1 = baseA + 1 * chunk * 8, b1 = baseB + 1 * chunk * 8, o1 = baseP + 1 * strideP;
  const size_t a2 = baseA + 2 * chunk * 8, b2 = baseB + 2 * chunk * 8, o2 = baseP + 2 * strideP;
  const size_t a3 = baseA + 3 * chunk * 8, b3 = baseB + 3 * chunk * 8, o3 = baseP + 3 * strideP;

  // Chequeos de rango (evita errores silenciosos)
  assert(a0 + chunk*8 <= baseA + N*8);
  assert(a1 + chunk*8 <= baseA + N*8);
  assert(a2 + chunk*8 <= baseA + N*8);
  assert(a3 + chunk*8 <= baseA + N*8);
  assert(o3 + 8 <= baseP + 4*strideP);

  // Log de segmentos
  std::printf("seg0: A=%zu B=%zu out=%zu len=%zu\n", a0, b0, o0, chunk);
  std::printf("seg1: A=%zu B=%zu out=%zu len=%zu\n", a1, b1, o1, chunk);
  std::printf("seg2: A=%zu B=%zu out=%zu len=%zu\n", a2, b2, o2, chunk);
  std::printf("seg3: A=%zu B=%zu out=%zu len=%zu\n", a3, b3, o3, chunk);

  pe0.set_segment(a0, b0, o0, chunk);
  pe1.set_segment(a1, b1, o1, chunk);
  pe2.set_segment(a2, b2, o2, chunk);
  pe3.set_segment(a3, b3, o3, chunk);

  // --- Ejecutar en hilos ---
  std::thread t0([&]{ pe0.run(/*max_steps=*/0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

  // --- Diagnóstico: leer parciales directo de DRAM (sin caché) ---
  auto read_double_dram = [&](uint64_t addr)->double{
    double d;
    std::memcpy(&d, dram.data() + addr, 8);
    return d;
  };
  double d0 = read_double_dram(o0);
  double d1 = read_double_dram(o1);
  double d2 = read_double_dram(o2);
  double d3 = read_double_dram(o3);
  std::cout << "DRAM partials = ["<< d0 << ", " << d1 << ", " << d2 << ", " << d3 << "]\n";

  // --- Lectura COHERENTE de parciales (a través de la caché/Bus) ---
  auto load_double_coherent = [&](uint64_t addr) -> double {
    uint64_t u = mp0.load64(addr);
    double d; std::memcpy(&d, &u, 8);
    return d;
  };

  const double p0 = load_double_coherent(o0);
  const double p1 = load_double_coherent(o1);
  const double p2 = load_double_coherent(o2);
  const double p3 = load_double_coherent(o3);
  const double result = p0 + p1 + p2 + p3;

  // sum_{i=1..N} i * (0.5 i) = 0.5 * sum i^2 = 0.5 * N(N+1)(2N+1)/6
  const double expected = 0.5 * (double(N) * (N + 1) * (2.0 * N + 1) / 6.0);

  std::cout << "partials = [" << p0 << ", " << p1 << ", " << p2 << ", " << p3 << "]\n";
  std::cout << "result   = " << result   << "\n";
  std::cout << "expected = " << expected << "\n";

  // Métricas de ejemplo (PE0) + métricas por puerto
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

  if (std::abs(result - expected) < 1e-9 * std::max(1.0, std::abs(expected))) {
    std::puts("PASS dotprod with MESI");
    return 0;
  } else {
    std::puts("FAIL dotprod with MESI");
    return 1;
  }
}
