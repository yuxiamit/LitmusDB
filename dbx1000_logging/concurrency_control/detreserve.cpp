#include "detreserve.h"

uint32_t * detreserve_table;

uint32_t lock_table_mask = 0xFFFFFF;

uint32_t min(uint32_t a, uint32_t b) {
	return a>b?b:a;
}

