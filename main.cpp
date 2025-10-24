/*
 * main.cpp (unificado)
 * --------------------
 * Ejecutable con dos modos:
 *   1) --mode=dot  : corre el producto punto en doble precisión con 4 PEs,
 *                    usando L1$ MESI + Interconnect + SharedMemory. Exporta métricas a CSV.
 *   2) --mode=demo : igual que dot, pero habilita stepping del BUS (si Stepper está integrado)
 *                    para visualizar las emisiones BusRd/BusRdX/BusUpgr/Flush y los snoops.
 *
 * Estructura general:
 *  - SharedMemory: memoria compartida “DRAM” del modelo.
 *  - MesiInterconnect: bus/colectivo que difunde snoops y entrega Data/Flush.
 *  - MESICache: caché L1 por PE, coherente con MESI (M/E/S/I).
 *  - MesiMemoryPort: adapta la L1$ a la interfaz IMemoryPort del PE (load64/store64).
 *  - PE: ejecuta un pequeño “programa” (mini-ISA) para el dot product.
 *
 * Flujo en --mode=dot:
 *  - Layout: A y B contiguos desde 0; 4 “parciales” al final de la memoria (cada parcial en su línea).
 *  - Inicializa A[i]=i+1 y B[i]=0.5*(i+1).
 *  - Conecta 4 cachés al bus e instancia 4 puertos de memoria + 4 PEs.
 *  - Cada PE procesa N/4 (o N/4±1 si N%4!=0) y escribe su parcial en su propia línea (evita false sharing).
 *  - Junta los 4 parciales y valida contra la fórmula cerrada.
 *  - Exporta métricas de cada L1$ a cache_stats.csv (para graficar luego).
 *
 * Notas importantes:
 *  - Bus “síncrono” simplificado: la primera llamada a cache_.load/store puede devolver false
 *    (porque se emitió BusRd/BusRdX). El reintento ya completa tras onDataResponse() del bus.
 *  - La línea de parcial de cada PE permanece en M (exclusiva) hasta flush en evicción o al final.
 */

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
#include "../src/utils/Stepper.hpp"   //  Visualizador del BUS (opcional en --mode=demo)
#include "../PE/pe/pe.hpp"

// ---------------- Métricas simples por puerto ----------------
// Contadores del “front-end” (llamadas del PE al puerto). Son independientes
// de las métricas internas de la caché (misses, invalidations, etc.).
struct PortMetrics { uint64_t loads=0, stores=0; };

// ---------------- IMemoryPort respaldado por la caché MESI ----------------
// Adapta la L1$ MESI a la interfaz del PE. Implementa load64/store64 a 8B.
class MesiMemoryPort : public IMemoryPort {
public:
  MesiMemoryPort(MESICache& c, MesiInterconnect& ic, PortMetrics* pm=nullptr)
  : cache_(c), ic_(ic), pm_(pm) {}

  // Lee 8 bytes coherentemente. Si la caché devuelve false, es porque emitió BusRd.
  // En este modelo, el segundo intento ya es hit (onDataResponse).
  uint64_t load64(uint64_t addr) override {
    if (pm_) pm_->loads++;
    uint64_t u = 0;
    while (!cache_.load(addr, &u)) { /* bus síncrono: segundo intento ya es hit */ }
    return u;
  }

  // Escribe 8 bytes coherentemente. Si devuelve false, la caché emitió BusRdX/Upgr.
  void store64(uint64_t addr, uint64_t val) override {
    if (pm_) pm_->stores++;
    while (!cache_.store(addr, &val)) { /* write-allocate */ }
  }

  // Un bus asíncrono podría requerir “bombear” colas aquí.
  void service() override { /* vacío para bus síncrono */ }

private:
  MESICache&        cache_;
  MesiInterconnect& ic_;
  PortMetrics*      pm_;
};

// ---------------- Helpers SharedMemory (acceso directo de 8B) ----------------
// Se usan para inicializar/verificar memoria compartida sin pasar por la caché.
// El cómputo de los PEs SIEMPRE pasa por la L1$ a través del MesiMemoryPort.
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
// R0=i, R1=baseA, R2=baseB, R3=acc, R5=partial_out, R7=limit; temporales R4,R6
static Program make_dot_program() {
  Program p;
  p.push_back({Op::LEA,   4, 1, 0, 3}); // R4 = &A[i]
  p.push_back({Op::LEA,   6, 2, 0, 3}); // R6 = &B[i]
  p.push_back({Op::LOAD,  4, 4, 0, 0}); // A[i]
  p.push_back({Op::LOAD,  6, 6, 0, 0}); // B[i]
  p.push_back({Op::FMUL,  4, 4, 6, 0}); // t = A[i]*B[i]
  p.push_back({Op::FADD,  3, 3, 4, 0}); // acc += t
  p.push_back({Op::INC,   0, 0, 0, 0});
  p.push_back({Op::DEC,   7, 0, 0, 0});
  p.push_back({Op::JNZ,   7, 0, 0,-8});
  p.push_back({Op::STORE, 3, 5, 0, 0}); // [partial_out] = acc
  p.push_back({Op::HALT,  0, 0, 0, 0});
  return p;
}

