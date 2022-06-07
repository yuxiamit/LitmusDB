#include "txn.h"
#include "row.h"
#include "wl.h"
#include "ycsb.h"
#include "thread.h"
#include "mem_alloc.h"
#include "occ.h"
#include "table.h"
#include "catalog.h"
#include "index_btree.h"
#include "index_hash.h"
#include "log.h"
#include "serial_log.h"
#include "parallel_log.h"
#include "taurus_log.h"
#include "log_recover_table.h"
#include "log_pending_table.h"
#include "free_queue.h"
#include "manager.h"
#include <fcntl.h>
#include "locktable.h"
#include "logging_thread.h"
#include "litmus-prover.h"
#include <inttypes.h>
#include <sstream>
#include "detreserve.h"

#if LOG_ALGORITHM == LOG_BATCH
pthread_mutex_t * txn_man::_log_lock;
#endif

void txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
	this->h_thd = h_thd;
	this->h_wl = h_wl;
	pthread_mutex_init(&txn_lock, NULL);
	lock_ready = false;
	ready_part = 0;
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	accesses = (Access **) _mm_malloc(sizeof(Access *) * MAX_ROW_PER_TXN, ALIGN_SIZE);
	write_set = (uint32_t *) _mm_malloc(sizeof(uint32_t) * MAX_ROW_PER_TXN, ALIGN_SIZE);
	for (int i = 0; i < MAX_ROW_PER_TXN; i++)
		accesses[i] = NULL;
	num_accesses_alloc = 0;
#if CC_ALG == TICTOC || CC_ALG == SILO
	_pre_abort = g_pre_abort; 
	_validation_no_wait = true;
#endif
#if CC_ALG == TICTOC
	_max_wts = 0;
	_min_cts = 0;
	_write_copy_ptr = false; //(g_write_copy_form == "ptr");
	_atomic_timestamp = g_atomic_timestamp;
#elif CC_ALG == SILO || LOG_ALGORITHM == LOG_SERIAL
	_cur_tid = 0;
#endif
	_last_epoch_time = 0;	
	_log_entry_size = 0;

#if LOG_ALGORITHM == LOG_PARALLEL
	_num_raw_preds = 0;
	_num_waw_preds = 0;
//	_predecessor_info = new PredecessorInfo;	
//	for (uint32_t i = 0; i < 4; i++)
//		aggregate_pred_vector[i] = 0;
#elif LOG_ALGORITHM == LOG_TAURUS
	thread_local_counter = 0; // local counter
	if(g_num_logger < 4)
	{
		// for SIMD
		lsn_vector = (lsnType*) _mm_malloc(sizeof(lsnType) * 4, ALIGN_SIZE);
		memset(lsn_vector, 0, sizeof(lsnType) * 4); // initialize to 0
	}
	else
	{
		lsn_vector = (lsnType*) _mm_malloc(sizeof(lsnType) * g_num_logger, ALIGN_SIZE);
		memset(lsn_vector, 0, sizeof(lsnType) * g_num_logger); // initialize to 0
	}	
#endif
	_log_entry = new char [g_max_log_entry_size];
	_log_entry_size = 0;
	_txn_state_queue = new queue<TxnState> * [g_thread_cnt]; // here, why g_thread_cnt? I think txn_man is a thread-private variable.
	for (uint32_t i = 0; i < g_thread_cnt; i++) {
		_txn_state_queue[i] = (queue<TxnState> *) _mm_malloc(sizeof(queue<TxnState>), ALIGN_SIZE);
		new (_txn_state_queue[i]) queue<TxnState>();
	}
}

void txn_man::set_txn_id(txnid_t txn_id) {
	this->txn_id = txn_id;
}

txnid_t txn_man::get_txn_id() {
	return this->txn_id;
}

workload * txn_man::get_wl() {
	return h_wl;
}

uint64_t txn_man::get_thd_id() {
	return h_thd->get_thd_id();
}

void txn_man::set_ts(ts_t timestamp) {
	this->timestamp = timestamp;
}

ts_t txn_man::get_ts() {
	return this->timestamp;
}

