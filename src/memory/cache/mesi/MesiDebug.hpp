#pragma once
#include <cstdio>

#ifndef TRACE_MESI
#define TRACE_MESI 1   // pon 0 para silenciar sin tocar CMake ni el código
#endif

#if TRACE_MESI
  #define LOG(...) std::printf(__VA_ARGS__)
#else
  #define LOG(...)
#endif
