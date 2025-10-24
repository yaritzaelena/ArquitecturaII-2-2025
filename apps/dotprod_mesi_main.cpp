/*
 * dotprod_mesi_main.cpp
 * ---------------------
 * Integra 4 PEs + L1$ MESI + Interconnect + Memoria Compartida para calcular
 * el producto punto en doble precisión de forma paralela.
 *
 * ¿Qué hace este main?
 *  - Reserva el layout de memoria (A, B y 4 parciales) en SharedMemory (4096 B).
 *  - Inicializa A[i] = i+1 y B[i] = 0.5*(i+1) para N=248.
 *  - Crea 4 cachés MESI (una por PE) y las conecta al interconect.
 *  - Construye el “programa” mini-ISA (LEA/LOAD/FMUL/FADD/INC/DEC/JNZ/STORE/HALT).
 *  - Parte N en 4 segmentos contiguos (N/4 por PE) y asigna el tramo a cada PE.
 *  - Ejecuta los 4 PEs (hilos) y, al final, lee los 4 parciales (vía caché) y valida.
 *
 * Detalles de coherencia:
 *  - Lecturas de A y B tienden a instalar líneas en S (BusRd).
 *  - Cada PE escribe su parcial en SU PROPIA línea (evita false sharing);
 *    requiere exclusividad (BusRdX/BusUpgr) y la línea queda en M.
 *  - Evicción de una línea M emite Flush (write-back).
 *
 * Notas de implementación:
 *  - Se usa un bus/interconect SINCRONO simple: el primer load/store puede “fallar”
 *    (load()/store() devuelve false) para forzar la emisión de BusRd/BusRdX; el
 *    segundo intento ya completa (onDataResponse).
 *  - MesiMemoryPort envuelve la caché y expone load64/store64 al PE.
 *  - Las direcciones y longitudes se imprimen para comprobar la segmentación.
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

#include "../src/MesiInterconnect.hpp"
#include "../src/memory/cache/mesi/MESICache.hpp"
#include "../src/memory/SharedMemory.h"
#include "../PE/pe/pe.hpp"

// ---------------- Métricas simples por puerto ----------------
// Solo para contar invocaciones desde el PE (no son las métricas internas de la L1$)
struct PortMetrics { uint64_t loads=0, stores=0; };

// ---------------- IMemoryPort respaldado por la caché MESI ----------------
// Adapta la interfaz del PE (IMemoryPort) a la L1$ MESI.
// En este modelo síncrono, si load()/store() devuelve false es porque se emitió
// BusRd/BusRdX; el reintento ya es hit (tras onDataResponse del bus).
class MesiMemoryPort : public IMemoryPort {
public:
  MesiMemoryPort(MESICache& cache, MesiInterconnect& ic, PortMetrics* pm=nullptr)
  : cache_(cache), ic_(ic), pm_(pm) {}

  // Lee 8B coherentemente (alineación implícita a offset dentro de la línea).
  uint64_t load64(uint64_t addr) override {
    if (pm_) pm_->loads++;
    uint64_t u = 0;
    while (!cache_.load(addr, &u)) {
      // bus síncrono: el segundo intento ya es hit al llegar onDataResponse
    }
    return u;
  }

  // Escribe 8B coherentemente (write-allocate + write-back).
  void store64(uint64_t addr, uint64_t val) override {
    if (pm_) pm_->stores++;
    while (!cache_.store(addr, &val)) {
      // write-allocate: tras el Data/propiedad, el reintento completa
    }
  }

  // En un bus asíncrono aquí se bombearía la cola del bus. En este demo no hace falta.
  void service() override { /* vacío para bus síncrono */ }

private:
  MESICache&        cache_;
  MesiInterconnect& ic_;
  PortMetrics*      pm_;
};

