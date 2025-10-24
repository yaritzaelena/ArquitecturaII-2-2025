#pragma once
#include <cstdio>

/*
 * MesiDebug.hpp
 * =============
 * Pequeño helper para logging/depuración condicionado por un macro.
 *
 * ¿Para qué sirve?
 * ----------------
 * - Permite activar o silenciar trazas (printf) de forma centralizada
 *   sin tener que tocar cada llamada a printf en el código.
 * - Se usa, por ejemplo, en la caché MESI para imprimir transiciones
 *   de estados, emisiones al bus, etc., cuando se está depurando.
 *
 * ¿Cómo se activa/desactiva?
 * --------------------------
 * - Si TRACE_MESI == 1  -> LOG(...) imprime usando std::printf.
 * - Si TRACE_MESI == 0  -> LOG(...) no hace nada (se compila a vacío).
 *
 * Dónde definir TRACE_MESI
 * ------------------------
 * 1) En este mismo archivo (por defecto lo dejamos en 1 para desarrollo).
 * 2) O mejor: desde CMake únicamente para la configuración Debug:
 *
 *    target_compile_definitions(mesi_core PRIVATE $<$<CONFIG:Debug>:TRACE_MESI=1>)
 *
 *    y para Release:
 *    target_compile_definitions(mesi_core PRIVATE $<$<CONFIG:Release>:TRACE_MESI=0>)
 *
 *    Con esto no necesitas editar headers; CMake controla si hay trazas.
 *
 * Ejemplo de uso
 * --------------
 *   LOG("[PE%d] BusRd @0x%llx\n", pe_id, (unsigned long long)addr);
 *
 *   // Salida (si TRACE_MESI=1):
 *   // [PE0] BusRd @0x0000000000001234
 *
 * Notas
 * -----
 * - LOG usa std::printf, así que respeta el formato C (no iostreams).
 * - Evita llamadas costosas dentro de LOG(...) si vas a compilar con
 *   TRACE_MESI=0, porque aunque se “eliminen” en compilación, las
 *   expresiones dentro de los argumentos pueden evaluarse antes.
 */

#ifndef TRACE_MESI
  // Valor por defecto: activa trazas. Cambia a 0 si prefieres silencio por defecto.
  #define TRACE_MESI 1
#endif

#if TRACE_MESI
  #define LOG(...) std::printf(__VA_ARGS__)
#else
  #define LOG(...) /* no-op */
#endif
