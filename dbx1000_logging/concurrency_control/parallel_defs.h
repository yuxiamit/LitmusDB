#pragma once
#include "config.h"
#include "global.h"
#include <cstdint>

#if CC_ALG == DETRESERVE
typedef uint32_t intT;
// we use CILKPLUS by default.
#define OPENMP

#ifdef OPENMP
#include <omp.h>
#define parallel_for _Pragma("omp parallel for") for
#define parallel_for_1 _Pragma("omp parallel for schedule (static,1)") for
#define parallel_for_256 _Pragma("omp parallel for schedule (static,256)") for
int getWorkerId();
int getWorkers();
void setWorkers(uint32_t n);
#else
#include <cilk/cilk.h>
#include <cilk/cilk_api.h>
#include <sstream>
#include <iostream>
#include <cstdlib>
#define parallel_for cilk_for
#define parallel_main main
#define parallel_for_1 _Pragma("cilk grainsize = 1") parallel_for
#define parallel_for_256 _Pragma("cilk grainsize = 256") parallel_for
int getWorkerId() {return __cilkrts_get_worker_number();}
static int getWorkers() {
  return __cilkrts_get_nworkers();
}
static void setWorkers(int n) {
  __cilkrts_end_cilk();
  //__cilkrts_init();
  std::stringstream ss; ss << n;
  if (0 != __cilkrts_set_param("nworkers", ss.str().c_str())) {
    std::cerr << "failed to set worker count!" << std::endl;
    std::abort();
  }
}
#endif
#endif