// ===================================================================
// ============================= MODO DOT =============================
// ===================================================================
// Ejecuta el dot product con 4 PEs y exporta cache_stats.csv
int run_dot_mode(size_t N) {
  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;

  // Layout: A y B contiguos desde 0; parciales en las últimas 4 líneas
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

  // DRAM + BUS
  SharedMemory shm;
  MesiInterconnect bus(0);
  bus.set_shared_memory(&shm);

  // Inicialización A/B y parciales
  for (size_t i=0; i<N; ++i) {
    shm_write_double(shm, baseA + i*8, double(i+1));        // A[i] = 1..N
    shm_write_double(shm, baseB + i*8, 0.5*double(i+1));    // B[i] = 0.5,1.0,1.5,...
  }
  shm_write_double(shm, o0, 0.0);
  shm_write_double(shm, o1, 0.0);
  shm_write_double(shm, o2, 0.0);
  shm_write_double(shm, o3, 0.0);

  // 4 L1$ MESI conectadas al bus
  MESICache c0(0,bus), c1(1,bus), c2(2,bus), c3(3,bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  // 4 puertos de memoria (uno por PE)
  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0,bus,&pm0), mp1(c1,bus,&pm1), mp2(c2,bus,&pm2), mp3(c3,bus,&pm3);

  // Programa mini-ISA y PEs
  Program prog = make_dot_program();
  PE pe0(0,&mp0), pe1(1,&mp1), pe2(2,&mp2), pe3(3,&mp3);
  pe0.load_program(prog); pe1.load_program(prog); pe2.load_program(prog); pe3.load_program(prog);

  // Segmentación: reparte N entre 4 (balancea si N%4!=0)
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

  // Asignar tramos
  pe0.set_segment(aK[0], bK[0], oK[0], len_k(0));
  pe1.set_segment(aK[1], bK[1], oK[1], len_k(1));
  pe2.set_segment(aK[2], bK[2], oK[2], len_k(2));
  pe3.set_segment(aK[3], bK[3], oK[3], len_k(3));

  // Ejecutar en paralelo
  std::thread t0([&]{ pe0.run(0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

  // Leer parciales coherentemente a través del puerto (y por ende de la L1$)
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

  // ---------- Exportar métricas de cada L1$ a CSV ----------
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
  std::cout << " Métricas exportadas a cache_stats.csv\n";

  if (std::abs(result-expected) < 1e-9*std::max(1.0, std::abs(expected))) {
    std::puts("PASS dotprod with MESI");
    return 0;
  } else {
    std::puts("FAIL dotprod with MESI");
    return 1;
  }
}


// ===================================================================
// ===================== MODO DEMO (BUS STEPPING) =====================
// ===================================================================
// Igual que --mode=dot, pero activa Stepper en el BUS para pausar/avanzar
// entre emisiones y snoops (útil en presentaciones).
int run_demo_mode(size_t N, bool stepping) {
  std::cout << "\n===== DEMO: Visualizacion de coherencia MESI =====\n";
  std::cout << "Vector size N = " << N << "\n";
  if (stepping) std::cout << "Presione Siguiente evento para avanzar entre eventos del BUS...\n\n";

  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;

  // Layout básico
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

  // Stepper para visualización del BUS (si Stepper.hpp soporta set_stepper())
  Stepper step;
  step.enabled = stepping;
  bus.set_stepper(&step);

  // Inicialización
  for (size_t i=0; i<N; ++i) {
    shm_write_double(shm, baseA + i*8, double(i+1));
    shm_write_double(shm, baseB + i*8, 0.5*double(i+1));
  }

  // Caches + puertos + PEs
  MESICache c0(0,bus), c1(1,bus), c2(2,bus), c3(3,bus);
  bus.connect(&c0); bus.connect(&c1); bus.connect(&c2); bus.connect(&c3);

  PortMetrics pm0, pm1, pm2, pm3;
  MesiMemoryPort mp0(c0,bus,&pm0), mp1(c1,bus,&pm1), mp2(c2,bus,&pm2), mp3(c3,bus,&pm3);

  Program prog = make_dot_program();
  PE pe0(0,&mp0), pe1(1,&mp1), pe2(2,&mp2), pe3(3,&mp3);
  pe0.load_program(prog); pe1.load_program(prog); pe2.load_program(prog); pe3.load_program(prog);

  // Segmentación balanceada
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

  // En demo leemos parciales directo de DRAM para simplificar la impresión
  double p0 = shm_read_double(shm, o0);
  double p1 = shm_read_double(shm, o1);
  double p2 = shm_read_double(shm, o2);
  double p3 = shm_read_double(shm, o3);
  double result = p0 + p1 + p2 + p3;
  double expected = 0.5 * (double(N)*(N+1)*(2.0*N+1)/6.0);

  // ---------- Exportar métricas de cada L1$ a CSV ----------
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

  std::cout << "Métricas exportadas\n";

  // (En demo no imprimimos PASS/FAIL; puedes añadirlo si deseas)
  (void)result; (void)expected;
  return 0;
}

// ===================================================================
// ================================ MAIN ==============================
// ===================================================================
int main(int argc, char** argv) {
  std::string mode = "dot";  // por defecto ejecuta dot product
  size_t N = 248;            // valor por defecto del enunciado
  bool stepping = true;      // stepping del BUS en --mode=demo (desactivable con --nostep)

  // Parseo de flags
  for (int i=1;i<argc;++i) {
    std::string a(argv[i]);
    if      (a.rfind("--mode=",0)==0) mode = a.substr(7);     // dot | demo
    else if (a.rfind("--N=",0)==0)    N = std::stoul(a.substr(4));
    else if (a=="--nostep")           stepping = false;       // solo relevante en demo
  }

  if (mode == "dot")  return run_dot_mode(N);
  if (mode == "demo") return run_demo_mode(N, stepping);

  std::fprintf(stderr,"Uso: %s [--mode=dot|demo] [--N=248] [--nostep]\n", argv[0]);
  return 1;
}