RC txn_man::cleanup(RC in_rc) 
{
	RC rc = in_rc;

	// Perform Verification
#if VERIFICATION

    // log the access data of this transaction.

	//prove(this);

#endif


#if LOG_ALGORITHM == LOG_TAURUS && CC_ALG != SILO // already logged in silo_validate
	// Start logging
	// uint64_t & max_lsn = _max_lsn;
	if(wr_cnt>0 && rc!=Abort)
	{
		uint64_t current_time = get_sys_clock();
		create_log_entry();
		uint64_t current_time2 = get_sys_clock();
		log_manager->serialLogTxn(_log_entry, _log_entry_size, lsn_vector);  // add to buffer

		INC_INT_STATS(time_log_serialLogTxn, get_sys_clock() - current_time2);
		INC_INT_STATS(time_log_create, current_time2 - current_time);
	}
#elif LOG_ALGORITHM == LOG_SERIAL && CC_ALG != SILO // Silo updates _cur_tid inside the cc algorithm
	if(wr_cnt > 0 && rc == RCOK)
	{
		uint64_t current_time = get_sys_clock();
		create_log_entry();
		uint64_t current_time2 = get_sys_clock();
		_cur_tid = log_manager->serialLogTxn(_log_entry, _log_entry_size);
		INC_INT_STATS(time_log_serialLogTxn, get_sys_clock() - current_time2);
		INC_INT_STATS(time_log_create, current_time2 - current_time);
	}
#endif
	uint64_t starttime = get_sys_clock();
	// start to release the locks
#if CC_ALG != SILO && CC_ALG != DETRESERVE // updating the data is already handled in silo_validate
	for (int rid = row_cnt - 1; rid >= 0; rid --) {
		row_t * orig_r = accesses[rid]->orig_row;
		access_t type = accesses[rid]->type;
		if (type == WR && rc == Abort)
			type = XP;  // means we need to roll back the data value

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
		if (type == RD) {
			accesses[rid]->data = NULL;
			continue;
		}
#endif

		char *newdata;
		if (ROLL_BACK && type == XP &&
					(CC_ALG == DL_DETECT || 
					CC_ALG == NO_WAIT || 
					CC_ALG == WAIT_DIE)) 
		{
			newdata = ((row_t*)accesses[rid]->orig_data)->data;  // fixed a bug left from the original code base
		} else {
			newdata = accesses[rid]->data;
		}
#if USE_LOCKTABLE 
		LockTable & lt = LockTable::getInstance();
		uint64_t current_time = get_sys_clock();
		//assert((uint64_t)newdata != 0);
		#if LOG_ALGORITHM == LOG_TAURUS
		lt.release_lock(orig_r, type, this, newdata, lsn_vector, NULL, rc);
		#elif LOG_ALGORITHM == LOG_SERIAL
		lt.release_lock(orig_r, type, this, newdata, NULL, &_max_lsn, rc);
		#else
		lt.release_lock(orig_r, type, this, newdata, NULL, NULL, rc);
		#endif
		INC_INT_STATS(time_locktable_release, get_sys_clock() - current_time);
#else
		orig_r->return_row(type, this, newdata);
#endif
#if CC_ALG != TICTOC && CC_ALG != SILO
		accesses[rid]->data = NULL;  // will not need this any more
#endif
	}
#endif
	uint64_t cleanup_1_begin = get_sys_clock();
	INC_INT_STATS(time_phase1_1, cleanup_1_begin - starttime);
	if (rc == Abort) { // remove inserted rows.
		for (UInt32 i = 0; i < insert_cnt; i ++) {
			row_t * row = insert_rows[i];
			assert(g_part_alloc == false);
#if CC_ALG != HSTORE && CC_ALG != OCC
			mem_allocator.free(row->manager, 0);
#endif
			row->free_row();
			mem_allocator.free(row, sizeof(row));
		}
	}
	uint64_t cleanup_1_end = get_sys_clock();
	INC_INT_STATS(time_cleanup_1, cleanup_1_end - cleanup_1_begin);
	// Logging
	//printf("log??\n");
#if LOG_ALGORITHM != LOG_NO
	if (rc == RCOK && wr_cnt > 0)
	{
//		if (wr_cnt > 0) {
	    {
			uint64_t before_log_time = get_sys_clock();
			//uint32_t size = _log_entry_size;
			assert(_log_entry_size != 0);
  #if LOG_ALGORITHM == LOG_TAURUS
			// for waiting for txn to flush and not spinning around
			queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
			TxnState state;
			state.lsn_vec = (uint64_t*) _mm_malloc( sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
			memcpy(state.lsn_vec, lsn_vector, sizeof(uint64_t) * g_num_logger);
			//state.destination = thread_local_counter++ % g_thread_cnt;
			state.start_time = _txn_start_time;
			state.wait_start_time = get_sys_clock();
			state_queue->push(state);
			//log_manager[state.destination]->logTxn
			
  #elif LOG_ALGORITHM == LOG_SERIAL
			//	_max_lsn: max LSN for predecessors
			//  _cur_tid: LSN for the log record of the current txn 
  			uint64_t max_lsn = max(_max_lsn, _cur_tid); // for serial logging, we only need a single one
			/*if (max_lsn <= log_manager->get_persistent_lsn()) {  // does not matter
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {*/
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				state.max_lsn = max_lsn;
				state.start_time = _txn_start_time;
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			//}
  #elif LOG_ALGORITHM == LOG_PARALLEL
			bool success = true;
			// check own log record  
			uint32_t logger_id = _cur_tid >> 48;
			uint64_t lsn = (_cur_tid << 16) >> 16;
			if (lsn > log_manager[logger_id]->get_persistent_lsn())
				success = false;
			if (success) {
				for (uint32_t i=0; i < _num_raw_preds; i++)  {
					if (_raw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _raw_preds_tid[i] >> 48;
					lsn = (_raw_preds_tid[i] << 16) >> 16;
					if (lsn > log_manager[logger_id]->get_persistent_lsn()) { 
						success = false;
						break;
					}
				} 
			}
			if (success) {
				for (uint32_t i=0; i < _num_waw_preds; i++)  {
					if (_waw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _waw_preds_tid[i] >> 48;
					lsn = (_waw_preds_tid[i] << 16) >> 16;
					if (lsn > log_manager[logger_id]->get_persistent_lsn()) { 
						success = false;
						break;
					}
				} 		
			}
			if (success) { 
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				for (uint32_t i = 0; i < g_num_logger; i ++)
					state.preds[i] = 0;
				// calculate the compressed preds
				uint32_t logger_id = _cur_tid >> 48;
				uint64_t lsn = (_cur_tid << 16) >> 16;
				if (lsn > state.preds[logger_id])
					state.preds[logger_id] = lsn;
				for (uint32_t i=0; i < _num_raw_preds; i++)  {
					if (_raw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _raw_preds_tid[i] >> 48;
					lsn = (_raw_preds_tid[i] << 16) >> 16;
					if (lsn > state.preds[logger_id])
						state.preds[logger_id] = lsn;
				} 
				for (uint32_t i=0; i < _num_waw_preds; i++)  {
					if (_waw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _waw_preds_tid[i] >> 48;
					lsn = (_waw_preds_tid[i] << 16) >> 16;
					if (lsn > state.preds[logger_id])
						state.preds[logger_id] = lsn;
				} 
				state.start_time = _txn_start_time;
				//memcpy(state.preds, _preds, sizeof(uint64_t) * g_num_logger);
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			}
  #elif LOG_ALGORITHM == LOG_BATCH
  			uint64_t flushed_epoch = (uint64_t)-1;

			for (uint32_t i = 0; i < g_num_logger; i ++) {
				uint64_t max_epoch = glob_manager->get_persistent_epoch(i);
				if (max_epoch < flushed_epoch)
					flushed_epoch = max_epoch; 
			}
			//printf("flushed_epoch= %ld\n", flushed_epoch);
			if (_epoch <= flushed_epoch) {
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				state.epoch = _epoch;
				state.start_time = _txn_start_time;
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			}
  #endif
			uint64_t after_log_time = get_sys_clock();
			INC_INT_STATS(time_log, after_log_time - before_log_time);
		}	
	}
	uint64_t cleanup2_begin = get_sys_clock();
	INC_INT_STATS(time_phase1_2, cleanup2_begin - cleanup_1_end);
	try_commit_txn();  // no need to try_commit_txn if abort
#else // LOG_ALGORITHM == LOG_NO
	uint64_t cleanup2_begin = get_sys_clock();
	INC_INT_STATS(num_latency_count, 1);
	INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
#endif
	_log_entry_size = 0;
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
#if LOG_ALGORITHM == LOG_PARALLEL
	_num_raw_preds = 0;
	_num_waw_preds = 0;
#elif LOG_ALGORITHM == LOG_SERIAL
	_max_lsn = 0;
#elif LOG_ALGORITHM == LOG_TAURUS
	_max_lsn = 0;
	memset(lsn_vector, 0, sizeof(uint64_t) * g_num_logger);
#endif
#if CC_ALG == DL_DETECT
	dl_detector.clear_dep(get_txn_id());
#endif
	INC_INT_STATS(time_cleanup_2, get_sys_clock() - cleanup2_begin);
	//printf("Txn cleaned\n");
	return rc;
}

void 			
txn_man::try_commit_txn()
{
	uint64_t starttime = get_sys_clock();
#if LOG_ALGORITHM == LOG_SERIAL
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		if (state.max_lsn > log_manager->get_persistent_lsn()) { 
			success = false;
			break;
		}
		if (success) {
			uint64_t lat = get_sys_clock() - state.start_time;
			INC_FLOAT_STATS(latency, lat);
			INC_INT_STATS(num_latency_count, 1);
			state_queue->pop();
		}
	}
#elif LOG_ALGORITHM == LOG_TAURUS
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	//if(state_queue->size() > 1000)
	//	printf("[%" PRIu64 "] tries to commit txn, now queue length: %" PRIu64 "\n", GET_THD_ID, state_queue->size());
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		// TODO: we can have N fronts in parallel so that one does not have to wait
		/*
		if (state.max_lsn > log_manager->_logger[state.destination]->get_persistent_lsn()) { 
			success = false;
			break;
		}*/
		
		for(uint32_t i=0; i<g_num_logger; i++)
		{
			if(state.lsn_vec[i] > log_manager->_logger[i]->get_persistent_lsn())
			{
				success = false;
				break;
			}
		}
		if (success) {
			uint64_t lat = get_sys_clock() - state.start_time;
			INC_FLOAT_STATS(latency, lat);
			INC_INT_STATS(num_latency_count, 1);
			_mm_free(state.lsn_vec);
			state_queue->pop();
		}
	}
#elif LOG_ALGORITHM == LOG_PARALLEL
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		for (uint32_t i=0; i < g_num_logger; i++)  {
			if (state.preds[i] > log_manager[i]->get_persistent_lsn()) { 
				success = false;
				break;
			}
		}
		if (success) {
			INC_INT_STATS(num_latency_count, 1);
			INC_FLOAT_STATS(latency, get_sys_clock() - state.start_time);
			state_queue->pop();
		}
	}
#elif LOG_ALGORITHM == LOG_BATCH
  	uint64_t flushed_epoch = (uint64_t)-1;
	for (uint32_t i = 0; i < g_num_logger; i ++) {
		uint64_t max_epoch = glob_manager->get_persistent_epoch(i);
		if (max_epoch < flushed_epoch)
			flushed_epoch = max_epoch; 
	}
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		if (state.epoch > flushed_epoch) { 
			success = false;
			break;
		}
		if (success) {
			INC_INT_STATS(num_latency_count, 1);
			INC_FLOAT_STATS(latency, get_sys_clock() - state.start_time);
			state_queue->pop();
		}
	}
#endif
	INC_INT_STATS(time_debug5, get_sys_clock() - starttime);
}


RC txn_man::get_row(row_t * row, access_t type, char * &data) { //TODO: change this function so that it aquires the Locktable
	// NOTE. 
	// For recovery, no need to go through concurrncy control
	if (g_log_recover) {
		data = row->get_data(this, type);
		return RCOK;
	}

	if (CC_ALG == HSTORE) {
		data = row->get_data();
		return RCOK;
	}
	uint64_t starttime = get_sys_clock();
	RC rc = RCOK;
	if (accesses[row_cnt] == NULL) {
		Access * access = (Access *) _mm_malloc(sizeof(Access), ALIGN_SIZE);
		accesses[row_cnt] = access;
#if (CC_ALG == SILO || CC_ALG == TICTOC)
		access->data = new char [MAX_TUPLE_SIZE];
//#if LOG_ALGORITHM == LOG_TAURUS
//		access->orig_data = (char*) row;
//#else
		access->orig_data = NULL;
//#endif
#elif (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
		access->orig_data = (char*)_mm_malloc(sizeof(row_t), ALIGN_SIZE);
		((row_t *)(access->orig_data))->init(MAX_TUPLE_SIZE);
#elif CC_ALG == DETRESERVE
		access->data = new char [MAX_TUPLE_SIZE];
		access->hashed_key = mhash((uint64_t)row);
#endif
		num_accesses_alloc ++;
	}
	uint64_t right_before_get = get_sys_clock();
	INC_INT_STATS(time_get_row_before, right_before_get - starttime);
	//if(row_cnt == 19)
	//	printf("Encounter 19, before get_row as %" PRIu64 ", type %d\n", (uint64_t)row, type);
#if USE_LOCKTABLE && CC_ALG != SILO && CC_ALG != DETRESERVE
	LockTable & lt = LockTable::getInstance();
	
	#if LOG_ALGORITHM == LOG_TAURUS
	rc = lt.get_row(row, type, this, accesses[ row_cnt ]->data, lsn_vector, NULL);
	#elif LOG_ALGORITHM == LOG_SERIAL
	rc = lt.get_row(row, type, this, accesses[ row_cnt ]->data, NULL, &_max_lsn);
	#else
	rc = lt.get_row(row, type, this, accesses[ row_cnt ]->data, NULL, NULL);
	#endif
	
#else
	rc = row->get_row(type, this, accesses[ row_cnt ]->data);
#endif
	starttime = get_sys_clock();
	INC_INT_STATS(time_locktable_get, starttime - right_before_get);
	
	if (rc == Abort) {
		return Abort;
	}
	accesses[row_cnt]->type = type;
	accesses[row_cnt]->orig_row = row;
	//if(row_cnt == 19)
	//	printf("Encounter 19, saved row as %" PRIu64 "\n", (uint64_t)row);
#if CC_ALG == TICTOC
	accesses[row_cnt]->wts = last_wts;
	accesses[row_cnt]->rts = last_rts;
#elif CC_ALG == SILO
	accesses[row_cnt]->tid = last_tid;
#elif CC_ALG == HEKATON
	accesses[row_cnt]->history_entry = history_entry;
#endif

#if ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
	// orig_data should be char *
	// assert(false);
	if (type == WR) {
		((row_t *)(accesses[row_cnt]->orig_data))->table = row->get_table();
		((row_t *)(accesses[row_cnt]->orig_data))->copy(row);
	}
#endif

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
	if (type == RD)
		row->return_row(type, this, accesses[ row_cnt ]->data);
#endif
	
	
	if (type == WR)
	{
		write_set[wr_cnt] = row_cnt;
		wr_cnt ++;
		#if CC_ALG == DETRESERVE

		bool r = false;
		uint32_t pri;
		uint32_t * en;
		en = &detreserve_table[mhash((uint64_t)row)];
		pri = *en;
		do {PAUSE; pri = *en; }
		while (_priority < pri && !(r=__sync_bool_compare_and_swap(en, pri, _priority)));
		// if(_priority > *en) return Abort; // early abort
		#endif
	}
	//uint64_t timespan = get_sys_clock() - starttime;
	INC_INT_STATS(time_get_row_after, get_sys_clock() - starttime);
	//INC_INT_STATS(time_man, timespan);
	data = accesses[row_cnt]->data;
	// shadow write by default.
	row_cnt ++;  // moved 
	return RCOK;
}

void txn_man::insert_row(row_t * row, table_t * table) {
	if (CC_ALG == HSTORE)
		return;
	assert(insert_cnt < MAX_ROW_PER_TXN);
	insert_rows[insert_cnt ++] = row;
}

itemid_t *
txn_man::index_read(INDEX * index, idx_key_t key, int part_id) {
itemid_t * item;
#if CC_ALG == DETRESERVE
	// we are inside openmp
	index->index_read(key, item, part_id, 0);
#else
	index->index_read(key, item, part_id, get_thd_id());
#endif
	return item;
}

void 
txn_man::index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item) {
	uint64_t starttime = get_sys_clock();
	index->index_read(key, item, part_id, get_thd_id());
	INC_INT_STATS(time_index, get_sys_clock() - starttime);
}

