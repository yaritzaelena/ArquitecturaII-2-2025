cmake_minimum_required(VERSION 3.24)
project(mp_mesi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------
# Librería núcleo MESI (cache + interconnect)
# ---------------------------------------
add_library(mesi_core
    src/memory/cache/mesi/MESICache.cpp
    src/memory/cache/mesi/MesiInterconnect.cpp
)
target_include_directories(mesi_core PUBLIC
    ${CMAKE_SOURCE_DIR}      
    ${CMAKE_SOURCE_DIR}/src
)
# Warnings
if(MSVC)
    target_compile_options(mesi_core PRIVATE /W4)
else()
    target_compile_options(mesi_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Logs solo en Debug
target_compile_definitions(mesi_core PRIVATE $<$<CONFIG:Debug>:TRACE_MESI=1>)

# pthread para Linux (threads en el main)
find_package(Threads REQUIRED)
target_link_libraries(mesi_core PUBLIC Threads::Threads)

# ---------------------------------------
# Ejecutable de integración (4 PEs + MESI)
# ---------------------------------------
add_executable(dotprod_mesi
    apps/dotprod_mesi_main.cpp
    PE/pe/pe.cpp                 # el PE
)
target_link_libraries(dotprod_mesi PRIVATE mesi_core)
target_include_directories(dotprod_mesi PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# ---------------------------------------
# Tests (si no requieren interconnect, pueden quedar igual)
# ---------------------------------------
add_executable(test_mesi_basic
    tests/cache/mesi/test_mesi_basic.cpp
)
target_link_libraries(test_mesi_basic PRIVATE mesi_core)

set(TEST_M2S ${CMAKE_SOURCE_DIR}/tests/cache/mesi/test_mesi_m_to_s.cpp)
if(EXISTS ${TEST_M2S})
    add_executable(test_mesi_m_to_s
        tests/cache/mesi/test_mesi_m_to_s.cpp
    )
    target_link_libraries(test_mesi_m_to_s PRIVATE mesi_core)
endif()
