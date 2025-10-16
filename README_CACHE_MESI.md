# Caché + Protocolo MESI (tu módulo)

Este módulo implementa una caché por PE con coherencia **MESI**, cumpliendo la especificación:
- **2-way set associative**, **16 líneas** totales (8 sets × 2 vías).
- **Línea de 32 bytes** (offset=5).
- Políticas **write-allocate** y **write-back**. :contentReference[oaicite:3]{index=3}
- **Métricas por PE**: misses, invalidations, conteos de R/W y tráfico de bus, y matriz de transiciones MESI. :contentReference[oaicite:4]{index=4} :contentReference[oaicite:5]{index=5}

## Archivos clave
- `MesiTypes.hpp`: enums, estructuras de línea, set y métricas.
- `MESICache.[hpp|cpp]`: lógica del controlador MESI (lookup, LRU, install, evict con Flush, load/store, snoop).
- `adapters/InterconnectAdapter.hpp`: interfaz para conectar con el Interconnect real del proyecto.

## Integración rápida
1. **Registrar el caché** de cada PE en el interconnect (o bus) para que:
   - Al emitir `BusRd/BusRdX/BusUpgr`, el bus notifique `onSnoop` a los demás caches.
   - El bus resuelva `Data` (memoria o Flush de otro PE) y llame `onDataResponse(addr, line, shared)`.
2. **shared** en `onDataResponse`:
   - `true` si hubo otro poseedor (S/E/M) ⇒ instala `S`.
   - `false` si exclusivo ⇒ instala `E`.
3. **Evicción**:
   - Si la víctima está `M`, se emite `Flush` (write-back) antes de sobrescribir.

## Compilar
```bash
cmake -S . -B build
cmake --build build -j
./build/test_mesi_basic
