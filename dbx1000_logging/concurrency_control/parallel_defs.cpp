#if CC_ALG == DETRESERVE
#include "parallel_defs.h"
#include <omp.h>
int getWorkerId() {return omp_get_thread_num();}
int getWorkers() { return omp_get_max_threads(); }
void setWorkers(uint32_t n) { omp_set_num_threads(n); }

#endif