RC txn_man::finish(RC rc) {
	/*
	if(rc == RCOK)
		printf("[%" PRIu64 "] txn committed\n", GET_THD_ID);
	else
		printf("[%" PRIu64 "] txn aborted\n", GET_THD_ID);
	*/
	assert(!g_log_recover);
#if CC_ALG == HSTORE
	return RCOK;
#endif
	uint64_t starttime = get_sys_clock();
#if CC_ALG == OCC
	if (rc == RCOK)
		rc = occ_man.validate(this);
	else 
		cleanup(rc);
#elif CC_ALG == TICTOC
	if (rc == RCOK) {
		rc = validate_tictoc();
	} else 
		rc = cleanup(rc);
#elif CC_ALG == SILO
	if (rc == RCOK)
	#if LOG_ALGORITHM == LOG_SERIAL
		rc = validate_silo_serial();
	#else
		rc = validate_silo();
	#endif
	else 
		cleanup(rc);
#elif CC_ALG == HEKATON
	rc = validate_hekaton(rc);
	cleanup(rc);
#elif CC_ALG == DETRESERVE
    // cleanup(rc); // postpone cleanup to later.
#else // lock-based
	cleanup(rc);
#endif
	uint64_t timespan = get_sys_clock() - starttime;
	//INC_INT_STATS(time_man, timespan);
	INC_INT_STATS(time_cleanup,  timespan);
	return rc;
}

void
txn_man::release() {
	for (int i = 0; i < num_accesses_alloc; i++)
	{
#if ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
		_mm_free(((row_t*)accesses[i]->orig_data)->data);
		_mm_free(accesses[i]->orig_data);
#endif // otherwise orig_data is NULL
		mem_allocator.free(accesses[i], 0);
	}
	mem_allocator.free(accesses, 0);
}

// Recovery for data logging
void 
txn_man::recover() {
#if LOG_ALGORITHM == LOG_SERIAL
	serial_recover();
#elif LOG_ALGORITHM == LOG_PARALLEL
	parallel_recover();
#elif LOG_ALGORITHM == LOG_BATCH
	batch_recover();
#elif LOG_ALGORITHM == LOG_TAURUS
	taurus_recover();
#endif
}

#if LOG_ALGORITHM == LOG_TAURUS

/*
// # returns if tlv is strictly large than rlv in any dimension
bool lvLarger(uint64_t *tlv, uint64_t *rlv)
{
	for(uint i=0; i<g_num_logger; i++)
		if(tlv[i] > rlv[i])
			return true;
	return false;
}
*/
void printPI(LoggingThread::poolItem & pi)
{
	cout << "PI {";
	for(uint i=0; i<g_num_logger; i++)
	{
		cout << pi.txnLV[i] << " ";
	}
	cout << ", " << *pi.LSN << "}" << endl;
}
#if RECOVER_TAURUS_LOCKFREE
void printPool(uint64_t index) {
	uint32_t logger = GET_THD_ID % g_num_logger;
	LoggingThread *lt = logging_thds[logger];
	//list<LoggingThread::poolItem> *pool = lt->pool;
	//LoggingThread::poolItem * it = lt->pool;
	//poolItem * tail = lt->tail;
	//uint64_t counter = 0;
	//cout << "pool size " << pool->size() << endl;
	cout << "pool size " << lt->poolsize << endl;
	//for(auto it = pool->begin(); it != pool->end(); it++)
	/*
	while((it=it->next)!=NULL)
	{
		if(counter == index)
		{
			printPI(*it);
			break;
		}
		counter ++;
	}
	*/
	printPI(lt->pool[index]);
}
#endif

