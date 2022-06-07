#include "logging_thread.h"
#include "manager.h"
#include "wl.h"
#include "serial_log.h"
#include "parallel_log.h"
#include "taurus_log.h"
#include "locktable.h"
#include "log.h"
#include <sys/types.h>
#include <aio.h>
#include <fcntl.h>
#include <errno.h>
#include <sstream>

#if LOG_ALGORITHM != LOG_NO

#define BYPASS_WORKER false
// This switch is used to test the raw throughput of the log reader.

LoggingThread::LoggingThread()
{
	#if LOG_ALGORITHM == LOG_TAURUS && !PER_WORKER_RECOVERY
		if(g_log_recover)
		{
	#if !DECODE_PER_WORKER && COMPRESS_LSN_LOG
			LVFence = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
			memset(LVFence, 0, sizeof(uint64_t) * g_num_logger);
	#endif
			maxLSN = (uint64_t*)_mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*maxLSN = 0;
#if RECOVER_TAURUS_LOCKFREE
			//pool = (list<poolItem>*)_mm_malloc(sizeof(list<poolItem>), ALIGN_SIZE);
			//new (pool) list<poolItem>();
			pool = (poolItem*)_mm_malloc(sizeof(poolItem) * g_poolsize_wait, ALIGN_SIZE);
			memset(pool, 0, sizeof(poolItem) * g_poolsize_wait);
			poolStart = poolEnd = 0;
			poolsize = 0;
			for(uint32_t k=0; k<g_poolsize_wait; k++)
			{
				pool[k].latch = 1; // make the latch
				// just in case someone goes beyond the range at the beginning;
				pool[k].txnData = (char*) _mm_malloc(g_max_log_entry_size, ALIGN_SIZE);
				pool[k].txnLV = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
				pool[k].LSN = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			}
			mutex = (uint64_t*)_mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*mutex = 0;
			/*
			poolPtr = (uint64_t**) _mm_malloc(sizeof(uint64_t*) * g_thread_cnt, ALIGN_SIZE);
			for(uint32_t i=0; i<g_thread_cnt; i++)
			{
				poolPtr[i] = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
				*poolPtr[i] = 0;
			}
			*/
#else
	// initializer for multi-SPSC queues
			assert(g_thread_cnt % g_num_logger == 0);
			uint64_t num_worker = g_thread_cnt / g_num_logger;
			SPSCPools = (poolItem**) _mm_malloc(sizeof(poolItem*) * num_worker + ALIGN_SIZE, ALIGN_SIZE);
			for(uint32_t i=0; i<num_worker; i++)
			{
				// allocate the memories
				SPSCPools[i] = (poolItem*) _mm_malloc(sizeof(poolItem) * g_poolsize_wait + ALIGN_SIZE, ALIGN_SIZE);
				for(uint32_t j=0; j<g_poolsize_wait; j++)
				{
					SPSCPools[i][j].txnData = (char*) _mm_malloc(g_max_log_entry_size, ALIGN_SIZE);
					SPSCPools[i][j].txnLV = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
					SPSCPools[i][j].LSN = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
					SPSCPools[i][j].LSN[0] = 0; // this is important.
					SPSCPools[i][j].starttime = 0;
				}
			}
			/*	
			SPSCPoolStart = (cacheline*) _mm_malloc(sizeof(cacheline) * num_worker, ALIGN_SIZE);
			SPSCPoolEnd = (cacheline*) _mm_malloc(sizeof(cacheline) * num_worker, ALIGN_SIZE);
			for(uint32_t i=0; i<num_worker; i++)
			{
				SPSCPoolStart[i]._val = 0;
				SPSCPoolEnd[i]._val = 0;
			}
			workerDone._val = 0;
			*/
			SPSCPoolStart = (volatile uint64_t *) _mm_malloc(sizeof(uint64_t) * 16 * num_worker, ALIGN_SIZE);
			SPSCPoolEnd = (volatile uint64_t *) _mm_malloc(sizeof(uint64_t) * 16 * num_worker, ALIGN_SIZE);
			for(uint32_t i=0; i<num_worker; i++)
			{
				SPSCPoolStart[i<<4] = 0;
				SPSCPoolEnd[i<<4] = 0;
			}
			workerDone = (volatile uint64_t *) _mm_malloc(sizeof(uint64_t) * 16, ALIGN_SIZE);
			workerDone[0] = 0;
#endif
		}
	#endif
	poolDone = false;
}

