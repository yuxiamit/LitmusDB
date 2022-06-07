#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include "global.h"
#include "helper.h"
#include "stats.h"
#include "mem_alloc.h"
#include <inttypes.h>
#include <iomanip>

#define BILLION 1000000000UL

#ifndef PRIu64
#define PRIu64 "ld"
#endif

Stats_thd::Stats_thd()
{
	_float_stats = (double *) _mm_malloc(sizeof(double) * NUM_FLOAT_STATS, ALIGN_SIZE);
	_int_stats = (uint64_t *) _mm_malloc(sizeof(uint64_t) * NUM_INT_STATS, ALIGN_SIZE);
	
	clear();
}

void Stats_thd::init(uint64_t thd_id) {
	clear();
}

void Stats_thd::clear() {
	for (uint32_t i = 0; i < NUM_FLOAT_STATS; i++)
		_float_stats[i] = 0;
	for (uint32_t i = 0; i < NUM_INT_STATS; i++)
		_int_stats[i] = 0;
}

void 
Stats_thd::copy_from(Stats_thd * stats_thd)
{
	memcpy(_float_stats, stats_thd->_float_stats, sizeof(double) * NUM_FLOAT_STATS);
	memcpy(_int_stats, stats_thd->_int_stats, sizeof(double) * NUM_INT_STATS);
}

void Stats_tmp::init() {
	clear();
}

void Stats_tmp::clear() {	
}

////////////////////////////////////////////////
// class Stats
////////////////////////////////////////////////
Stats::Stats()
{}

void Stats::init() {
	if (!STATS_ENABLE) 
		return;
    //_num_cp = 0;
	_total_thread_cnt = g_thread_cnt + g_num_logger + g_prover_threads;
	_stats = new Stats_thd * [_total_thread_cnt];
	for (uint32_t i = 0; i < _total_thread_cnt; i++) {
		_stats[i] = (Stats_thd *) _mm_malloc(sizeof(Stats_thd), ALIGN_SIZE);
		new(_stats[i]) Stats_thd();
	}
	//_stats = (Stats_thd**) _mm_malloc(sizeof(Stats_thd*) * _total_thread_cnt, ALIGN_SIZE);
}

void Stats::clear(uint64_t tid) {
	if (STATS_ENABLE) {
		_stats[tid]->clear();
		tmp_stats[tid]->clear();
	}
}

void Stats::output(std::ostream * os) 
{
	std::ostream &out = *os;

/* if (g_warmup_time > 0) {
		// subtract the stats in the warmup period
		uint32_t cp = int(1000 * g_warmup_time / STATS_CP_INTERVAL) - 1;
		Stats * base = _checkpoints[cp];
//		for (cp=0; cp<5; cp++)
//		printf("cp=%d. commits=%ld\n", cp, _checkpoints[cp]->_stats[0]->_int_stats[0]);
		for (uint32_t i = 0; i < _total_thread_cnt; i++) {
			for	(uint32_t n = 0; n < NUM_FLOAT_STATS; n++) 
				_stats[i]->_float_stats[n] -= base->_stats[i]->_float_stats[n];
			if (i < g_num_worker_threads)
				_stats[i]->_float_stats[STAT_run_time] = g_run_time * BILLION;
			for	(uint32_t n = 0; n < NUM_INT_STATS; n++) 
				_stats[i]->_int_stats[n] -= base->_stats[i]->_int_stats[n];

			for (uint32_t n = 0; n < Message::NUM_MSG_TYPES; n++) {
				_stats[i]->_msg_count[n] -= base->_stats[i]->_msg_count[n];
				_stats[i]->_msg_size[n] -= base->_stats[i]->_msg_size[n];
				_stats[i]->_msg_committed_count[n] -= base->_stats[i]->_msg_committed_count[n];
				_stats[i]->_msg_committed_size[n] -= base->_stats[i]->_msg_committed_size[n];
			}
		}
	}
*/
	uint64_t total_num_commits = 0;
	double total_run_time = 0;
	double max_run_time = 0;




	for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++) { 
		total_num_commits += _stats[tid]->_int_stats[STAT_num_commits];
		_stats[tid]->_float_stats[STAT_run_time] /= CPU_FREQ;
		// because we are using the raw rdtsc
		total_run_time += _stats[tid]->_float_stats[STAT_run_time];
		if(_stats[tid]->_float_stats[STAT_run_time] > max_run_time)
			max_run_time = _stats[tid]->_float_stats[STAT_run_time];
	}

/*
#if LOG_ALGORITHM == LOG_SERIAL

	if(g_log_recover)
	{
		// we only count the first recovering thread
		total_num_commits = _stats[0]->_int_stats[STAT_num_commits];
		total_run_time = _stats[0]->_float_stats[STAT_run_time];
		max_run_time = _stats[0]->_float_stats[STAT_run_time];
	}

#endif
*/


	//assert(total_num_commits > 0);
	out << "=Worker Thread=" << endl;