void txn_man::taurus_recover() {
	stringstream sstream;
#if PER_WORKER_RECOVERY
	uint64_t recover_full_start = get_sys_clock();
	uint32_t loggerId = GET_THD_ID % g_num_logger;
	uint32_t workerId = GET_THD_ID / g_num_logger;
	uint32_t num_worker = g_thread_cnt / g_num_logger;
	uint64_t latestLSN = 0;
	#if COMPRESS_LSN_LOG
			uint64_t * LVFence = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
			memset(LVFence, 0, sizeof(uint64_t) * g_num_logger);
	#endif
	//cout << workerId << " " << logger << endl;
	// TODO: might not accurate
	//LoggingThread *lt = logging_thds[logger];
	//uint32_t realLogId = logging_thds[logger]->_thd_id % g_num_logger;
	
	//LoggingThread::poolItem* pool = lt->SPSCPools[workerId];
	LoggingThread::poolItem* pool = (LoggingThread::poolItem*) _mm_malloc(sizeof(LoggingThread::poolItem) * g_poolsize_wait + ALIGN_SIZE, ALIGN_SIZE);
	for(uint32_t j=0; j<g_poolsize_wait; j++)
	{
		pool[j].txnData = (char*) _mm_malloc(g_max_log_entry_size, ALIGN_SIZE);
		pool[j].txnLV = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
		pool[j].LSN = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		pool[j].LSN[0] = 0; // this is important.
	}
	LoggingThread::poolItem *it;
	assert(g_thread_cnt % g_num_logger == 0);
	uint64_t poolStart = 0, poolEnd = 0;
	LogManager * logger = log_manager->_logger[loggerId];
	char * default_entry = (char*) _mm_malloc(MAX_TUPLE_SIZE, 64);
	for(;;)
	{
		uint64_t lz1 = get_sys_clock();
		uint32_t lz4;
		char * entry = default_entry;
		uint32_t size;
		if(poolEnd - poolStart < g_poolsize_wait)
		{
			// fill in the pool
			bool eof = logger->iseof();
#if RECOVERY_FULL_THR	
	if(glob_manager->_workload->sim_done>0)
		break; // someone has finished.
#endif
			uint64_t lsn = logger->get_next_log_entry(entry, size);
			uint32_t lz2 = get_sys_clock();
			
			INC_INT_STATS(time_recover1, lz2 - lz1);
			//cout << lsn << " " << (uint64_t)entry << endl;
			if(entry != NULL)
			{
				assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
				it = pool + poolEnd % g_poolsize_wait;
				
				char * & ptdentry = entry;
	#if COMPRESS_LSN_LOG
				// read metainfo
				if(ptdentry[0] == 0x7f)
				{
					// this is a PSN Flush
					memcpy(LVFence, ptdentry + sizeof(uint32_t) * 2, sizeof(uint64_t) * g_num_logger);
					INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger + sizeof(uint32_t) * 2);
					continue; // no recovering for PSN flush
				}
				else
				{
					// use LVFence to update T.LV
					memcpy(it->txnLV, LVFence, sizeof(uint64_t) * g_num_logger);
					uint64_t psnCounter = *(uint64_t*)(ptdentry + size - 1); // sizeof(uint64_t));
					psnCounter &= 0xff; // extract only one byte
					for(uint i=1; i<=psnCounter; i++)
					{
						//uint64_t psnToWrite = *(uint64_t*)(ptdentry + size - sizeof(uint64_t) - sizeof(uint64_t) * i);
						uint64_t psnToWrite = *(uint64_t*)(ptdentry + size - 1 - sizeof(uint64_t) * i);
						it->txnLV[psnToWrite&((1<<5)-1)] = psnToWrite >> 5;
					}
					//INC_INT_STATS(int_aux_bytes, (psnCounter + 1) * sizeof(uint64_t));
					INC_INT_STATS(int_aux_bytes, psnCounter * sizeof(uint64_t) + 1);
				}
	#else
				// read meta_info
				uint64_t *LV_start = (uint64_t*)(ptdentry + size - sizeof(uint64_t) * g_num_logger);
				for(uint i=0; i<g_num_logger; i++)
				{
					it->txnLV[i] = LV_start[i];
				}
				INC_INT_STATS(int_aux_bytes, g_num_logger * sizeof(uint64_t));
	#endif
				INC_INT_STATS(num_log_entries, 1);
				uint32_t lz3 = get_sys_clock();
				INC_INT_STATS(time_recover2, lz3 - lz2);
				uint i=0, j=0;
				// use SIMD and manual unroll
				//int unroll = (g_num_logger/4) * 4;
				for(; i<g_num_logger; i++)
				{
					//uint64_t txnLVi_cache = it->txnLV[i];
					if(it->txnLV[i] < *log_manager->recoverLVSPSC_min[i])
						continue;
					// must be good even if it is stale
					uint64_t recoverLV_i_min = UINT64_MAX;
					for(j=0; j<num_worker; j++)
					// unnecessary to be atomic.
					// It is good to have recoverLVSPSC in a same cacheline!
					{
						uint64_t recoverlvspscIJ = *log_manager->recoverLVSPSC[i][j];
						if(recoverLV_i_min > recoverlvspscIJ)
							recoverLV_i_min = recoverlvspscIJ;
							//break;
					}
					//INC_INT_STATS(int_debug10, 1);
					// unnecessary to atomically update
					*log_manager->recoverLVSPSC_min[i] = recoverLV_i_min;
					//if(j<num_worker)
					if(it->txnLV[i] > recoverLV_i_min)
						break;
				}
				if(i<g_num_logger)
				{
					// not good for now, put into the pool.
					memcpy(it->txnData, entry, size);
					it->recovered = 0;
					poolEnd ++;
				}
				else
				{
					// we can recover it right away
					assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
					recover_txn(entry + sizeof(uint32_t) * 2);
					INC_INT_STATS(num_commits, 1);

					uint32_t size_aligned = size % 64 == 0 ? size : size + 64 - size % 64;
					INC_INT_STATS(int_debug5, size_aligned);
				}
				

				COMPILER_BARRIER
				//logger->rasterizedLSN[workerId][0] = lsn;
#if PER_WORKER_RECOVERY
				logger->reserveLSN[workerId][0] = 0;
#endif

				latestLSN = lsn;
				lz4 = get_sys_clock();
				INC_INT_STATS(time_recover3, lz4 - lz3);
			}
			else
			{
				if(eof && poolStart == poolEnd)
					break; // finished
				else
				{
					PAUSE
					lz4 = get_sys_clock();
					INC_INT_STATS(time_recover4, lz4 - lz2);
				}
			}
		}
		// scan the pool to see if we can recover any
		uint64_t poolIndex = poolStart;
		bool found = false;
		bool checkSuccessive = true; // do not optimize this!

		for(;poolIndex < poolEnd; poolIndex ++)
		{
#if RECOVERY_FULL_THR	
				if(glob_manager->_workload->sim_done>0)
					break; // someone has finished.
#endif
				it = pool + poolIndex % g_poolsize_wait;
				if(it->recovered)
					continue;
				if(checkSuccessive)
				{
					*log_manager->recoverLVSPSC[loggerId][workerId] = it->LSN[0] - 1;
					poolStart = poolIndex;
					checkSuccessive = false;
				}
				uint i=0, j=0;
				// use SIMD and manual unroll
				//int unroll = (g_num_logger/4) * 4;
				for(; i<g_num_logger; i++)
				{
					//uint64_t txnLVi_cache = it->txnLV[i];
					if(it->txnLV[i] < *log_manager->recoverLVSPSC_min[i])
						continue;
						// must be good even if it is stale
					uint64_t recoverLV_i_min = UINT64_MAX;
					for(j=0; j<num_worker; j++)
					// unnecessary to be atomic.
					// It is good to have recoverLVSPSC in a same cacheline!
					{
						uint64_t recoverlvspscIJ = *log_manager->recoverLVSPSC[i][j];
						if(recoverLV_i_min > recoverlvspscIJ)
							recoverLV_i_min = recoverlvspscIJ;
							//break;
					}
					//INC_INT_STATS(int_debug10, 1);
					// unnecessary to atomically update
					*log_manager->recoverLVSPSC_min[i] = recoverLV_i_min;
					//if(j<num_worker)
					if(it->txnLV[i] > recoverLV_i_min)
						break;
				}
				if(i<g_num_logger)
				{
					//it->latch = 0; // release back
					continue;
				}
				found = true;
				break;
		}
		uint64_t lz5 = get_sys_clock();
		INC_INT_STATS(time_recover6, lz5 - lz4);
		if(checkSuccessive) // && poolEnd > poolStart)// means the pool is full of recovered txns
		{
			poolStart = poolIndex; // here poolIndex is the (maybe stale) poolEnd.
			*log_manager->recoverLVSPSC[loggerId][workerId] = latestLSN; //lt->maxLSN[0];
		}
		if(found)
		{
			recover_txn(it->txnData + sizeof(uint32_t) * 2);
			
			INC_INT_STATS(num_commits, 1);

			//uint32_t size_aligned = it->size % 64 == 0 ? it->size : it->size + 64 - it->size % 64;
			//INC_INT_STATS(int_debug5, size_aligned);
			it->recovered = 1; // so logging thread can recycle.
		}
		uint64_t lz6 = get_sys_clock();
		INC_INT_STATS(time_recover7, lz6 - lz5);
#if RECOVERY_FULL_THR	
	if(glob_manager->_workload->sim_done>0)
		break; // someone has finished.
#endif
	}
	_mm_free(default_entry);
#if RECOVERY_FULL_THR	
	glob_manager->_workload->sim_done = 1;
#endif
	INC_INT_STATS(time_recover_full, get_sys_clock() - recover_full_start);
	//ATOM_ADD(lt->workerDone[0], 1);  // notify the logger.
	
	sstream << "Recover finished for worker " << GET_THD_ID << " of logger " << loggerId << endl;
	cout << sstream.str(); // atomic output.
	return;
