#pragma once 

enum StatsFloat {
	// Worker Thread
	STAT_run_time,
	STAT_log_bytes,
	STAT_log_dep_size,
	STAT_log_total_size,
	
	STAT_latency,


	// for logging recover 
	NUM_FLOAT_STATS
};

enum StatsInt {
	STAT_num_commits,
	STAT_num_aborts,
	// For logging
	STAT_num_aborts_logging,
	STAT_num_log_records,
	STAT_log_data,

	STAT_num_latency_count,
	// For Log Recovery
	STAT_num_raw_edges,
	STAT_num_waw_edges,
	STAT_num_war_edges,
	STAT_int_num_log,
	STAT_int_debug_get_next,
	STAT_int_debug1,
	STAT_int_debug2,
	STAT_int_debug3,
	STAT_int_debug4,
	STAT_int_debug5,
	STAT_int_debug6,
	STAT_int_debug7,
	STAT_int_debug8,
	STAT_int_debug9,
	STAT_int_debug10,
	STAT_int_psnflush,
	STAT_int_flush_time_interval,
	STAT_int_flush_half_full,
	STAT_int_num_get_row,
	STAT_int_locktable_volume,
	STAT_int_aux_bytes,
	STAT_num_log_entries,
	STAT_time_ts_alloc,
	STAT_time_man,
	STAT_time_cleanup,
	STAT_time_index,
	STAT_time_log,
	
	STAT_time_io,
	STAT_time_phase1_add_graph,
	STAT_time_recover_txn,

	STAT_time_phase1_1,
	STAT_time_phase1_2,
	STAT_time_phase2,
	STAT_time_phase3,
	
	STAT_time_phase1_1_raw,
	STAT_time_phase1_2_raw,
	STAT_time_phase2_raw,
	STAT_time_phase3_raw,

	STAT_time_recover_full,
	STAT_time_recover1,
	STAT_time_recover2,
	STAT_time_recover3,
	STAT_time_recover4,
	STAT_time_recover5,
	STAT_time_recover6,
	STAT_time_recover7,
	STAT_time_recover8,

	// debug stats
	STAT_time_debug_get_next,
	STAT_time_debug0,
	STAT_time_debug1,
	STAT_time_debug2,
	STAT_time_debug3,
	STAT_time_debug4,
	STAT_time_debug5,
	STAT_time_debug6,
	STAT_time_debug7,
	STAT_time_debug8,
	STAT_time_debug9,
	STAT_time_debug10,
	STAT_time_debug11,
	STAT_time_debug12,
	STAT_time_debug13,
	STAT_time_debug14,
	STAT_time_debug15,

	STAT_time_silo_validate1,
	STAT_time_silo_validate2,
	STAT_time_silo_validate3,
	STAT_time_silo_validate4,
	STAT_time_silo_validate5,
	STAT_time_silo_validate6,
	STAT_time_silo_validate7,
	STAT_time_silo_validate8,
	STAT_time_silo_validate9,

	STAT_time_locktable_get, // good
	STAT_time_locktable_get_validation,
	STAT_time_locktable_release, // good
	STAT_time_get_row_before, // good
	STAT_time_get_row_after, // good
	STAT_time_log_create,
	STAT_time_log_serialLogTxn,
	STAT_time_cleanup_1,
	STAT_time_cleanup_2,

	STAT_time_insideSLT1,
	STAT_time_insideSLT2,
	STAT_time_STLother,

    // verification

    STAT_time_prepareTraces,
    STAT_time_copyTraces,
    STAT_time_proverInit,
    STAT_time_proverFinishBatch,
    STAT_time_proverProcessTxn, // include finishbatch
    STAT_time_proverProcessBatches, 
    STAT_time_generateConstraintSystem,
    STAT_time_generateKey,
    STAT_time_generateProof,
    STAT_time_verify,
    STAT_time_outputProof,

    STAT_time_dispatcher_latency,
    STAT_int_dispatcher_latency_num,
    
    STAT_time_cc_latency,
    STAT_int_cc_latency_num,

    STAT_time_latency_verify,
    STAT_int_latency_num,

    STAT_int_comm_cost,

    STAT_int_batch_num,

	STAT_time_merkle_init,
	STAT_time_merkle_prove,

	NUM_INT_STATS
};

class Stats_thd {
public:
	Stats_thd();
	void copy_from(Stats_thd * stats_thd);

	void init(uint64_t thd_id);
	void clear();
	
	//double _float_stats[NUM_FLOAT_STATS];
	//uint64_t _int_stats[NUM_INT_STATS];
	double * _float_stats;
	uint64_t * _int_stats;
};