// ---------------- Programa de dot product (mini-ISA) ----------------
// R0=i, R1=baseA, R2=baseB, R3=acc, R5=partial_out, R7=limit; temporales R4,R6
// Por iteración:
//   R4 = &A[i], R6 = &B[i]
//   R4 = *R4;   R6 = *R6
//   R4 = R4*R6; acc += R4
//   i++, limit--, repetir si limit!=0
//   [partial_out] = acc
static Program make_dot_program() {
  Program p;
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

// -------- Helpers: escribir/leer double en SharedMemory (8B) --------
// Se usan solo para la inicialización/lectura de verificación a memoria compartida.
// El cómputo en sí siempre accede a través de la caché (vía MesiMemoryPort).
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

int main() {
  // ====== Layout FIJO para SharedMemory (4096 B, línea = 32 B) ======
  // Memoria total pequeña para el demo de N=248:
  //   - A: N*8 bytes
  //   - B: N*8 bytes
  //   - Parciales: 4 dobles, cada uno en su PROPIA línea al final de la memoria
  //                (evita false sharing entre PEs).
  static constexpr uint64_t MEM_BYTES = 4096;
  static constexpr uint64_t LINE      = 32;
  static constexpr size_t   N         = 248;  // N de la especificación (se divide 4)

  // Bases de A y B (contiguos en memoria)
  static constexpr uint64_t baseA = 0;
  static constexpr uint64_t baseB = baseA + N * 8;

  // Parciales: ubicados en las últimas 4 líneas de la memoria
  static constexpr uint64_t baseP = MEM_BYTES - 4 * LINE;
  const uint64_t o0 = baseP + 0*LINE;
  const uint64_t o1 = baseP + 1*LINE;
  const uint64_t o2 = baseP + 2*LINE;
  const uint64_t o3 = baseP + 3*LINE;

  // Seguridad: garantizar que A y B caben antes de los parciales
  static_assert(baseB + N*8 <= baseP, "A+B no caben antes de los parciales");

  // --- Interconnect + SharedMemory ---
  SharedMemory shm;
  MesiInterconnect bus(/*irrelevante*/0);
  bus.set_shared_memory(&shm); // El bus resolverá Data/Flush contra esta memoria

  // --- Inicializa A, B, parciales en SharedMemory ---
  for (size_t i = 0; i < N; ++i) {
    shm_write_double(shm, baseA + i * 8, double(i + 1));           // A[i] = 1..N
    shm_write_double(shm, baseB + i * 8, 0.5 * double(i + 1));     // B[i] = 0.5,1.0,1.5,...
  }
  // Inicializa parciales a 0.0
  shm_write_double(shm, o0, 0.0);
  shm_write_double(shm, o1, 0.0);
  shm_write_double(shm, o2, 0.0);
  shm_write_double(shm, o3, 0.0);

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

  // Segmentos: 4 tramos contiguos de tamaño N/4
  const size_t   chunk = N / 4;  assert(chunk > 0);
  const uint64_t a0 = baseA + 0 * chunk * 8, b0 = baseB + 0 * chunk * 8;
  const uint64_t a1 = baseA + 1 * chunk * 8, b1 = baseB + 1 * chunk * 8;
  const uint64_t a2 = baseA + 2 * chunk * 8, b2 = baseB + 2 * chunk * 8;
  const uint64_t a3 = baseA + 3 * chunk * 8, b3 = baseB + 3 * chunk * 8;

  // Log de segmentos (útil para verificar alineación y longitudes)
  std::printf("seg0: A=%llu B=%llu out=%llu len=%llu\n",
    (unsigned long long)a0,(unsigned long long)b0,(unsigned long long)o0,(unsigned long long)chunk);
  std::printf("seg1: A=%llu B=%llu out=%llu len=%llu\n",
    (unsigned long long)a1,(unsigned long long)b1,(unsigned long long)o1,(unsigned long long)chunk);
  std::printf("seg2: A=%llu B=%llu out=%llu len=%llu\n",
    (unsigned long long)a2,(unsigned long long)b2,(unsigned long long)o2,(unsigned long long)chunk);
  std::printf("seg3: A=%llu B=%llu out=%llu len=%llu\n",
    (unsigned long long)a3,(unsigned long long)b3,(unsigned long long)o3,(unsigned long long)chunk);

  // Configurar segmentos en cada PE (bases de A/B, dirección de parcial y longitud)
  pe0.set_segment(a0, b0, o0, chunk);
  pe1.set_segment(a1, b1, o1, chunk);
  pe2.set_segment(a2, b2, o2, chunk);
  pe3.set_segment(a3, b3, o3, chunk);

  // --- Ejecutar en 4 hilos (uno por PE) ---
  std::thread t0([&]{ pe0.run(0); });
  std::thread t1([&]{ pe1.run(0); });
  std::thread t2([&]{ pe2.run(0); });
  std::thread t3([&]{ pe3.run(0); });
  t0.join(); t1.join(); t2.join(); t3.join();

  // --- Lectura COHERENTE de parciales (vía L1$/Bus) ---
  // Leemos a través del puerto (y por ende de la caché) para respetar coherencia.
  auto load_double_coherent = [&](uint64_t addr) -> double {
    uint64_t u = mp0.load64(addr); // usamos PE0 para las lecturas finales
    double d; std::memcpy(&d, &u, 8);
    return d;
  };

  const double p0 = load_double_coherent(o0);
  const double p1 = load_double_coherent(o1);
  const double p2 = load_double_coherent(o2);
  const double p3 = load_double_coherent(o3);
  const double result = p0 + p1 + p2 + p3;

  // Referencia analítica:
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

  // Validación numérica con tolerancia relativa
  if (std::abs(result - expected) < 1e-9 * std::max(1.0, std::abs(expected))) {
    std::puts("PASS dotprod with MESI");
    return 0;
  } else {
    std::puts("FAIL dotprod with MESI");
    return 1;
  }
}

