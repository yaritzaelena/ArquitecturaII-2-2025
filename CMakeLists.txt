cmake_minimum_required(VERSION 3.24)
project(mp_mesi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Librería del caché MESI (solo .cpp del caché) ---
add_library(mesi_cache
        src/memory/cache/mesi/MESICache.cpp
)
# Incluye el raíz para poder incluir "src/..." desde quien linkee esta lib
target_include_directories(mesi_cache PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

# Logs solo en Debug
target_compile_definitions(mesi_cache PRIVATE $<$<CONFIG:Debug>:TRACE_MESI=1>)

# Warnings
if(MSVC)
    target_compile_options(mesi_cache PRIVATE /W4)
else()
    target_compile_options(mesi_cache PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Ejecutable de integración: dot product con 4 PEs + MESI ---
add_executable(dotprod_mesi
        apps/dotprod_mesi_main.cpp
        PE/pe/pe.cpp                 # necesitas compilar el PE
)
target_link_libraries(dotprod_mesi PRIVATE mesi_cache)
# Para que funcionen #include "memory/..." y #include "PE/..."
target_include_directories(dotprod_mesi PRIVATE
        ${CMAKE_SOURCE_DIR}       # habilita "PE/..." y "src/..."
        ${CMAKE_SOURCE_DIR}/src   # habilita "memory/..." si lo usas así
)

# --- Tests ---
add_executable(test_mesi_basic
        tests/cache/mesi/test_mesi_basic.cpp
)
target_link_libraries(test_mesi_basic PRIVATE mesi_cache)

set(TEST_M2S ${CMAKE_SOURCE_DIR}/tests/cache/mesi/test_mesi_m_to_s.cpp)
if(EXISTS ${TEST_M2S})
    add_executable(test_mesi_m_to_s
            tests/cache/mesi/test_mesi_m_to_s.cpp
    )
    target_link_libraries(test_mesi_m_to_s PRIVATE mesi_cache)
endif()