#else
#if RECOVER_TAURUS_LOCKFREE
	uint32_t logger = GET_THD_ID % g_num_logger;
	LoggingThread *lt = logging_thds[logger];
	//list<LoggingThread::poolItem> *pool = lt->pool;
	LoggingThread::poolItem* pool = lt->pool;
	//volatile uint64_t * rlv_logger = log_manager->recoverLV[logger];
	//assert(*rlv_logger == 0); // rlv logger might not be zero if multiple workers are working.
	assert(g_thread_cnt % g_num_logger == 0);
	uint64_t poolCapacity = g_thread_cnt / g_num_logger;
	uint64_t poolOffset = GET_THD_ID/g_num_logger;
	//uint64_t poolStep = PRIMES[GET_THD_ID/g_num_logger];
	while(!(lt->poolDone && lt->poolempty()))
	// TODO: lt->pool->empty() might cause data racing.
	{
		bool found = false;
		LoggingThread::poolItem *it;
		uint64_t poolIndex;
		uint64_t tt2 = get_sys_clock();
		/*
		for(poolIndex = lt->poolStart + poolOffset; poolIndex < lt->poolEnd; poolIndex += poolStep) // ++poolIndex)
		{*/
		/*
		uint64_t poolIterator;
		for(poolIterator = poolOffset; ; poolIterator += poolStep)
		{
			uint64_t poolS = lt->poolStart, poolE = lt->poolEnd;
			uint64_t poolLength = poolE - poolS;
			if(poolLength == 0) break;
			poolIndex = poolS + poolIterator % poolLength;
			//if(poolIndex >= lt->poolEnd) break;*/
		uint64_t poolS = lt->poolStart;
		poolIndex = poolS - poolS % poolCapacity + poolOffset;
		if(poolS % poolCapacity > poolOffset) poolIndex += poolCapacity;
		for(;poolIndex < lt->poolEnd; poolIndex += poolCapacity)
		{
			uint64_t tt = get_sys_clock();
			// it's ok if poolIndex here is actually behind the poolStart
			// and read some newly pushed item
			it = pool + poolIndex % g_poolsize_wait;
			if(!ATOM_CAS(it->latch, 0, 1))
			{
				INC_INT_STATS(time_recover1, get_sys_clock() - tt);  // the first ATOM_CAS
				continue;
			}
			tt2 = get_sys_clock();
			INC_INT_STATS(time_recover1, tt2 - tt); // ATOM CAS
			uint i=0;
			for(; i<g_num_logger; i++)
				if(it->txnLV[i] > *(log_manager->recoverLV[i]))
					break;
			if(i<g_num_logger)
			{
				it->latch = 0; // release back
				INC_INT_STATS(time_recover2, get_sys_clock() - tt2); // compare LV
				continue;
			}
			found = true;
			//pi = *it;
			// we do not need to set its latch back to 0
			// because it will be moved out of the linked list
			//pool->erase(it);
			/*
			if(lt->tail == it) // we make empty() one-way relaxed: if the list is empty then empty() must be true
				lt->tail = prev;
			prev->next = it->next;
			prev->latch = 0;
			_mm_free(it);
			*/
			break;
		}
		//*(lt->mutex) = 0;
		uint64_t tt3 = get_sys_clock();
		INC_INT_STATS(time_recover2, tt3 - tt2); // same as above
		if(found)
		{
			recover_txn(it->txnData + sizeof(uint32_t) * 2);
			
			INC_INT_STATS(num_commits, 1);
			uint64_t tt4 = get_sys_clock();
			INC_INT_STATS(time_recover3, tt4 - tt3); // re-execute the transaction
					
			/*
			while(!ATOM_CAS(*lt->mutex, 0, 1))
					{
						PAUSE
					}
			if(*pi.LSN > *rlv_logger)
				*rlv_logger = *pi.LSN;
			*lt->mutex = 0;
			*/
			/*
			while(!ATOM_CAS(*(lt->mutex), 0, 1))
			{
				PAUSE
			}
			*/
			
			/*
			uint64_t rlv_local = *rlv_logger;
			uint64_t piLSN = *pi.LSN;
			while(rlv_local < piLSN && ATOM_CAS(*rlv_logger, rlv_local, piLSN))
			{
				rlv_local = *rlv_logger;
			}
			*/

			/*
			if(lt->poolempty())
			{
				uint64_t newval = lt->maxLSN[0];
				//cout << "1 changing from " << *rlv_logger << " to " << newval << endl;
				assert(newval >= *rlv_logger);
				//if(newval > *rlv_logger) 
				*rlv_logger = newval;
			}
			else
			{
				uint64_t newval = pool->next->LSN[0] - 1; //pool->begin()->LSN[0]- 1;
				//cout << "2 changing from " << *rlv_logger << " to " << newval << endl;
				assert(newval >= *rlv_logger);
				//if(newval > *rlv_logger) 
				*rlv_logger = newval;
			}
			*/
			//*(lt->mutex) = 0;
			// _mm_free(it->txnData);
			// _mm_free(it->txnLV); // collect garbage.
			// _mm_free(it->LSN);
			
			COMPILER_BARRIER

			it->recovered = 0; // so logging thread can recycle.
			uint64_t tt5 = get_sys_clock();
			INC_INT_STATS(time_recover4, tt5 - tt4); // clean
		}
		else
		{
			PAUSE
			// wait for other logger to catch up.
		}
	}
#else
	for(uint32_t i=0; i<g_num_logger; i++)
		printf("start: i=%d, recoverLVSPSC = %lu \n", i, *log_manager->recoverLVSPSC_min[i]);
	uint64_t recover_full_start = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	uint32_t workerId = GET_THD_ID / g_num_logger;
	uint32_t num_worker = g_thread_cnt / g_num_logger;
	#if COMPRESS_LSN_LOG
			uint64_t * LVFence = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
			memset(LVFence, 0, sizeof(uint64_t) * g_num_logger);
	#endif
	//cout << workerId << " " << logger << endl;
	// TODO: might not accurate
	LoggingThread *lt = logging_thds[logger];
	uint32_t realLogId = logging_thds[logger]->_thd_id % g_num_logger;
	//list<LoggingThread::poolItem> *pool = lt->pool;
	LoggingThread::poolItem* pool = lt->SPSCPools[workerId];
	//volatile uint64_t * rlv_logger = log_manager->recoverLV[logger];
	//assert(*rlv_logger == 0); // rlv logger might not be zero if multiple workers are working.
	assert(g_thread_cnt % g_num_logger == 0);
	volatile uint64_t *poolStart = &lt->SPSCPoolStart[workerId<<4];
	volatile uint64_t *poolEnd = &lt->SPSCPoolEnd[workerId<<4];
	//uint64_t poolStep = PRIMES[GET_THD_ID/g_num_logger];
	//while(!(lt->poolDone && poolEnd<=poolStart))
	// TODO: lt->pool->empty() might cause data racing.
	for(;;)
	{
		uint64_t recover_start = get_sys_clock();
		if(lt->poolDone)
		{
			if(*poolEnd<=*poolStart)
					break;
		}
		//if(lt->poolDone)	
		//	cout << lt->poolDone << " " << poolStart << " " << poolEnd << endl;
		bool found = false;
		LoggingThread::poolItem *it;
		uint64_t poolIndex = *poolStart;
		uint64_t tt2_l1 = get_sys_clock();
		
		bool checkSuccessive = true; // do not optimize this!
		INC_INT_STATS(time_debug0, tt2_l1 - recover_start);
		for(;poolIndex < *poolEnd; poolIndex ++)
		{
			//uint64_t tt = get_sys_clock();
			// it's ok if poolIndex here is actually behind the poolStart
			// and read some newly pushed item
			it = pool + poolIndex % g_poolsize_wait;
			if(it->recovered)
			{
				//INC_INT_STATS(time_recover1, get_sys_clock() - tt);  // the first ATOM_CAS
				continue;
			}
			if(checkSuccessive)
			{
				*log_manager->recoverLVSPSC[realLogId][workerId] = it->LSN[0] - 1;
				*poolStart = poolIndex;
				checkSuccessive = false;
			}
#if DECODE_AT_WORKER
			if(it->rasterized ==0)
			{
				assert(*(uint32_t*)it->oldp == 0xbeef || it->oldp[0] == 0x7f);
				it->size = *(uint32_t*)(it->oldp + sizeof(uint32_t));
				memcpy(it->txnData, it->oldp, it->size);
				//COMPILER_BARRIER  // rasterizedLSN must be updated after memcpy
				//assert(log_manager->_logger[realLogId]->rasterizedLSN[workerId][0] < it->LSN[0]);
				log_manager->_logger[realLogId]->rasterizedLSN[workerId][0] = it->LSN[0];
				char * ptdentry = it->txnData;
#if COMPRESS_LSN_LOG
								// read metainfo
								if(ptdentry[0] == 0x7f)
								{
									// this is a PSN Flush
									memcpy(LVFence, ptdentry + sizeof(uint32_t) * 2, sizeof(uint64_t) * g_num_logger);
									it->recovered = 1;// No recover for PSN
									it->rasterized = 1;
									INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger + sizeof(uint32_t) * 2);
									continue;
								}
								else
								{
										// use LVFence to update T.LV
										memcpy(it->txnLV, LVFence, sizeof(uint64_t) * g_num_logger);
										uint64_t psnCounter = *(uint64_t*)(ptdentry + it->size - 1); // sizeof(uint64_t));
										psnCounter &= 0xff; // extract only one byte
										//cout << psnCounter << endl;
										for(uint i=1; i<=psnCounter; i++)
										{
											//uint64_t psnToWrite = *(uint64_t*)(ptdentry + it->size - sizeof(uint64_t) - sizeof(uint64_t) * i);
											uint64_t psnToWrite = *(uint64_t*)(ptdentry + it->size - 1 - sizeof(uint64_t) * i);
											it->txnLV[psnToWrite&((1<<5)-1)] = psnToWrite >> 5;
										}
										//INC_INT_STATS(int_aux_bytes, (psnCounter + 1) * sizeof(uint64_t));
										INC_INT_STATS(int_aux_bytes, psnCounter * sizeof(uint64_t) + 1);
								}
#else
								// read meta_info
								uint64_t *LV_start = (uint64_t*)(ptdentry + it->size - sizeof(uint64_t) * g_num_logger);
								for(uint i=0; i<g_num_logger; i++)
								{
									it->txnLV[i] = LV_start[i];
								}
								INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger);
#endif
				INC_INT_STATS(num_log_entries, 1);
				
				it->rasterized = 1;
			}