void printLV(uint64_t *lv)
{
	cout << "LV:" << endl;
	for (uint i=0; i<g_num_logger; i++)
	{
		cout << lv[i] << "  ";
	}
	cout << endl;
}

RC
LoggingThread::run()
{
	pthread_barrier_wait( &log_bar );
	
  //pthread_barrier_wait( &warmup_bar );
  if (LOG_ALGORITHM == LOG_BATCH && g_log_recover)
	  return FINISH; 
	
	//uint64_t logging_start = get_sys_clock();

	glob_manager->set_thd_id( _thd_id );
	LogManager * logger;
  #if LOG_ALGORITHM == LOG_SERIAL
	//uint32_t logger_id = GET_THD_ID % g_num_logger;
	logger = log_manager->_logger[0];
  #elif LOG_ALGORITHM == LOG_PARALLEL
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	logger = log_manager[logger_id];
  #elif LOG_ALGORITHM == LOG_TAURUS
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	logger = log_manager->_logger[logger_id];
  #elif LOG_ALGORITHM == LOG_BATCH
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	logger = log_manager[logger_id];
  #endif
	uint64_t starttime = get_sys_clock(); 
	uint64_t total_log_data = 0;
	uint64_t flushcount = 0;
	if (g_log_recover) {  // recover
  #if LOG_ALGORITHM == LOG_PARALLEL
  		return FINISH;
  #endif
		//stats.init( _thd_id );
#if LOG_ALGORITHM == LOG_TAURUS && !PER_WORKER_RECOVERY
	char * default_entry = (char*)_mm_malloc(g_max_log_entry_size, 64);
#if RECOVER_TAURUS_LOCKFREE	
	// read some data and starts to process
				if(GET_THD_ID ==0)
					cout << "Recovery Starts." << endl;
				uint32_t count = 0;
				// uint32_t logger_id = GET_THD_ID % g_num_logger;
				for(;;) {
					/*
					if(poolsize > g_poolsize_wait)
					{
						usleep(100);
						continue;
					}
					*/
					char * entry = default_entry;
					uint64_t tt = get_sys_clock();
					uint64_t lsn;
					lsn = logger->get_next_log_entry_non_atom(entry);
					if (entry == NULL) {
						// if the pool has too many txns, we will wait to reduce the searching cost of workers.
						// as well as their atomic waits.
						uint32_t bytes = logger->tryReadLog(); // load more log into the buffer.
						total_log_data += bytes;
						if (logger->iseof()) {
							entry = default_entry;
							lsn =  logger->get_next_log_entry_non_atom(entry);
							if (entry == NULL)
								break;
						}
						else { 
							PAUSE //usleep(50);
							INC_INT_STATS(time_io, get_sys_clock() - tt);
							continue;
						}
					}

#if BYPASS_WORKER
					/// now we don't really add them into the pool to see the bare performance.
					INC_INT_STATS(num_commits, 1);
					continue;
#endif					

					INC_INT_STATS(time_io, get_sys_clock() - tt);
					// Format for serial logging
					// | checksum | size | ... |
					assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
					uint32_t size = *(uint32_t*)(entry + sizeof(uint32_t));
					// recover_txn(entry + sizeof(uint32_t) * 2);
					/*
					while(!ATOM_CAS(*mutex, 0, 1))
					{
						PAUSE
					}
					*/
					//printLV(pi.txnLV);
					//pool->push_back(pi);
		
					*maxLSN = lsn;
					while(
						pool[poolEnd % g_poolsize_wait].recovered == 1 || // someone is still using it
						poolEnd - poolStart >= g_poolsize_wait)
					{
						while(pool[poolStart % g_poolsize_wait].recovered == 0)
						{
							poolStart ++;
							if(poolStart == poolEnd)
								break;
						}
						if(poolEnd == poolStart)
							*log_manager->recoverLV[logger_id] = lsn - 1;
						else
							*log_manager->recoverLV[logger_id] = pool[poolStart % g_poolsize_wait].LSN[0] - 1;
					}
					poolItem &newpi = pool[poolEnd % g_poolsize_wait];
					//poolItem * pi = (poolItem*) _mm_malloc(sizeof(poolItem), ALIGN_SIZE);
					poolItem * pi = &newpi;
					//pi->txnData = (char*) _mm_malloc(size, ALIGN_SIZE);
					//pi->txnLV = (uint64_t*) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE);
					//pi->LSN = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
					// TODO: txnLV and LSN can be packed together later.
					memcpy(pi->txnData, entry, size);
					char * ptdentry = pi->txnData;
					#if COMPRESS_LSN_LOG
						// read metainfo
						if(ptdentry[0] == 0x7f)
						{
							// this is a PSN Flush
							memcpy(LVFence, ptdentry + sizeof(uint32_t) * 2, sizeof(uint64_t) * g_num_logger);
							INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger + sizeof(uint32_t) * 2);
						}
						else
						{
								// use LVFence to update T.LV
								memcpy(pi->txnLV, LVFence, sizeof(uint64_t) * g_num_logger);
								uint64_t psnCounter = *(uint64_t*)(ptdentry + size - sizeof(uint64_t));
								psnCounter &= 0xff; // extract only one byte
								for(uint i=1; i<=psnCounter; i++)
								{
									uint64_t psnToWrite = *(uint64_t*)(ptdentry + size - sizeof(uint64_t) - sizeof(uint64_t) * i);
									pi->txnLV[psnToWrite&((1<<5)-1)] = psnToWrite >> 5;
								}
								INC_INT_STATS(int_aux_bytes, psnCounter * sizeof(uint64_t) + 1);
						}
					#else
						// read meta_info
						uint64_t *LV_start = (uint64_t*)(ptdentry + size - sizeof(uint64_t) * g_num_logger);
						for(uint i=0; i<g_num_logger; i++)
						{
							pi->txnLV[i] = LV_start[i];
						}
						INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger);
					#endif
					//assert(pi.txnLV[logger_id] >= *maxLSN);
					*(pi->LSN) = lsn;
					//newpi = *pi;
					//newpi.txnLV = pi->txnLV;
					//newpi.txnData = pi->txnData;
					//newpi.LSN = pi->LSN;
					newpi.recovered = 1; // 1 means not recovered, 0 means recovered
					// the order is important
					newpi.latch = 0;
					poolEnd ++;
					/*
					pi->next = NULL;
					pi->latch = 1;
					poolsize ++;
					tail->next = pi;
					tail = pi;
					*/
					
					//tail->latch = 0;  // release to the workers
					//COMPILER_BARRIER
					// We do not need to update _gc_lsn because only I am reading the log
					// It's OK to use _next_lsn as the gc. 
					//log_manager->_logger[logger_id]->set_gc_lsn(lsn);
					//*mutex = 0;
					count ++;

					//printf("size=%d lsn=%ld\n", *(uint32_t*)(entry+4), lsn);
				}
					
				for(;poolEnd != poolStart;)
				{
					// help cleaning the pool
					while(pool[poolStart % g_poolsize_wait].recovered == 0)
					{
						poolStart ++;
						if(poolStart == poolEnd)
							break;
					}
					if(poolEnd == poolStart)
						break;
					*log_manager->recoverLV[logger_id] = pool[poolStart % g_poolsize_wait].LSN[0] - 1;
					
					PAUSE;
				}

				// We still need to update the recoverLV once more.
				*log_manager->recoverLV[logger_id] = UINT64_MAX; // so other guys won't worry about us!
				poolDone = true;
				std::stringstream temps;
				temps << "Logger " << GET_THD_ID << " finished with counter " << count << endl;
				cout << temps.str(); // atomic output
