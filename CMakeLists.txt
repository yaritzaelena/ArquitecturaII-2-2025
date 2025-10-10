cmake_minimum_required(VERSION 3.24)
project(mp_mesi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Librería del caché MESI (solo fuentes .cpp) ---
add_library(mesi_cache
        src/memory/cache/mesi/MESICache.cpp
)
target_include_directories(mesi_cache PUBLIC
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

# --- Tests ---
add_executable(test_mesi_basic
        tests/cache/mesi/test_mesi_basic.cpp
)
target_link_libraries(test_mesi_basic PRIVATE mesi_cache)

# (Opcional) segundo test, solo si el archivo existe
set(TEST_M2S ${CMAKE_SOURCE_DIR}/tests/cache/mesi/test_mesi_m_to_s.cpp)
if(EXISTS ${TEST_M2S})
    add_executable(test_mesi_m_to_s
            tests/cache/mesi/test_mesi_m_to_s.cpp
    )
    target_link_libraries(test_mesi_m_to_s PRIVATE mesi_cache)
endif()
