# Caché + Protocolo MESI

Este módulo implementa una caché por PE con coherencia **MESI**, cumpliendo la especificación:
- **2-way set associative**, **16 líneas** totales (8 sets × 2 vías).
- **Línea de 32 bytes** (offset = 5).
- Políticas **write-allocate** y **write-back**.
- **Métricas por PE**: loads, stores, RW_Accesses, cache_misses, invalidations, tráfico de bus (BusRd/BusRdX/BusUpgr/Flush) y transiciones MESI agregadas.

## Archivos clave
- `src/memory/cache/mesi/MESICache.[hpp|cpp]`: controlador L1$ (lookup, LRU, install, evict con Flush, load/store, snoop).
- `src/memory/cache/mesi/MesiTypes.hpp`: enums/structs (línea, set, métricas).
- `src/memory/cache/mesi/MesiDebug.hpp`: macros de traza (`TRACE_MESI` en Debug).
- `src/MesInterconnect.[hpp|cpp]`: interconect que difunde snoops y entrega datos al emisor.
- `src/memory/SharedMemory.[h|cpp]`: memoria compartida (si se usa en la integración).
- `PE/pe/pe.[hpp|cpp]`: mini-ISA del PE (LOAD/STORE/FMUL/FADD/INC/DEC/JNZ/LEA).
- `main.cpp`: ejecutable unificado (modos **demo** y **dot product**).
- `apps/dotprod_mesi_main.cpp` (opcional): ejecutable “solo dot product”.
- `metrics.py` / `metrics_no_pandas.py`: generación de gráficas desde `cache_stats.csv`.

## Integración rápida
1. **Registrar cada L1$** en el interconect:
   - El L1$ emite `BusRd` / `BusRdX` / `BusUpgr`.
   - El interconect llama `onSnoop(...)` en los demás L1$ y resuelve datos (memoria o `Flush` de un peer).
   - Al entregar datos, invoca `onDataResponse(addr, line, shared)`.
2. **Instalación E/S en `onDataResponse`**:
   - `shared = true` ⇒ instala en **S**.
   - `shared = false` ⇒ instala en **E**.
3. **Evicción**:
   - Si la víctima está en **M**, se emite **`Flush`** (write-back) antes de sobrescribir.

## Compilar (Windows CMD)
```CMD
# Crear build
rmdir /s /q build
mkdir build
cd build
cmake ..
cmake --build . --config Debug

# (opcional) también puedes compilar el ejecutable “solo dot product”
cmake --build . --target dotprod_mesi -j 8

# Demo (stepping + CSV con métricas)
.\cmake-build-debug\mp_main.exe --mode=demo

# Sin pausas:
.\cmake-build-debug\mp_main.exe --mode=demo --nostep

# Problema final: producto punto en doble precisión (N=248)
.\cmake-build-debug\mp_main.exe --mode=dot --N=248
# salida esperada: PASS dotprod with MESI

# (opcional) entorno virtual
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install pandas matplotlib

# generar figuras
python .\metrics.py
