#pragma once
#include "../MesiTypes.hpp"
#include <functional>
#include <vector>

struct MesiBusIface {
  // Emitir transacción al bus (interconnect)
  std::function<void(const BusTransaction&)> emit;

  // Registrar este caché para recibir snoops (el interconnect llamará esto)
  // En la integración real, usarás un registro centralizado de caches en el bus.
  std::function<void(std::function<void(const BusTransaction&)>)> registerSnoopSink;
};
