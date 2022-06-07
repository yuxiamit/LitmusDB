#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <sched.h>
#include "global.h"
#include "manager.h"
#include "thread.h"
#include "txn.h"
#include "wl.h"
#include "query.h"
#include "plock.h"
#include "occ.h"
#include "vll.h"
#include "ycsb_query.h"
#include "tpcc_query.h"
#include "mem_alloc.h"
#include "inttypes.h"
#include "txn_man_vec.h"

#if CC_ALG == DETRESERVE
#include "parallel_defs.h"
#include "detreserve.h"
__thread int thread_id;
__thread int initialized;
__thread txn_man *thread_man;
txn_man ** local_txn_man;
#endif

void thread_t::init(uint64_t thd_id, workload * workload) {
	_thd_id = thd_id;
	_wl = workload;
	//srand48_r((_thd_id + 1) * get_sys_clock(), &buffer);
	_abort_buffer_size = ABORT_BUFFER_SIZE;
	_abort_buffer = (AbortBufferEntry *) _mm_malloc(sizeof(AbortBufferEntry) * _abort_buffer_size, ALIGN_SIZE); 
	for (int i = 0; i < _abort_buffer_size; i++)
		_abort_buffer[i].query = NULL;
	_abort_buffer_empty_slots = _abort_buffer_size;
	_abort_buffer_enable = g_abort_buffer_enable;
}

uint64_t thread_t::get_thd_id() { return _thd_id; }
uint64_t thread_t::get_host_cid() {	return _host_cid; }
void thread_t::set_host_cid(uint64_t cid) { _host_cid = cid; }
uint64_t thread_t::get_cur_cid() { return _cur_cid; }
void thread_t::set_cur_cid(uint64_t cid) {_cur_cid = cid; }

