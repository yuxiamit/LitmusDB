#pragma once
#include "config.h"
#include "row.h"
#include "parallel_defs.h"

#if CC_ALG == DETRESERVE

#include "sequence.h"

#define TXN_START(priority, id) \
	thread_man->_priority = priority; \
	thread_man->_id = id; \
	thread_man->readonly = true;

#define TXN_END \
	thread_man->txnEnd();

extern uint32_t * detreserve_table;
extern uint32_t lock_table_mask;
inline uint32_t mhash(uint64_t key) {
	return (key / sizeof(row_t)) & lock_table_mask;
}

uint32_t min(uint32_t a, uint32_t b);

#endif