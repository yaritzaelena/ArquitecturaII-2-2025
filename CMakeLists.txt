cmake_minimum_required(VERSION 3.24)
project(mp_mesi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------------
# Localizar fuentes con nombres/rutas variables
# -------------------------------
# MESICache.cpp
set(MESI_CACHE_SRC "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/memory/cache/mesi/MESICache.cpp)
    set(MESI_CACHE_SRC src/memory/cache/mesi/MESICache.cpp)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/src/memory/MESICache.cpp)
    set(MESI_CACHE_SRC src/memory/MESICache.cpp)
else()
    message(FATAL_ERROR "No encuentro MESICache.cpp (esperado en src/memory/cache/mesi/ o src/memory/).")
endif()

# Interconnect (Mes vs Mesi)
set(INTERCONNECT_SRC "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/MesInterconnect.cpp)
    set(INTERCONNECT_SRC src/MesInterconnect.cpp)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/src/MesiInterconnect.cpp)
    set(INTERCONNECT_SRC src/MesiInterconnect.cpp)
else()
    message(FATAL_ERROR "No encuentro Mes(i)Interconnect.cpp (esperado en src/).")
endif()

# SharedMemory (opcional – si no existe, seguimos sin él)
set(SHARED_MEM_SRC "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/memory/SharedMemory.cpp)
    set(SHARED_MEM_SRC src/memory/SharedMemory.cpp)
endif()

# -------------------------------
# Librería núcleo MESI
# -------------------------------
add_library(mesi_core
        ${MESI_CACHE_SRC}
        ${INTERCONNECT_SRC}
        ${SHARED_MEM_SRC}
)

target_include_directories(mesi_core PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/PE
)

# Warnings y logs
if(MSVC)
    target_compile_options(mesi_core PRIVATE /W4)
else()
    target_compile_options(mesi_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_definitions(mesi_core PRIVATE $<$<CONFIG:Debug>:TRACE_MESI=1>)

# Hilos
find_package(Threads REQUIRED)
target_link_libraries(mesi_core PUBLIC Threads::Threads)

# -------------------------------
# Ejecutable dot product (apps/dotprod_mesi_main.cpp)
# -------------------------------
add_executable(dotprod_mesi
        apps/dotprod_mesi_main.cpp
        PE/pe/pe.cpp
)
target_link_libraries(dotprod_mesi PRIVATE mesi_core)
target_include_directories(dotprod_mesi PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/PE
)

# -------------------------------
# Ejecutable unificado (si tienes apps/main.cpp o main.cpp en raíz)
# -------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/apps/main.cpp)
    add_executable(mp_main apps/main.cpp PE/pe/pe.cpp)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/main.cpp)
    add_executable(mp_main main.cpp PE/pe/pe.cpp)
endif()

if(TARGET mp_main)
    target_link_libraries(mp_main PRIVATE mesi_core)
    target_include_directories(mp_main PRIVATE
            ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/PE
    )
endif()