class Stats_tmp {
public:
	void init();
	void clear();
	double time_man;
	double time_index;
	double time_wait;
	char _pad[CL_SIZE - sizeof(double)*3];
};

class Stats {
public:
	Stats();
	// PER THREAD statistics
	Stats_thd ** _stats;
	// stats are first written to tmp_stats, if the txn successfully commits, 
	// copy the values in tmp_stats to _stats
	Stats_tmp ** tmp_stats;
	
	// GLOBAL statistics
	double dl_detect_time;
	double dl_wait_time;
	uint64_t cycle_detect;
	uint64_t deadlock;	
	
	// output thread	
	uint64_t bytes_sent;
	uint64_t bytes_recv;

//    double last_cp_bytes_sent(double &dummy_bytes);
	void init();
	void clear(uint64_t tid);
	void print();
	void print_lat_distr();
	
//	void checkpoint();
//	void copy_from(Stats * stats);
		
	void output(std::ostream * os); 
	
	std::string statsFloatName[NUM_FLOAT_STATS] = {
		// worker thread
		"run_time",
		"log_bytes",
		"log_dep_size",
		"log_total_size",
	
		"latency",
	};

	std::string statsIntName[NUM_INT_STATS] = {
		"num_commits",
		"num_aborts",
		// For logging
		"num_aborts_logging",
		"num_log_records",
		"log_data",
		"num_latency_count",

		// For Log Recovery
		"num_raw_edges",
		"num_waw_edges",
		"num_war_edges",
		"int_num_log",
	
		"int_debug_get_next",
		"int_debug1",
		"int_debug2",
		"int_debug3",
		"int_debug4",
		"int_debug5",
		"int_debug6",
		"int_debug7",
		"int_debug8",
		"int_debug9",
		"int_debug10",
		"int_psnflush",
		"int_flush_time_interval",
		"int_flush_half_full",
		"int_num_get_row",
		"int_locktable_volume",
		"int_aux_bytes",
		"num_log_entries",
		"time_ts_alloc",
		"time_man",
		"time_cleanup",
		"time_index",
		"time_log",
		"time_io",
		"time_phase1_add_graph",
		"time_recover_txn",

		"time_phase1_1",
		"time_phase1_2",
		"time_phase2",
		"time_phase3",
		
		"time_phase1_1_raw",
		"time_phase1_2_raw",
		"time_phase2_raw",
		"time_phase3_raw",

		"time_recover_full",
		"time_recover1",
		"time_recover2",
		"time_recover3",
		"time_recover4",
		"time_recover5",
		"time_recover6",
		"time_recover7",
		"time_recover8",

		// debug stats
		"time_debug_get_next",
		"time_debug0",
		"time_debug1",
		"time_debug2",
		"time_debug3",
		"time_debug4",
		"time_debug5",
		"time_debug6",
		"time_debug7",
		"time_debug8",
		"time_debug9",
		"time_debug10",
		"time_debug11",
		"time_debug12",
		"time_debug13",
		"time_debug14",
		"time_debug15",

		"time_silo_validate1",
		"time_silo_validate2",
		"time_silo_validate3",
		"time_silo_validate4",
		"time_silo_validate5",
		"time_silo_validate6",
		"time_silo_validate7",
		"time_silo_validate8",
		"time_silo_validate9",

		"time_locktable_get", // good
		"time_locktable_get_validation",
		"time_locktable_release", // good
		"time_get_row_before", // good
		"time_get_row_after", // good
		"time_log_create",
		"time_log_serialLogTxn",
		"time_cleanup_1",
		"time_cleanup_2",
		"time_insideSLT1",
		"time_insideSLT2",
		"time_STLother",

        // verification

        "time_prepareTraces",
        "time_copyTraces",
        "time_proverInit",
        "time_proverFinishBatch",
        "time_proverProcessTxn", // include finishbatch
        "time_proverProcessBatches", 
        "time_generateConstraintSystem",
        "time_generateKey",
        "time_generateProof",
        "time_verify",
        "time_outputProof",

        "time_dispatcher_latency",
        "int_dispatcher_latency_num",

        "time_cc_latency",
        "int_cc_latency_num",

        "time_latency_verify",
        "int_latency_num",

        "int_comm_cost",
        "int_batch_num",
		
		"time_merkle_init",
		"time_merkle_prove",
	};
//private:
	uint32_t _total_thread_cnt;
	//vector<double> _aggregate_latency;
	//vector<Stats *> _checkpoints;
    //uint32_t        _num_cp;
};