#endif
			//INC_INT_STATS(time_recover1, get_sys_clock() - tt); // ATOM CAS
			uint i=0, j=0;
			// use SIMD and manual unroll
			//int unroll = (g_num_logger/4) * 4;
			for(; i<g_num_logger; i++)
			{
				//uint64_t txnLVi_cache = it->txnLV[i];
				if(it->txnLV[i] < *log_manager->recoverLVSPSC_min[i]){
					printf("i=%d, txnLV[i] = %lu, recoverLVSPSC = %lu \n", i, it->txnLV[i], *log_manager->recoverLVSPSC_min[i]);
					continue;
				}
					// must be good even if it is stale
				uint64_t recoverLV_i_min = UINT64_MAX;
				for(j=0; j<num_worker; j++)
				// unnecessary to be atomic.
				// It is good to have recoverLVSPSC in a same cacheline!
				{
					uint64_t recoverlvspscIJ = *log_manager->recoverLVSPSC[i][j];
						if(recoverLV_i_min > recoverlvspscIJ)
							recoverLV_i_min = recoverlvspscIJ;
						//break;
				}
				//INC_INT_STATS(int_debug10, 1);
				// unnecessary to atomically update
				*log_manager->recoverLVSPSC_min[i] = recoverLV_i_min;
				//if(j<num_worker)
				if(it->txnLV[i] > recoverLV_i_min)
					break;
			}
			if(i<g_num_logger)
			{
				//it->latch = 0; // release back
				continue;
			}
			found = true;
			break;
		}
		uint64_t tt2_i = get_sys_clock();
		INC_INT_STATS(time_debug1, tt2_i - tt2_l1);
		
		//assert(!(found==true && checkSuccessive==true));
		if(checkSuccessive) // && poolEnd > poolStart)// means the pool is full of recovered txns
		{
			*poolStart = poolIndex; // here poolIndex is the (maybe stale) poolEnd.
			// Note: we cannot write poolEnd here!
			// The logging thread might advance poolEnd before we reach this if clause (but after the above for loop).
			*log_manager->recoverLVSPSC[realLogId][workerId] = pool[(poolIndex-1) % g_poolsize_wait].LSN[0]; //lt->maxLSN[0];
			// this line is spending very much time!
		}
		uint64_t tt3 = get_sys_clock();
		INC_INT_STATS(time_debug2, tt3 - tt2_i);
		// 66% INC_INT_STATS(time_debug4, get_sys_clock() - recover_start);
		//*(lt->mutex) = 0;
		
		//INC_INT_STATS(time_recover2, tt3 - tt2); // same as above
		if(found)
		{
			recover_txn(it->txnData + sizeof(uint32_t) * 2);
			uint32_t size_aligned = it->size % 64 == 0 ? it->size : it->size + 64 - it->size % 64;
			//cout << get_sys_clock() - it->starttime << endl;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
			INC_FLOAT_STATS(latency, get_sys_clock() - it->starttime);
#pragma GCC diagnostic pop
			
			//uint64_t tt4 = get_sys_clock();
			//INC_INT_STATS(time_recover3, tt4 - tt3); // re-execute the transaction
			it->recovered = 1; // so logging thread can recycle.
			//uint64_t tt5 = get_sys_clock();
			//INC_INT_STATS(time_recover4, tt5 - tt4); // clean
			
			INC_INT_STATS(int_debug5, size_aligned);
			INC_INT_STATS(num_commits, 1);
			INC_INT_STATS(time_debug4, get_sys_clock() - tt3); // re-execute the transaction
		}
		else
		{
			PAUSE
			INC_INT_STATS(time_debug5, get_sys_clock() - tt3); // re-execute the transaction
			// wait for other logger to catch up.
		}
		INC_INT_STATS(time_debug3, get_sys_clock() - tt3); // re-execute the transaction
		//92% INC_INT_STATS(time_debug4, get_sys_clock() - recover_start);
#if RECOVERY_FULL_THR
		if (glob_manager->_workload->sim_done > 0)
				break;
#endif
	}
#if RECOVERY_FULL_THR	
	glob_manager->_workload->sim_done = 1;
#endif
	INC_INT_STATS(time_recover_full, get_sys_clock() - recover_full_start);
	ATOM_ADD(lt->workerDone[0], 1);  // notify the logger.
#endif
	//stringstream sstream;
	//stats->_stats[GET_THD_ID]->_int_stats[STAT_num_latency_count] = stats->_stats[GET_THD_ID]->_int_stats[STAT_num_commits];
	sstream << "Recover finished for worker " << GET_THD_ID << " of logger " << realLogId << endl;
	cout << sstream.str(); // atomic output.
#endif
}

#elif LOG_ALGORITHM == LOG_SERIAL 

void 
txn_man::serial_recover() {
	char default_entry[g_max_log_entry_size];
	// right now, only a single thread does the recovery job.
	if (GET_THD_ID > 0)
		return;
	uint32_t count = 0;
	while (true) {
		char * entry = default_entry;
		uint64_t tt = get_sys_clock();
		uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry);
		if (entry == NULL) {
			if (log_manager->_logger[0]->iseof()) {
				lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry);
				if (entry == NULL)
					break;
			}
			else { 
				PAUSE //usleep(50);
				INC_INT_STATS(time_io, get_sys_clock() - tt);
				continue;
			}
		}
		uint64_t tt2 = get_sys_clock();
		INC_INT_STATS(time_io, tt2 - tt);
		// Format for serial logging
		// | checksum | size | ... |
		assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
		
    	recover_txn(entry + sizeof(uint32_t) * 2);
		
		

		//printf("size=%d lsn=%ld\n", *(uint32_t*)(entry+4), lsn);
		COMPILER_BARRIER
		//INC_INT_STATS(time_recover_txn, get_sys_clock() - tt2);
		log_manager->_logger[0]->set_gc_lsn(lsn);
		INC_INT_STATS(num_commits, 1);
		count ++;
	}
}


#elif LOG_ALGORITHM == LOG_PARALLEL

