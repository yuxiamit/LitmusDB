#include "global.h"
#include "helper.h"
#include "ycsb.h"
#include "tpcc.h"
#include "thread.h"
#include "logging_thread.h"
#include "manager.h"
#include "mem_alloc.h"
#include "query.h"
#include "plock.h"
#include "occ.h"
#include "vll.h"
#include "log.h"
#include "serial_log.h"
#include "parallel_log.h"
#include "taurus_log.h"
#include "locktable.h"
#include "log_pending_table.h"
#include "log_recover_table.h"
#include "free_queue.h"
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "jvm-bridge.h"
#include "litmus-prover.h"

void * f(void *);
void * f_log(void *);

thread_t ** m_thds;
LoggingThread ** logging_thds;

// defined in parser.cpp
void parser(int argc, char * argv[]);

void handler (int sig) {
	void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);

  raise (SIGABRT); // cause a core dump.
	//exit(1);
}

int main(int argc, char* argv[])
{
	//signal(SIGBUS, handler);   // install our handler
	uint64_t mainstart = get_sys_clock();
	

	string dir;
	char hostname[256];
	gethostname(hostname, 256);
	if (strncmp(hostname, "draco", 5) == 0)
		dir = "./";
	if (strncmp(hostname, "yx", 2) == 0)
	{
		g_max_txns_per_thread = 100;
		cout << "[!] Detected desktop. Entering low disk-usage mode... " << endl;
	}
  	parser(argc, argv);
#if VERIFICATION && !SKIP_MAT_GEN
/*
	g_jvm_init.options.push_back("-Djava.class.path=../pequin/compiler/frontend/bin");
	//g_jvm_init.options.push_back("-Xrunjdwp:transport=dt_socket,server=y,address=54321");
	g_jvm_init.options.push_back("-Xdebug");
	g_jvm_init.options.push_back("-Xmx14000M");
	g_jvm = g_jvm_init.init();
	if (!g_jvm.env) {return 0;}
	
	char cwdbuffer[50];//="PYTHONPATH=";
	
	getcwd(cwdbuffer, 50);

	setenv("PYTHONPATH", cwdbuffer, 1);

	Py_Initialize();
	cout << "Finished JVM and Python Intepreter initialization" << endl;
*/
#endif
	
	if(g_thread_cnt < g_num_logger) g_num_logger = g_thread_cnt;
	
	if(g_log_buffer_size % 512 != 0)
	{
		cout << "Bad log buffer size: " << g_log_buffer_size << endl;
		return 0;
	}

	cout << "Log Read Size: " << (uint64_t)(g_log_buffer_size * RECOVER_BUFFER_PERC) << endl;

#if LOG_ALGORITHM == LOG_SERIAL
	log_manager = new SerialLogManager;
	log_manager->init();
#elif LOG_ALGORITHM == LOG_TAURUS
	//string bench = (WORKLOAD == YCSB)? "YCSB" : "TPCC";
	log_manager = (TaurusLogManager*) _mm_malloc(sizeof(TaurusLogManager), 64); //new TaurusLogManager;
	new (log_manager) TaurusLogManager();
	log_manager->init();
#elif LOG_ALGORITHM == LOG_PARALLEL || LOG_ALGORITHM == LOG_BATCH
	string bench = (WORKLOAD == YCSB)? "YCSB" : "TPCC";
	log_manager = new LogManager * [g_num_logger];
	string type = (LOG_ALGORITHM == LOG_PARALLEL)? "P" : "B";
	if(LOG_ALGORITHM == LOG_TAURUS) type = "t";
	for (uint32_t i = 0; i < g_num_logger; i ++) {
		if (strncmp(hostname, "istc3", 5) == 0) {
			if (i == 0)
				dir = "/f0/yuxia/";
			else if (i == 1)
				dir = "/f1/yuxia/";
			else if (i == 2)
				dir = "/f2/yuxia/";
			else if (i == 3)
				dir = "/data/yuxia/";
		}
		if (strncmp(hostname, "ip-", 3) == 0) { // EC2
			if (i == 0)
				dir = "/data0/";
			else if (i == 1)
				dir = "/data1/";
			else if (i == 2)
				dir = "/data2/";
			else if (i == 3)
				dir = "/data3/";
			else if (i == 4)
				dir = "/data4/";
			else if (i == 5)
				dir = "/data5/";
			else if (i == 6)
				dir = "/data6/";
			else if (i == 7)
				dir = "/data7/";
		} 
		log_manager[i] = (LogManager *) _mm_malloc(sizeof(LogManager), ALIGN_SIZE);
		new(log_manager[i]) LogManager(i);
		#if LOG_TYPE == LOG_DATA
		log_manager[i]->init(dir + type + "D_log" + to_string(i) + "_" + to_string(g_num_logger) + "_" + bench + ".log");
		#else
		log_manager[i]->init(dir + type + "C_log" + to_string(i) + "_" + to_string(g_num_logger) + "_" + bench + ".log");
		#endif
	}
	
  #if LOG_ALGORITHM == LOG_PARALLEL
	if (g_log_recover) 
		MALLOC_CONSTRUCTOR(LogRecoverTable, log_recover_table);
  #endif
#endif
	next_log_file_epoch = new uint32_t * [g_num_logger];
	for (uint32_t i = 0; i < g_num_logger; i ++) {
		next_log_file_epoch[i] = (uint32_t *) _mm_malloc(sizeof(uint32_t), ALIGN_SIZE);
	}
	mem_allocator.init(g_part_cnt, MEM_SIZE / g_part_cnt); 
#if USE_LOCKTABLE
	LockTable::getInstance();  // initialize the lock table singleton
	//LockTable::printLockTable();
#endif
	stats = new Stats();
	stats->init();
	glob_manager = (Manager *) _mm_malloc(sizeof(Manager), ALIGN_SIZE);
	new(glob_manager) Manager();
	glob_manager->init();
#if CC_ALG == DL_DETECT
		dl_detector.init();
#endif
	printf("mem_allocator initialized!\n");
	workload * m_wl;
	switch (WORKLOAD) {
		case YCSB :
			m_wl = new ycsb_wl; break;
		case TPCC :
			m_wl = new tpcc_wl; break;
		case TEST :
            assert(false);
			break;
		default:
			assert(false);
	}
	m_wl->init();
	printf("workload initialized!\n");
	glob_manager->set_workload(m_wl);
	assert(GET_WORKLOAD->sim_done == 0);

	uint64_t thd_cnt = g_thread_cnt;
	pthread_t p_thds[thd_cnt - 1];
	pthread_t p_logs[g_num_logger];

	m_thds = new thread_t * [thd_cnt];
	logging_thds = (LoggingThread **) _mm_malloc(sizeof(LoggingThread*) * g_num_logger, ALIGN_SIZE);

	for (uint32_t i = 0; i < thd_cnt; i++) 
	{
		m_thds[i] = (thread_t *) _mm_malloc(sizeof(thread_t), ALIGN_SIZE);
		new(m_thds[i]) thread_t();
	}
	for (uint32_t i = 0; i < g_num_logger; i++)  
	{
		logging_thds[i] = (LoggingThread *) _mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
		new(logging_thds[i]) LoggingThread();
	}
	// query_queue should be the last one to be initialized!!!
	// because it collects txn latency
	if (!g_log_recover) {
		query_queue = (Query_queue *) _mm_malloc(sizeof(Query_queue), ALIGN_SIZE);
		query_queue->init(m_wl);
		printf("query_queue initialized!\n");
	}
	pthread_barrier_init( &warmup_bar, NULL, g_thread_cnt );
	pthread_barrier_init( &worker_bar, NULL, g_thread_cnt );
	pthread_barrier_init( &log_bar, NULL, g_num_logger );
#if CC_ALG == HSTORE
	part_lock_man.init();
#elif CC_ALG == OCC
	occ_man.init();
#elif CC_ALG == VLL
	vll_man.init();
#endif

	for (uint32_t i = 0; i < thd_cnt; i++) { 
		m_thds[i]->init(i, m_wl);
	}
#if LOG_ALGORITHM != LOG_NO
	for (uint32_t i = 0; i < g_num_logger; i++)
		logging_thds[i]->set_thd_id(i);
#endif

	if (WARMUP > 0){
		printf("WARMUP start!\n");
		for (uint32_t i = 0; i < thd_cnt - 1; i++) {
			uint64_t vid = i;
			pthread_create(&p_thds[i], NULL, f, (void *)vid);
		}
		f((void *)(thd_cnt - 1));
		for (uint32_t i = 0; i < thd_cnt - 1; i++)
			pthread_join(p_thds[i], NULL);
		printf("WARMUP finished!\n");
	}
	warmup_finish = true;
	pthread_barrier_init( &warmup_bar, NULL, g_thread_cnt);

	// spawn and run txns again.
	int64_t starttime = get_server_clock();
	if(g_log_recover)
	{
		// change the order of threads.
		assert (LOG_ALGORITHM != LOG_NO);
		for (uint32_t i = 0; i < g_num_logger; i++) {
			uint64_t vid = i;
			pthread_create(&p_logs[i], NULL, f_log, (void *)vid);
		}
		for (uint32_t i = 0; i < thd_cnt - 1; i++) {
			uint64_t vid = i;
			pthread_create(&p_thds[i], NULL, f, (void *)vid);
		}
	}
	else
	{
		for (uint32_t i = 0; i < thd_cnt - 1; i++) {
			uint64_t vid = i;
			pthread_create(&p_thds[i], NULL, f, (void *)vid);
		}
		if (LOG_ALGORITHM != LOG_NO) // && !g_log_recover)
			for (uint32_t i = 0; i < g_num_logger; i++) {
				uint64_t vid = i;
				pthread_create(&p_logs[i], NULL, f_log, (void *)vid);
			}
	}
	f((void *)(thd_cnt - 1));
	
	for (uint32_t i = 0; i < thd_cnt - 1; i++) 
		pthread_join(p_thds[i], NULL);
	if (LOG_ALGORITHM != LOG_NO) // && !g_log_recover)
		for (uint32_t i = 0; i < g_num_logger; i++) 
			pthread_join(p_logs[i], NULL);

#if VERIFICATION
    // start verification
	uint64_t prover_start_time = get_server_clock();
    //parseTraces();
    //proveAll();

	#if CLIENT_INTERACT
    #if MEM_INTEGRITY == RSA_AD
	proveInteractive();
    #else
    //proveInteractiveMerkleTree();
	proveInteractiveMerkleTreeNative();
    #endif
	#else
	proveHandWritten();
	#endif
	uint64_t prover_time = get_server_clock() - prover_start_time;
	cout << "Prover time = " << prover_time / CPU_FREQ << endl;
#endif

#if ELLE_OUTPUT
		outputElle();
#endif

	int64_t endtime = get_server_clock();
	cout << "PASS! SimTime = " << (endtime - starttime) / CPU_FREQ << endl;

#if LOG_ALGORITHM == LOG_PARALLEL || LOG_ALGORITHM == LOG_BATCH
	for (uint32_t i = 0; i < g_num_logger; i ++)
		delete log_manager[i];
#elif LOG_ALGORITHM == LOG_TAURUS
	delete log_manager;
#elif LOG_ALGORITHM == LOG_SERIAL
	delete log_manager;
#endif
	if (STATS_ENABLE) {
		stats->print();
	}
#if LOG_ALGORITHM == LOG_PARALLEL
	if (g_log_recover)
		log_recover_table->check_all_recovered();
#endif

//#if VERIFICATION
		uint64_t total_time = get_sys_clock() - mainstart;
		uint64_t total_num_commits = 0;

		assert(STATS_ENABLE);

		for (uint32_t tid = 0; tid < stats->_total_thread_cnt; tid ++) { 
			total_num_commits += stats->_stats[tid]->_int_stats[STAT_num_commits];
		}



#if !VERFICATION
        g_verification_txn_count = total_num_commits;
#endif
		cout << "total commits: " << total_num_commits << endl;
		cout << "Full-Span-Thr: " << g_verification_txn_count / (float(total_time) / 1e9 / CPU_FREQ) << endl;
		cout << "Verify Txn: " << g_verification_txn_count << endl;
		cout << "Total time measured " << float(total_time) / 1e9 / CPU_FREQ << endl; // for CPU_FREQ calibration

        ////// latency
        uint64_t cc_lat = 0, dispatcher_latency = 0, verify_lat = 0, d_count=0, v_count = 0;
        const auto billion = 1000000000UL;
        for (uint32_t tid = 0; tid < stats->_total_thread_cnt; tid ++) { 
			cc_lat += (stats->_stats[tid]->_int_stats[STAT_time_cc_latency]);
            dispatcher_latency += (stats->_stats[tid]->_int_stats[STAT_time_dispatcher_latency]);
            verify_lat += (stats->_stats[tid]->_int_stats[STAT_time_latency_verify]);
            d_count += (stats->_stats[tid]->_int_stats[STAT_int_dispatcher_latency_num]);
            v_count += (stats->_stats[tid]->_int_stats[STAT_int_latency_num]);
		}

        if (d_count==0) d_count = 1;
        if (v_count == 0) v_count = 1; //hack

        cout << "CC Lat: " << (double) cc_lat / CPU_FREQ / billion / total_num_commits << endl;
        cout << "D Lat: " << (double) dispatcher_latency / CPU_FREQ / billion / d_count << endl;
        cout << "V Lat: " << (double) verify_lat / CPU_FREQ / billion / v_count << endl;

//#endif

    return 0;
}

void * f(void * id) {
	uint64_t tid = (uint64_t)id;
	m_thds[tid]->run();
	return NULL;
}
void * f_log(void * id) {
#if LOG_ALGORITHM != LOG_NO
	uint64_t tid = (uint64_t)id;
	logging_thds[tid]->run();
#endif
	return NULL;
}