RC thread_t::run() {
#if !NOGRAPHITE
	_thd_id = CarbonGetTileId();
#endif
	glob_manager->set_thd_id( get_thd_id() );
	if (warmup_finish) {
		mem_allocator.register_thread(_thd_id);
	}
	//pthread_barrier_wait( &warmup_bar );
	//stats.init(get_thd_id());
	pthread_barrier_wait( &warmup_bar );

	set_affinity(get_thd_id()); // TODO: to make this work

	//myrand rdm;
	//rdm.init(get_thd_id());
	RC rc = RCOK;

	txn_man * m_txn;

	rc = _wl->get_txn_man(m_txn, this);
	assert (rc == RCOK);
	glob_manager->set_txn_man(m_txn);

	base_query * m_query = NULL;
	// XXX ???
	uint64_t thd_txn_id = g_log_parallel_num_buckets;
	UInt64 txn_cnt = 0;

	if (g_log_recover) {
        //if (get_thd_id() == 0)
		uint64_t starttime = get_sys_clock();
		COMPILER_BARRIER
		m_txn->recover();
		COMPILER_BARRIER
		INC_FLOAT_STATS(run_time, get_sys_clock() - starttime);
		return FINISH;
	}
#if CC_ALG == DETRESERVE

	if(_thd_id != g_thread_cnt - 1) return RCOK;
	// only a single thread runs the main logic
	ts_t starttime = get_sys_clock();
	uint32_t *I = (uint32_t *) _mm_malloc(sizeof(uint32_t) * g_batch_size, ALIGN_SIZE);
	uint32_t *Ihold = (uint32_t *) _mm_malloc(sizeof(uint32_t) * g_batch_size, ALIGN_SIZE);
	bool *keep = (bool *) _mm_malloc(sizeof(bool) * g_batch_size, ALIGN_SIZE);
	uint32_t cilkThreadCount = g_thread_cnt;
	char num[3];
	sprintf(num,"%d",cilkThreadCount);
	setWorkers(cilkThreadCount);
	std::cout << "The number of threads " << cilkThreadCount << std::endl;
	parallel_for(uint32_t i=0; i<g_batch_size; i++) keep[i] = false;
	uint32_t base_pri = UINT_MAX / 2;
	uint32_t upper_pri = UINT_MAX / 2;
	uint32_t round = 0; 
	uint32_t numberDone = 0; // number of iterations done
	uint32_t numberKeep = 0; // number of iterations to carry to next round
	uint32_t totalProcessed = 0;
	uint32_t curRoundSize = g_batch_size; //40000; //maxRoundSize; //10000;
    uint32_t totalKeep = 0;

	uint32_t allmem = 0;
	// initialize queries

	base_query **queries = (base_query**) _mm_malloc(sizeof(base_query*) * g_max_txns_per_thread, ALIGN_SIZE);
	parallel_for
    (uint32_t i=0; i<g_max_txns_per_thread; i++)
		queries[i] = query_queue->get_next_query( _thd_id );
	local_txn_man = (txn_man**) _mm_malloc(sizeof(txn_man*) * g_batch_size, ALIGN_SIZE);
	
	parallel_for
    (uint32_t i=0; i<g_batch_size; i++)
	{
		//_wl->get_txn_man(local_txn_man[i], getWorkerId());
		_wl->get_txn_man(local_txn_man[i], this);
	}
	
	currentBatch = 0;

	while(numberDone < g_max_txns_per_thread) {
		// fill in the next batch
		uint32_t size = min(curRoundSize, g_max_txns_per_thread - numberDone);
		assert(base_pri > size);
		base_pri -= size;
		totalProcessed += size;
		// Phase 1
		parallel_for (uint32_t i1 =0; i1 < size; i1++) {
			/*if (__builtin_expect(!initialized, 0)) {
				initialized = true;
				thread_id = getWorkerId();
			}*/
			thread_man = local_txn_man[i1];

			if (i1>= numberKeep)
				I[i1] = numberDone + i1;
			TXN_START(i1 + base_pri, i1);
			rc = thread_man->run_txn(queries[I[i1]]);
			keep[i1] = TXN_END;
		}
		// Phase 2
		parallel_for(uint32_t i2=0; i2<size; i2++)
		{
			thread_man = local_txn_man[i2];
			if(keep[i2])
			{
				keep[i2] = thread_man->process_phase2(i2 + base_pri);
			}
            else thread_man->cleanAfterPhase2();
            //thread_man->cleanup(keep[i2]?Abort:RCOK);
		}
		// cleanup
		numberKeep = sequence::pack(I, Ihold, (bool *)keep, size);
		swap(I, Ihold);

		totalKeep += numberKeep;
		numberDone += size - numberKeep;

        INC_INT_STATS(time_cc_latency, (get_sys_clock() - starttime) * (size - numberKeep));

		INC_INT_STATS(num_commits, size - numberKeep);

		//parallel_for (uint32_t i=0; i<getWorkers(); i++)
		//local_txn_man[i]->cleanup();

		assert(numberKeep < size);
		currentBatch++;

	}
	ts_t endtime = get_sys_clock();
	uint64_t timespan = endtime - starttime;
	INC_FLOAT_STATS(run_time, timespan);
    INC_INT_STATS(int_batch_num, currentBatch);
	_wl->sim_done = g_thread_cnt; // tell the logging thread to finish
	return FINISH;
#else
    ts_t thread_starttime = get_sys_clock();
	while (true) {
		ts_t starttime = get_sys_clock();
		if (WORKLOAD != TEST) {
			int trial = 0;
			if (_abort_buffer_enable) {
				m_query = NULL;
				while (trial < 2) {
					ts_t curr_time = get_sys_clock();
					ts_t min_ready_time = UINT64_MAX;
					if (_abort_buffer_empty_slots < _abort_buffer_size) {
						for (int i = 0; i < _abort_buffer_size; i++) {
							if (_abort_buffer[i].query != NULL && curr_time > _abort_buffer[i].ready_time) {
								m_query = _abort_buffer[i].query;
								_abort_buffer[i].query = NULL;
								_abort_buffer_empty_slots ++;
								break;
							} else if (_abort_buffer_empty_slots == 0 
									  && _abort_buffer[i].ready_time < min_ready_time) 
								min_ready_time = _abort_buffer[i].ready_time;
						}
					}
					if (m_query == NULL && _abort_buffer_empty_slots == 0) {
						assert(trial == 0);
						//M_ASSERT(min_ready_time >= curr_time, "min_ready_time=%ld, curr_time=%ld\n", min_ready_time, curr_time);
						assert(min_ready_time >= curr_time);
						usleep(min_ready_time - curr_time);
					}
					else if (m_query == NULL) {
						m_query = query_queue->get_next_query( _thd_id );
					#if CC_ALG == WAIT_DIE
						m_txn->set_ts(get_next_ts());
					#endif
					}
					if (m_query != NULL)
						break;
				}
			} else {
				if (rc == RCOK)
					m_query = query_queue->get_next_query( _thd_id );
			}
		}
		INC_STATS(_thd_id, time_query, get_sys_clock() - starttime);
		m_txn->set_start_time(get_sys_clock());
		m_txn->abort_cnt = 0;
//#if CC_ALG == VLL
//		_wl->get_txn_man(m_txn, this);
//#endif
		m_txn->set_txn_id(get_thd_id() + thd_txn_id * g_thread_cnt);
		thd_txn_id ++;

		if ((CC_ALG == HSTORE && !HSTORE_LOCAL_TS)
				|| CC_ALG == MVCC 
				|| CC_ALG == HEKATON
				|| CC_ALG == TIMESTAMP) 
			m_txn->set_ts(get_next_ts());

		rc = RCOK;
#if CC_ALG == HSTORE
		rc = part_lock_man.lock(m_txn, m_query->part_to_access, m_query->part_num);
#elif CC_ALG == VLL
		vll_man.vllMainLoop(m_txn, m_query);
#elif CC_ALG == MVCC || CC_ALG == HEKATON
		glob_manager->add_ts(get_thd_id(), m_txn->get_ts());
#elif CC_ALG == OCC
		// In the original OCC paper, start_ts only reads the current ts without advancing it.
		// But we advance the global ts here to simplify the implementation. However, the final
		// results should be the same.
		m_txn->start_ts = get_next_ts(); 
#endif
		if (rc == RCOK) 
		{
#if CC_ALG != VLL
			rc = m_txn->run_txn(m_query);
#endif
#if CC_ALG == HSTORE
			part_lock_man.unlock(m_txn, m_query->part_to_access, m_query->part_num);
#endif
		}
		if (rc == Abort) {
			//cout << m_txn->get_txn_id() << " Aborted" << endl;
			uint64_t penalty = 0;
			if (ABORT_PENALTY != 0)  {
				double r;
				//drand48_r(&buffer, &r);
				r = erand48(buffer);
				penalty = r * ABORT_PENALTY;
			}
			if (!_abort_buffer_enable)
				usleep(penalty / 1000);
			else {
				assert(_abort_buffer_empty_slots > 0);
				for (int i = 0; i < _abort_buffer_size; i ++) {
					if (_abort_buffer[i].query == NULL) {
						_abort_buffer[i].query = m_query;
						_abort_buffer[i].ready_time = get_sys_clock() + penalty;
						_abort_buffer_empty_slots --;
						break;
					}
				}
			}
		}

		ts_t endtime = get_sys_clock();
		uint64_t timespan = endtime - starttime;
		INC_FLOAT_STATS(run_time, timespan);
		// running for more than 1000 seconds.
//		if (stats._stats[GET_THD_ID]->run_time > 1000UL * 1000 * 1000 * 1000) {	
//			cerr << "Running too long" << endl;
//			exit(0);
//		}
		if (rc == RCOK) {
            INC_INT_STATS(time_cc_latency, get_sys_clock() - thread_starttime);
			INC_INT_STATS(num_commits, 1);
			//cout << "Commit" << endl;
			txn_cnt ++;
			/*
			if(txn_cnt % 100 == 0)
				printf("[%" PRIu64 "] %" PRIu64 "\n", GET_THD_ID, txn_cnt);
			*/
		} else if (rc == Abort) {
			INC_STATS(get_thd_id(), time_abort, timespan);
			//INC_STATS(get_thd_id(), abort_cnt, 1);
			INC_INT_STATS(num_aborts, 1);
			//stats.abort(get_thd_id());
			m_txn->abort_cnt ++;
		}

		if (rc == FINISH)
			return rc;
		if (!warmup_finish && txn_cnt >= WARMUP / g_thread_cnt) 
		{
			//stats.clear( get_thd_id() );
			return FINISH;
		}
		if ((warmup_finish && txn_cnt >= g_max_txns_per_thread) || _wl->sim_done > 0) {
			ATOM_ADD_FETCH(_wl->sim_done, 1);
			uint64_t terminate_time = get_sys_clock(); 
			printf("sim_done = %d\n", _wl->sim_done);
			while (_wl->sim_done != g_thread_cnt && get_sys_clock() - terminate_time < 1000 * 1000) {
				m_txn->try_commit_txn();
				usleep(10);
			}
			return FINISH;
	    }
	}
#endif
	assert(false);
}


ts_t
thread_t::get_next_ts() {
	if (g_ts_batch_alloc) {
		if (_curr_ts % g_ts_batch_num == 0) {
			_curr_ts = glob_manager->get_ts(get_thd_id());
			_curr_ts ++;
		} else {
			_curr_ts ++;
		}
		return _curr_ts - 1;
	} else {
		_curr_ts = glob_manager->get_ts(get_thd_id());
		return _curr_ts;
	}
}