#else
// Taurus with Multiple SPSC
				if(GET_THD_ID ==0)
					cout << "Recovery Starts." << endl;
				uint64_t num_worker = g_thread_cnt / g_num_logger * 16;
				uint64_t next_worker = 0;
				uint32_t count = 0;
				//uint32_t size;
				// uint32_t logger_id = GET_THD_ID % g_num_logger;
				for(;;) {
					/*
					if(poolsize > g_poolsize_wait)
					{
						usleep(100);
						continue;
					}
					*/
					char * entry = default_entry;
					uint64_t tt = get_sys_clock();
					//COMPILER_BARRIER
					uint64_t lsn;
					uint32_t bytes;
#if ASYNC_IO
					bytes = logger->tryReadLog();
					total_log_data += bytes;
#endif
					lsn = logger->get_next_log_entry_non_atom(entry); //, size);
					COMPILER_BARRIER
					INC_INT_STATS(time_recover6, get_sys_clock() - tt);
					COMPILER_BARRIER
					if (entry == NULL) {
						// if the pool has too many txns, we will wait to reduce the searching cost of workers.
						// as well as their atomic waits.
#if RECOVERY_FULL_THR
						if (glob_manager->_workload->sim_done > 0)
							break;
#endif
						
						bytes = logger->tryReadLog(); // load more log into the buffer.
						//INC_INT_STATS(time_recover1, get_sys_clock() - tt_recover);
						total_log_data += bytes;
						if (logger->iseof()) {
							entry = default_entry;
							lsn = logger->get_next_log_entry_non_atom(entry); //, size);
							if (entry == NULL)
								break;
						}
						else { 
							//PAUSE //usleep(50);
							uint64_t t3 = get_sys_clock();
							INC_INT_STATS(time_recover3, t3 - tt);
							INC_INT_STATS(time_io,  t3 - tt);
							continue;
						}
					}
#if BYPASS_WORKER
					/// now we don't really add them into the pool to see the bare performance.
					INC_INT_STATS(num_commits, 1);
					continue;
#endif					
					uint64_t tt2 = get_sys_clock();
					INC_INT_STATS(time_recover4, tt2 - tt);
					INC_INT_STATS(time_io, tt2 - tt);
					// Format for serial logging
					// | checksum | size | ... |
// if compression is not on, entry must be 0xbeef.					
					//assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
#if DECODE_AT_WORKER
					// if this is a PLv item, we need to broadcast it to all the workers
					if(entry[0] == 0x7f)
					{
						for(uint32_t workerId=0; workerId<num_worker; workerId += 16)
						{
							while(SPSCPoolEnd[workerId] - SPSCPoolStart[workerId] >= g_poolsize_wait)
							{
								PAUSE
							}
							// this will not cause live-lock
							poolItem * pi = &SPSCPools[workerId/16][SPSCPoolEnd[workerId] % g_poolsize_wait]; //(poolItem*) _mm_malloc(sizeof(poolItem), ALIGN_SIZE);
							pi->oldp = entry;
							pi->rasterized = 0;
								
								//pi->size = size;
								//INC_INT_STATS(time_recover3, get_sys_clock() - tt3); 2.85/3.37
								//assert(pi.txnLV[logger_id] >= *maxLSN);
							*(pi->LSN) = lsn;
								
							//*maxLSN = lsn;  // get rid of maxLSN, which is accessed by all the workers.
								
							pi->recovered = 0;
							SPSCPoolEnd[workerId] ++;
						}
					}
					else // otherwise we add it into a worker's queue.
#endif
					for(;;)
					{
#if RECOVERY_FULL_THR				
						if (glob_manager->_workload->sim_done > 0)
								break;
#endif
#if ASYNC_IO
						bytes = logger->tryReadLog();
						total_log_data += bytes; // keep the DMA module busy
#endif
						uint64_t tt7 = get_sys_clock();
						// try next_worker;
						uint64_t workerId = next_worker % num_worker;
						if(SPSCPoolEnd[workerId] - SPSCPoolStart[workerId] < g_poolsize_wait)
						{
							poolItem * pi = &SPSCPools[workerId/16][SPSCPoolEnd[workerId] % g_poolsize_wait]; //(poolItem*) _mm_malloc(sizeof(poolItem), ALIGN_SIZE);
							// TODO: txnLV and LSN can be packed together.
							//uint64_t tt3 = get_sys_clock();
							//memcpy(pi->txnData, entry, size);
#if DECODE_AT_WORKER
							pi->oldp = entry;
							pi->rasterized = 0;
							
							//pi->size = size;
							//INC_INT_STATS(time_recover3, get_sys_clock() - tt3); 2.85/3.37
							//assert(pi.txnLV[logger_id] >= *maxLSN);
							*(pi->LSN) = lsn;
							
							//*maxLSN = lsn;  // get rid of maxLSN, which is accessed by all the workers.
							
							pi->recovered = 0;
							// 1 means recovered, 0 means not recovered: this is different from the SPMC version
							// the order is important
							// newpi.latch = 0;
#else
							assert(*(uint32_t*)entry == 0xbeef || entry[0] == 0x7f);
							poolItem * it = pi;
							*(pi->LSN) = lsn;
							it->size = *(uint32_t*)(entry + sizeof(uint32_t));
							memcpy(it->txnData, entry, it->size);
							//COMPILER_BARRIER  // rasterizedLSN must be updated after memcpy
							//assert(log_manager->_logger[realLogId]->rasterizedLSN[workerId][0] < it->LSN[0]);
							//logger->rasterizedLSN[workerId/16][0] = lsn;
							logger->rasterizedLSN[0][0] = lsn;
							it->starttime = get_sys_clock();
							char * ptdentry = it->txnData;
							
#if COMPRESS_LSN_LOG
								// read metainfo
								if(ptdentry[0] == 0x7f)
								{
									// this is a PSN Flush
									memcpy(LVFence, ptdentry + sizeof(uint32_t) * 2, sizeof(uint64_t) * g_num_logger);
									//it->recovered = 1;// No recover for PSN
									//it->rasterized = 1;
									INC_INT_STATS(int_aux_bytes, sizeof(uint64_t) * g_num_logger + sizeof(uint32_t) * 2);
									//continue;
									break; // this will not go to workers
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
											uint64_t lvElem = psnToWrite >> 5;
											uint64_t lvIndex = psnToWrite&((1<<5)-1);
											if(lvElem > log_manager->endLV[lvIndex][0])
											{
												glob_manager->_workload->sim_done = 1;
												logger->_eof = true;
												cout << "Stop due to an uncommitted transaction " << i << " " << it->txnLV[i] << endl;
												break;
											}
											it->txnLV[lvIndex] = lvElem;
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
								/*
								for(uint i=0; i<g_num_logger; i++)
								{
									if(it->txnLV[i] >= log_manager->endLV[i][0])
									{
										// we hit the end line of this log file.
										glob_manager->_workload->sim_done = 1;
										// stop the simulation
										logger->_eof = true;
										cout << "Stop due to an uncommitted transaction " << i << " " << it->txnLV[i] << endl;
									}
								} 
								*/
								INC_INT_STATS(num_log_entries, 1);
								it->rasterized = 1;
								it->recovered = 0;
#endif
							SPSCPoolEnd[workerId] ++;
							
							INC_INT_STATS(time_recover8, get_sys_clock() - tt7);
							next_worker += 16;
							break;
						}
						next_worker += 16;
						INC_INT_STATS(time_recover7, get_sys_clock() - tt7);
					}
					 //+= PRIMES[64]; // an arbitrary prime
					INC_INT_STATS(time_recover2, get_sys_clock() - tt2); // decode and push into the queue
					/*
					while(
						pool[poolEnd % g_poolsize_wait].recovered == 1 || // someone is still using it
						poolEnd - poolStart >= g_poolsize_wait)
					{
						while(pool[poolStart % g_poolsize_wait].recovered == 0)
						{
							poolStart ++;
							if(poolStart == poolEnd)
								break;
						}
						if(poolEnd == poolStart)
							*log_manager->recoverLV[logger_id] = pi->LSN[0] - 1;
						else
							*log_manager->recoverLV[logger_id] = pool[poolStart % g_poolsize_wait].LSN[0] - 1;
					}
					*/
					/*
					pi->next = NULL;
					pi->latch = 1;
					poolsize ++;
					tail->next = pi;
					tail = pi;
					*/
					
					//tail->latch = 0;  // release to the workers
					//COMPILER_BARRIER
					// We do not need to update _gc_lsn because only I am reading the log
					// It's OK to use _next_lsn as the gc. 
					//log_manager->_logger[logger_id]->set_gc_lsn(lsn);
					//*mutex = 0;
					count ++;
#if RECOVERY_FULL_THR
					if (glob_manager->_workload->sim_done > 0)
						break;
#endif
					//printf("size=%d lsn=%ld\n", *(uint32_t*)(entry+4), lsn);
				}

				// We still need to update the recoverLV once more.
				num_worker /= 16;
				poolDone = true;
				std::stringstream temps;
				temps << "Logger " << logger_id << " finished with counter " << count << ", now waiting..." << endl;
				cout << temps.str(); // atomic output
				// here we ignore a corner case that some worker might get 0 workload.
				// TODO: change the ending condition to get signals from the workers.
#if !BYPASS_WORKER
#if RECOVERY_FULL_THR
				while(!glob_manager->_workload->sim_done)
#else
				while(!(workerDone[0] == num_worker))
#endif
				{
					PAUSE
					//usleep(100);
				}
				maxLSN[0] = UINT64_MAX;
				printf("logger_id = %d, set recoverLVSPSC_min to be %lu\n", logger_id, *log_manager->recoverLVSPSC_min[logger_id]);
				*log_manager->recoverLVSPSC_min[logger_id] = UINT64_MAX;
				
				for(uint i=0; i< num_worker; i++)
				{
					*log_manager->recoverLVSPSC[logger_id][i] = UINT64_MAX;
				}
			#endif
				//std::stringstream temp2;
				//temp2 << workerDone._val << " Workers Done" << endl;
				//cout << temp2.str(); // atomic output
	//_mm_free(default_entry);
#endif
#else
		while (true) { //glob_manager->get_workload()->sim_done < g_thread_cnt) {
#if RECOVERY_FULL_THR	
			if(glob_manager->_workload->sim_done>0)
				break; // someone has finished.
#endif
			uint32_t bytes = logger->tryReadLog();
			total_log_data += bytes;
			if (logger->iseof())
				break;
			if (bytes == 0) 
			{
				usleep(100);
			}
		}
		//poolDone = true;
#endif
	
	} else {  // log
		//stats.init( _thd_id );
#if LOG_ALGORITHM == LOG_TAURUS
#if COMPRESS_LSN_LOG
		//uint32_t counter = 0;
#endif
#endif
		cout << "PSN Flush Frequency: " << g_psn_flush_freq << endl;
		while (glob_manager->get_workload()->sim_done < g_thread_cnt) {
			#if LOG_ALGORITHM == LOG_TAURUS
			uint32_t bytes = (uint32_t) log_manager->tryFlush(); // logger->tryFlush();
			#else
			uint32_t bytes = (uint32_t) logger->tryFlush();
			#endif
			total_log_data += bytes;
			if (bytes == 0) {
				/*
				if (g_no_flush)
				{
					usleep(100); // PAUSE; // usleep(100); // PAUSE; // reduce latency when simulation does not involve actual files.
				}
				else
				*/
				//usleep(100);
				PAUSE;
			}
			else
			{
				flushcount ++;
			}

#if LOG_ALGORITHM == LOG_TAURUS
			//counter++;
			// TODO: we can do evict here. From time to time the LSN grows
			/*if((counter) % EVICT_FREQ == 0 && ATOM_CAS(glob_manager->evictLatch, 0, 1))
			// TODO: need a random generator
			{
				LockTable & lt = LockTable::getInstance();
				lt.try_evict();
				glob_manager->evictLatch = 0;
			}
			*/
	  
	  #if COMPRESS_LSN_LOG
			/*if(g_psn_flush_freq > 0 && flushcount % g_psn_flush_freq == 0)
			{
				// write new psn;
				log_manager->flushPSN();
			}
			*/
			
	  #endif
	  
#endif

			// update epoch periodically. 
#if LOG_ALGORITHM == LOG_BATCH
				glob_manager->update_epoch();	
#endif
		}
		//cout << "logging counter " << counter << endl;
	}
	INC_INT_STATS(time_debug15, get_sys_clock() - starttime);
	//INC_INT_STATS(time_io, get_sys_clock() - starttime);
	INC_FLOAT_STATS(log_bytes, total_log_data);
	//INC_INT_STATS(int_debug10, flushcount);
	return FINISH;
}

#endif
