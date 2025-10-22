cmake_minimum_required(VERSION 3.24)
project(mp_mesi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------
# Librería núcleo MESI (cache + interconnect + shared memory)
# ---------------------------------------
add_library(mesi_core
    src/memory/cache/mesi/MESICache.cpp
    src/MesiInterconnect.cpp
    src/memory/SharedMemory.cpp         
)

target_include_directories(mesi_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/PE
)

# Warnings
if(MSVC)
    target_compile_options(mesi_core PRIVATE /W4)
else()
    target_compile_options(mesi_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Logs solo en Debug
target_compile_definitions(mesi_core PRIVATE $<$<CONFIG:Debug>:TRACE_MESI=1>)

# pthread (para hilos)
find_package(Threads REQUIRED)
target_link_libraries(mesi_core PUBLIC Threads::Threads)

# ---------------------------------------
# Ejecutable principal (4 PEs + MESI)
# ---------------------------------------
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