void 
txn_man::parallel_recover() {
	// Execution thread.
	// Phase 1: Construct the dependency graph from the log records. 
	//   Phase 1.1. read in all log records, each record only having predecessor info.   
	if (GET_THD_ID == 0)
		printf("Phase 1.1 starts\n");
	uint64_t tt = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	while (true) {
		char * buffer = NULL;
		uint64_t file_size = 0;
		uint64_t base_lsn = 0;
		uint64_t tt = get_sys_clock();
		uint32_t chunk_num = log_manager[logger]->get_next_log_chunk(buffer, file_size, base_lsn);
		INC_INT_STATS(time_io, get_sys_clock() - tt);
		INC_FLOAT_STATS(log_bytes, file_size);
		if (chunk_num == (uint32_t)-1) 
			break;
	
		// Format of log record 
		// | checksum | size | ... 
		uint32_t offset = 0;
		uint64_t lsn = base_lsn;
		tt = get_sys_clock();
		while (offset < file_size) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size = 0; 
			uint32_t start = offset;
			if (UNLIKELY(start + sizeof(uint32_t) * 2 >= file_size)) {
//				printf("[1] logger=%d. chunknum=%d LSN=%ld. offset=%d, size=%d, file_size=%ld\n", 
//					logger, chunk_num, lsn, offset, size, file_size);
				break;
			}
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			if (UNLIKELY(start + size > file_size)) {
//				printf("[2] logger=%d. chunk=%d LSN=%ld. offset=%d, size=%d, file_size=%ld\n", 
//					logger, chunk_num, lsn, offset, size, file_size);
				break;
			}
			if (UNLIKELY(checksum != 0xbeef)) { 
//				printf("logger=%d. chunk=%d LSN=%ld. txn lost\n", logger, chunk_num, lsn);
				break;
			}
			M_ASSERT(size > 0 && size <= g_max_log_entry_size, "size=%d\n", size);
			uint64_t tid = ((uint64_t)logger << 48) | lsn;
			log_recover_table->addTxn(tid, buffer + start);
		
			//COMPILER_BARRIER
			offset = start + size;
			lsn += size; 
			M_ASSERT(offset <= file_size, "offset=%d, file_size=%ld\n", offset, file_size);
		}
		INC_INT_STATS(time_phase1_add_graph, get_sys_clock() - tt);
		log_manager[logger]->return_log_chunk(buffer, chunk_num);
	}

	INC_INT_STATS(time_phase1_1_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_INT_STATS(time_phase1_1, get_sys_clock() - tt);
/*	tt = get_sys_clock();
	if (GET_THD_ID == 0)
		printf("Phase 1.2 starts\n");
	// Phase 1.2. add in the successor info to the graph.
	log_recover_table->buildSucc();
	
	INC_INT_STATS(time_phase1_2_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_INT_STATS(time_phase1_2, get_sys_clock() - tt);
*/
	tt = get_sys_clock();
	
	if (GET_THD_ID == 0)
		printf("Phase 2 starts\n");
	// Phase 2. Infer WAR edges.   
	log_recover_table->buildWARSucc(); 
	
	INC_INT_STATS(time_phase2_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_INT_STATS(time_phase2, get_sys_clock() - tt);
	tt = get_sys_clock();

	if (GET_THD_ID == 0)
		printf("Phase 3 starts\n");
	// Phase 3. Recover transactions
	// XXX the following termination detection is a HACK
	// Basically if no thread has seen a new txn in 100 us,
	// the program is terminated.
	bool vote_done = false;
	uint64_t last_idle_time = 0; //get_sys_clock();
	while (true) { 
		char * log_entry = NULL;
		void * node = log_recover_table->get_txn(log_entry);		
		if (log_entry) {
			if (vote_done) {
		        ATOM_SUB_FETCH(GET_WORKLOAD->sim_done, 1);
				vote_done = false;
			}
			last_idle_time = 0;
			do {
            	recover_txn(log_entry);
				void * next = NULL;
				log_entry = NULL;
				log_recover_table->remove_txn(node, log_entry, next);
				node = next;
				INC_INT_STATS(num_commits, 1);
			} while (log_entry);
		} else { //if (log_recover_table->is_recover_done()) {
			if (last_idle_time == 0)
				last_idle_time = get_sys_clock();
			PAUSE
			if (!vote_done && get_sys_clock() - last_idle_time > 1 * 1000 * 1000) {
				vote_done = true;
		       	ATOM_ADD_FETCH(GET_WORKLOAD->sim_done, 1);
			}
			if (GET_WORKLOAD->sim_done == g_thread_cnt)
				break;
		}
	}

	INC_INT_STATS(time_phase3_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_INT_STATS(time_phase3, get_sys_clock() - tt);
}
#elif LOG_ALGORITHM == LOG_BATCH
void
txn_man::batch_recover()
{
//	if (GET_THD_ID == 0) {
//		_log_lock = new pthread_mutex_t;
//		pthread_mutex_init(_log_lock, NULL);
//	}
	assert(glob_manager->_workload->sim_done==0);
	pthread_barrier_wait(&worker_bar);
	
	uint64_t starttime = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	uint64_t file_size;
	uint32_t offset;
	while (true) {
		char * buffer = NULL;
		file_size = 0;
		uint64_t base_lsn = 0;
		uint64_t tt = get_sys_clock();
		uint32_t chunk_num = log_manager[logger]->get_next_log_chunk(buffer, file_size, base_lsn);
		//cout <<	GET_THD_ID << " " << chunk_num << endl;
		INC_INT_STATS(time_io, get_sys_clock() - tt);
		//INC_INT_STATS(time_debug1, get_sys_clock() - tt);
		INC_FLOAT_STATS(log_bytes, file_size);
		if (chunk_num == (uint32_t)-1) 
			break;
		assert(buffer);
		// Format of log record 
		// | checksum | size | ... 
		offset = 0;
		tt = get_sys_clock();
		while (offset < file_size) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size; 
			uint64_t tid;
			uint32_t start = offset;
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			assert(size < g_max_log_entry_size);
			UNPACK(buffer, tid, offset);
			if (checksum != 0xbeef) {
				printf("checksum=%x, offset=%d, fsize=%lu\n", checksum, offset, file_size);
				break;
			}
			
			recover_txn(buffer + offset);
			INC_INT_STATS(num_commits, 1);
			uint32_t size_aligned = size % 64 == 0 ? size: size + 64 - size % 64;
			INC_INT_STATS(int_debug5, size_aligned);
			offset = start + size_aligned;
#if RECOVERY_FULL_THR
			if (glob_manager->_workload->sim_done > 0)
				break;
#endif				
		}
		INC_INT_STATS(time_debug2, get_sys_clock() - tt);
		log_manager[logger]->return_log_chunk(buffer, chunk_num);
#if RECOVERY_FULL_THR				
		if (glob_manager->_workload->sim_done > 0)
				break;
#endif
	}
#if RECOVERY_FULL_THR
  //cout <<	GET_THD_ID << " set " << offset << " " << file_size << " " <<  stats->_stats[GET_THD_ID]->_int_stats[STAT_num_commits] << endl;
	glob_manager->_workload->sim_done = 1;
#endif
	INC_INT_STATS(time_phase1_1_raw, get_sys_clock() - starttime);
	//pthread_barrier_wait(&worker_bar);
	//INC_INT_STATS(time_debug0, get_sys_clock() - starttime);



/*
	///////////////////////////////////
	uint64_t tt = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	while (true) {
		int32_t next_epoch = ATOM_FETCH_SUB(*next_log_file_epoch[logger], 1);
		if (next_epoch <= 0) 
			break;

		string path;
		if (logger == 0) 		path = "/f0/yuxia/silo/";
		else if (logger == 1)	path = "/f1/yuxia/silo/";
		else if (logger == 2)	path = "/f2/yuxia/silo/";
		else if (logger == 3)	path = "/data/yuxia/silo/";
		
		//if (logger == 3) 
		//	pthread_mutex_lock(_log_lock);

		string bench = (WORKLOAD == YCSB)? "YCSB" : "TPCC";
		path += "BD_log" + to_string(logger) + "_" + bench + ".log." + to_string(next_epoch);
		if (logger == 3)
			pthread_mutex_lock(_log_lock);
		int fd = open(path.c_str(), O_RDONLY | O_DIRECT);
		uint32_t fsize = lseek(fd, 0, SEEK_END);
		char * buffer = new char [fsize];
		lseek(fd, 0, SEEK_SET);
		uint32_t bytes = read(fd, buffer, fsize);
		assert(bytes == fsize);
		if (logger == 3) 
			pthread_mutex_unlock(_log_lock);
		
		INC_FLOAT_STATS(log_bytes, fsize);
		// Format for batch logging 
		// | checksum | size | TID | N | (table_id | primary_key | data_length | data) * N
		uint32_t offset = 0;
		while (offset < fsize) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size; 
			uint64_t tid;
			uint32_t start = offset;
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			UNPACK(buffer, tid, offset);
			if (checksum != 0xbeef) {
				//printf("checksum=%x, offset=%d, fsize=%d\n", checksum, offset, fsize);
				break;
			}
			
			recover_txn(buffer + offset);
			INC_INT_STATS(num_commits, 1);
			
			offset = start + size;
		}
		//printf("logger = %d. Epoch %d done\n", logger, next_epoch);
	}

	INC_INT_STATS(time_phase1_1_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);

	// each worker thread picks a log file and process it.
	//  
	//	 read from log file to buffer.
	//   process in arbitrary TID order.
	*/
}
#endif

uint32_t
txn_man::get_log_entry_size()
{
	assert(false);
	return 0;
/*
#if LOG_TYPE == LOG_DATA
	uint32_t buffsize = 0;
  	// size, txn_id and wr_cnt
	buffsize += sizeof(uint32_t) + sizeof(txn_id) + sizeof(wr_cnt);
  	// for table names
  	// TODO. right now, only store tableID. No column ID.
  	buffsize += sizeof(uint32_t) * wr_cnt;
  	// for keys
  	buffsize += sizeof(uint64_t) * wr_cnt;
  	// for data length
  	buffsize += sizeof(uint32_t) * wr_cnt; 
  	// for data
  	for (uint32_t i=0; i < wr_cnt; i++) {
		if (WORKLOAD == TPCC)
	    	buffsize += accesses[i]->orig_row->get_tuple_size();
		else 
			// TODO. For YCSB, only log 100 bytes of a tuple.  
	    	buffsize += 100; 

		//printf("tuple size=%ld\n", accesses[i]->orig_row->get_tuple_size());
	}
  	return buffsize; 
#elif LOG_TYPE == LOG_COMMAND
	// Format:
	//   size | txn_id | cmd_log_size
	return sizeof(uint32_t) + sizeof(txn_id) + get_cmd_log_size();
#else
	assert(false);
#endif
*/
}

uint32_t 
txn_man::get_log_entry_length()  ////////////////////// TODOTODO
{
	// TODO. in order to have a fair comparison with SiloR, Taurus only supports Silo at the moment 
	//assert(CC_ALG == SILO);
	//uint32_t ret = 0;
#if LOG_ALGORITHM != LOG_NO
#if LOG_TYPE == LOG_DATA
	// Format for serial logging
	// | checksum:4 | size:4 | N:4 | (table_id:4 | primary_key:8 | data_length:4 | data:?) * N
	// Format for parallel logging
	// | checksum | size | predecessor_info | N | (table_id | primary_key | data_length | data) * N
	//
	// predecessor_info has the following format
	// if TRACK_WAR_DEPENDENCY
	//   | num_raw_preds | TID * num_raw_preds | key * num_raw_preds | table * ...
	//   | num_waw_preds | TID * num_waw_preds | key * num_waw_preds | table * ...
	// else 
	//   | num_raw_preds | TID * num_raw_preds 
	//   | num_waw_preds | TID * num_waw_preds
	//
	// Format for batch logging 
	// | checksum | size | TID | N | (table_id | primary_key | data_length | data) * N
	// 
	// Assumption: every write is actually an update. 
	// predecessors store the TID of predecessor transactions. 
	uint32_t offset = sizeof(uint32_t) * 2;
  #if LOG_ALGORITHM == LOG_PARALLEL 
	offset += sizeof(_num_raw_preds);
	offset += _num_raw_preds * sizeof(uint64_t);
	#if TRACK_WAR_DEPENDENCY
	offset += _num_raw_preds * sizeof(uint64_t);
	offset += _num_raw_preds * sizeof(uint32_t);
	#endif
	offset += sizeof(_num_waw_preds);
    offset += _num_waw_preds * sizeof(uint64_t);
	#if TRACK_WAR_DEPENDENCY
	offset += _num_waw_preds * sizeof(uint64_t);
	offset += _num_waw_preds * sizeof(uint32_t);
	#endif
  #elif LOG_ALGORITHM == LOG_BATCH
    offset += sizeof(_cur_tid);
  #elif LOG_ALGORITHM == LOG_TAURUS
	// no need to pack other stuff
  #endif
	offset += sizeof(wr_cnt);
	
	#if LOG_ALGORITHM == LOG_TAURUS && CC_ALG == SILO
	uint32_t counter_wr = 0;
	for (uint32_t i = 0; i < row_cnt; i ++) {
		if(accesses[i]->type != WR) continue; // write_set is cleared in validate_silo
		row_t * orig_row = accesses[i]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
		//assert(tuple_size!=0);
		offset += sizeof(table_id);
		offset += sizeof(key);
		offset += sizeof(tuple_size);
		offset += tuple_size;
		counter_wr ++;
	}
	assert(counter_wr == wr_cnt);
	#else
	for (uint32_t i = 0; i < wr_cnt; i ++) {
	
		row_t * orig_row = accesses[write_set[i]]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
		//assert(tuple_size!=0);
		offset += sizeof(table_id);
		offset += sizeof(key);
		offset += sizeof(tuple_size);
		offset += tuple_size;
	}
	#endif
	// TODO checksum is ignored. 
	_log_entry_size = offset;
	assert(_log_entry_size < g_max_log_entry_size);

#elif LOG_TYPE == LOG_COMMAND
	// Format for serial logging
	// 	| checksum | size | benchmark_specific_command | 
	// Format for parallel logging
	// 	| checksum | size | predecessor_info | benchmark_specific_command | 
	uint32_t offset = 2 * sizeof(uint32_t);
  #if LOG_ALGORITHM == LOG_PARALLEL 
	offset += sizeof(_num_raw_preds);
	offset += _num_raw_preds * sizeof(uint64_t);
	#if TRACK_WAR_DEPENDENCY
	offset += _num_raw_preds * sizeof(uint64_t);
	offset += _num_raw_preds * sizeof(uint32_t);
	#endif
	offset += sizeof(_num_waw_preds);
    offset += _num_waw_preds * sizeof(uint64_t);
	#if TRACK_WAR_DEPENDENCY
	offset += _num_waw_preds * sizeof(uint64_t);
	offset += _num_waw_preds * sizeof(uint32_t);
	#endif
  #endif
	// internally, the following function will update _log_entry_size and _log_entry
	offset += get_cmd_log_entry_length();
#else
	assert(false);
#endif
	return offset;
#endif
	assert(false);
	return 0;
}

void 
txn_man::create_log_entry()  ////////////////////// TODOTODO
{
	// TODO. in order to have a fair comparison with SiloR, Taurus only supports Silo at the moment 
	//assert(CC_ALG == SILO);
#if LOG_ALGORITHM != LOG_NO
#if LOG_TYPE == LOG_DATA
	// Format for serial logging
	// | checksum:4 | size:4 | N:4 | (table_id:4 | primary_key:8 | data_length:4 | data:?) * N
	// Format for parallel logging
	// | checksum | size | predecessor_info | N | (table_id | primary_key | data_length | data) * N
	//
	// predecessor_info has the following format
	// if TRACK_WAR_DEPENDENCY
	//   | num_raw_preds | TID * num_raw_preds | key * num_raw_preds | table * ...
	//   | num_waw_preds | TID * num_waw_preds | key * num_waw_preds | table * ...
	// else 
	//   | num_raw_preds | TID * num_raw_preds 
	//   | num_waw_preds | TID * num_waw_preds
	//
	// Format for batch logging 
	// | checksum | size | TID | N | (table_id | primary_key | data_length | data) * N
	// 
	// Assumption: every write is actually an update. 
	// predecessors store the TID of predecessor transactions. 
	uint32_t offset = 0;
	uint32_t checksum = 0xbeef;  // we also use this to distinguish PSN items and log items
	//uint32_t size = 0;
	PACK(_log_entry, checksum, offset);
	//PACK(_log_entry, size, offset);
	offset += sizeof(uint32_t); // make space for size;
  #if LOG_ALGORITHM == LOG_PARALLEL 
	uint32_t start = offset;
    PACK(_log_entry, _num_raw_preds, offset);
	PACK_SIZE(_log_entry, _raw_preds_tid, _num_raw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _raw_preds_key, _num_raw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _raw_preds_table, _num_raw_preds * sizeof(uint32_t), offset);
	//for (uint32_t i = 0; i < _num_raw_preds; i++)
	//	if (_raw_preds_key[i] == 1 && _raw_preds_table[i] == 0)
	//	printf("tid=%ld, key=%ld, table=%d\n", _raw_preds_tid[i], _raw_preds_key[i], _raw_preds_table[i]);

	#endif
    PACK(_log_entry, _num_waw_preds, offset);
	PACK_SIZE(_log_entry, _waw_preds_tid, _num_waw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _waw_preds_key, _num_waw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _waw_preds_table, _num_waw_preds * sizeof(uint32_t), offset);
	#endif
	uint32_t dep_size = offset - start;	
	INC_FLOAT_STATS(log_dep_size, dep_size);
	//for (uint32_t i = 0; i < _num_waw_preds; i++)
	//	if (_waw_preds_key[i] == 1 && _waw_preds_table[i] == 0)
	//		printf("tid=%ld, key=%ld, table=%d\n", _waw_preds_tid[i], _waw_preds_key[i], _waw_preds_table[i]);
  #elif LOG_ALGORITHM == LOG_BATCH
    PACK(_log_entry, _cur_tid, offset);
  #elif LOG_ALGORITHM == LOG_TAURUS
	// no need to pack other stuff
  #endif

	
	
	/* #if LOG_ALGORITHM == LOG_TAURUS && CC_ALG == SILO
	uint32_t counter_wr = 0;
	for (uint32_t i = 0; i < row_cnt; i ++) {
		if(accesses[i]->type != WR) continue; // write_set is cleared in validate_silo
		row_t * orig_row = accesses[i]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
		char * tuple_data = accesses[i]->data;
		//assert(tuple_size!=0);

		PACK(_log_entry, table_id, offset);
		PACK(_log_entry, key, offset);
		PACK(_log_entry, tuple_size, offset);
		PACK_SIZE(_log_entry, tuple_data, tuple_size, offset);
		counter_wr ++;
	}
	assert(counter_wr == wr_cnt);
	#else*/

#if VERIFICATION || ELLE_OUTPUT // we need to trace for elle as well
	assert(wr_cnt>0);
	PACK(_log_entry, row_cnt, offset);
    // we also need to trace reads besides write operations
    for (uint32_t i = 0; i < row_cnt; i ++) {
	
		row_t * orig_row = accesses[i]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
        uint32_t access_type = accesses[i]->type;
		char * tuple_data = accesses[i]->data;
		//assert(tuple_size!=0);

		PACK(_log_entry, table_id, offset);
		PACK(_log_entry, key, offset);
        PACK(_log_entry, access_type, offset);
		PACK(_log_entry, tuple_size, offset);
		PACK_SIZE(_log_entry, tuple_data, tuple_size, offset);
	}
#if CC_ALG == DETRESERVE
	// pack the current batch number
	PACK(_log_entry, currentBatch, offset);
#endif	
#else
	PACK(_log_entry, wr_cnt, offset);
	for (uint32_t i = 0; i < wr_cnt; i ++) {
	
		row_t * orig_row = accesses[write_set[i]]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
		char * tuple_data = accesses[write_set[i]]->data;
		//assert(tuple_size!=0);

		PACK(_log_entry, table_id, offset);
		PACK(_log_entry, key, offset);
		PACK(_log_entry, tuple_size, offset);
		PACK_SIZE(_log_entry, tuple_data, tuple_size, offset);
	}
#endif
	// #endif
	// TODO checksum is ignored. 
	_log_entry_size = offset;
	assert(_log_entry_size < g_max_log_entry_size);
	// update size. 
	memcpy(_log_entry + sizeof(uint32_t), &_log_entry_size, sizeof(uint32_t));
	//cout << _log_entry_size << endl;
	INC_FLOAT_STATS(log_total_size, _log_entry_size);
	INC_INT_STATS(num_log_entries, 1);

#elif LOG_TYPE == LOG_COMMAND
	// Format for serial logging
	// 	| checksum | size | benchmark_specific_command | 
	// Format for parallel logging
	// 	| checksum | size | predecessor_info | benchmark_specific_command | 
	uint32_t offset = 0;
	uint32_t checksum = 0xbeef;
	uint32_t size = 0;
	PACK(_log_entry, checksum, offset);
	PACK(_log_entry, size, offset);
  #if LOG_ALGORITHM == LOG_PARALLEL 
	uint32_t start = offset;
    PACK(_log_entry, _num_raw_preds, offset);
	PACK_SIZE(_log_entry, _raw_preds_tid, _num_raw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _raw_preds_key, _num_raw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _raw_preds_table, _num_raw_preds * sizeof(uint32_t), offset);
	#endif
    PACK(_log_entry, _num_waw_preds, offset);
	PACK_SIZE(_log_entry, _waw_preds_tid, _num_waw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _waw_preds_key, _num_waw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _waw_preds_table, _num_waw_preds * sizeof(uint32_t), offset);
	#endif
	uint32_t dep_size = offset - start;	
	INC_FLOAT_STATS(log_dep_size, dep_size);
  #endif
    _log_entry_size = offset;
	// internally, the following function will update _log_entry_size and _log_entry
	get_cmd_log_entry();
	
	assert(_log_entry_size < g_max_log_entry_size);
	assert(_log_entry_size > sizeof(uint32_t) * 2);
	memcpy(_log_entry + sizeof(uint32_t), &_log_entry_size, sizeof(uint32_t));
	INC_FLOAT_STATS(log_total_size, _log_entry_size);
	INC_INT_STATS(num_log_entries, 1);
#else
	assert(false);
#endif
#endif
}