#if LOG_ALGORITHM == LOG_SERIAL
	if(g_log_recover)
	{
		out << "    " << setw(30) << left << "Throughput:"
		<< BILLION * _stats[0]->_int_stats[STAT_num_commits] / _stats[0]->_float_stats[STAT_run_time] << endl; // we only count the first thread in recovery
	}
	else
#endif
	out << "    " << setw(30) << left << "Throughput:"
		<< BILLION * total_num_commits / total_run_time * g_thread_cnt << endl;
	out << "    " << setw(30) << left << "MaxThr:"
		<< BILLION * total_num_commits / max_run_time << endl;
	// print floating point stats
	for	(uint32_t i = 0; i < NUM_FLOAT_STATS; i++) {
		double total = 0;
		for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++) 
		{
			total += _stats[tid]->_float_stats[i];
		}
		//if (i == STAT_latency)
		//	total /= total_num_commits;
		string suffix = "";
		out << "    " << setw(30) << left << statsFloatName[i] + suffix + ':' << total / BILLION;
		out << " (";
		for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++) {
			out << _stats[tid]->_float_stats[i] / BILLION << ',';
		}
		out << ')' << endl; 
	}

	out << endl;

#if COLLECT_LATENCY
	double avg_latency = 0;
	for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++)
		avg_latency += _stats[tid]->_float_stats[STAT_txn_latency];
	avg_latency /= total_num_commits;

	out << "    " << setw(30) << left << "average_latency:" << avg_latency / BILLION << endl;
	// print latency distribution
	out << "    " << setw(30) << left << "90%_latency:" 
		<< _aggregate_latency[(uint64_t)(total_num_commits * 0.90)] / BILLION << endl;
	out << "    " << setw(30) << left << "95%_latency:" 
		<< _aggregate_latency[(uint64_t)(total_num_commits * 0.95)] / BILLION << endl;
	out << "    " << setw(30) << left << "99%_latency:" 
		<< _aggregate_latency[(uint64_t)(total_num_commits * 0.99)] / BILLION << endl;
	out << "    " << setw(30) << left << "max_latency:" 
		<< _aggregate_latency[total_num_commits - 1] / BILLION << endl;

	out << endl;
#endif
	// print integer stats
	for	(uint32_t i = 0; i < NUM_INT_STATS; i++) {
		double total = 0;
		for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++) {
			total += _stats[tid]->_int_stats[i];
		}
		if(statsIntName[i].substr(0, 4) == "time")
		{
			out << "    " << setw(30) << left << statsIntName[i] + ':'<< (double)total / CPU_FREQ / BILLION; 
			cout << " " << (double)total / CPU_FREQ / total_run_time * 100.0 << "%";
			cout << " " << (double)total / CPU_FREQ / total_num_commits;
			out << " (";
			for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++)
				out << (double)_stats[tid]->_int_stats[i] / CPU_FREQ / BILLION << ',';
			out << ')' << endl; 
		}
		else
		{
			out << "    " << setw(30) << left << statsIntName[i] + ':'<< total; 
			out << " (";
			for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++)
				out << _stats[tid]->_int_stats[i] << ',';
			out << ')' << endl; 
		}
	}
	/*
	for(uint32_t i=0; i<NUM_INT_STATS; i++)
	{
		for (uint32_t tid=0; tid < _total_thread_cnt; tid++)
			if(statsIntName[i].substr(0, 4) == "time_")
				_stats[tid]->_float_stats[i] /= CPU_FREQ;
	}
	*/

}

void Stats::print() 
{
	ofstream file;
	bool write_to_file = false;
	if (output_file != NULL) {
		write_to_file = true;
		file.open (output_file);
	}
	// compute the latency distribution
#if COLLECT_LATENCY
	for (uint32_t tid = 0; tid < _total_thread_cnt; tid ++) { 
		M_ASSERT(_stats[tid]->all_latency.size() == _stats[tid]->_int_stats[STAT_num_commits], 
				 "%ld vs. %ld\n", 
				 _stats[tid]->all_latency.size(), _stats[tid]->_int_stats[STAT_num_commits]);
		// TODO. should exclude txns during the warmup
		_aggregate_latency.insert(_aggregate_latency.end(), 
								 _stats[tid]->all_latency.begin(),
								 _stats[tid]->all_latency.end());
	}
	std::sort(_aggregate_latency.begin(), _aggregate_latency.end());
#endif
	output(&cout);
	if (write_to_file) {
		std::ofstream fout (output_file);
		output(&fout);
		fout.close();
	}

	return;
}

void Stats::print_lat_distr() {
}
