#include "litmus-prover.h"
#include "helper.h"
#include "config.h"
#include "tpcc_const.h"

#include "txn.h"
#include "global.h"
#include <stdio.h>
#include "stdlib.h"
#include <iostream>
#include <fcntl.h>
#include <helper.h>
#include <fstream>
#include "logging_thread.h"
#include "serial_log.h"
#include "log.h"
#include "config.h"

//#include "index.h"
#include "index_hash.h"
#include "index_btree.h"
#include "manager.h"
#include "wl.h"
#include "table.h"
#include "tpcc.h"
#include "ycsb.h"
#include <vector>
#include <map>
#include <cstdlib>

#include "SHA256.h"

LoggingThread *logth;

#if VERIFICATION
uint64_t multi_scalar_coeff[2 * MS_LIMBS][2 * MS_LIMBS]; // c^i, c = [1 .. 2M - 1], i = [0 .. 2M - 1]
#endif

void *f_vec(void *id)
{
    logth->run();
    return NULL;
}

void outputElle() // output Elle format
{
    /*
        Sample:
(def h [{:type :ok, :value [[:append :x 1] [:r :y [1]]]}
           {:type :ok, :value [[:append :x 2] [:append :y 1]]}
           {:type :ok, :value [[:r :x [1 2]]]}])
    */

    ofstream fout("traces.clj");
    fout << "(require '[elle.list-append :as a])" << endl;
    fout << "(def h [" << endl;

    log_manager->_logger[0]->flushRestNClose();
    uint64_t numEntries = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        numEntries += stats->_stats[tid]->_int_stats[STAT_num_log_entries];
    }

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE // usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        // recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        // cout << "numkeys " << num_keys << endl;

        uint32_t wi = 0;
        uint32_t ri = 0;

        map<uint32_t, vector<uint32_t>> append_list;

        g_verification_txn_count++;

        fout << "{:type :ok :value [";

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;
#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl *wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item,
                0,
                GET_THD_ID);
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif
            if (append_list.find(key) == append_list.end())
            {
                append_list[key] = vector<uint32_t>();
            }

            if (accessType == RD)
            {
                fout << "[:r :x" << key << " [";
                for (int v : append_list[key])
                {
                    fout << v << " ";
                }
                fout << "]] ";
            }
            else
            {
                // append
                fout << "[:append :x" << key << " ";
                fout << val;
                fout << "]";
                append_list[key].push_back(val);
            }
        }
        fout << "]}" << endl;

        uint64_t t_verify = get_sys_clock();
        INC_INT_STATS(time_latency_verify, t_verify - tt2);
        INC_INT_STATS(int_latency_num, 1);
    }
    fout << "])" << endl;
    fout << "(pprint (time (a/check {:consistency-models [:serializable]} h)))" << endl;
    pthread_join(p_log, NULL);
}

#if VERIFICATION

#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>
#include <libsnark/common/default_types/r1cs_gg_ppzksnark_pp.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_gg_ppzksnark/r1cs_gg_ppzksnark.hpp>

#include <iostream>
#include <fstream>
#include <sstream>

#include <gmp.h>
//#include "libv/computation_p.h"

#include "common_defs.h"
#include "litmus-template.h"
#include "jvm-bridge.h"

#include "Python.h"
#include "litmus-gadgets.hpp"

#include <chrono>
#include <thread>

#include "merkle_tree_circuit.h"
#include <libsnark/gadgetlib1/gadgets/merkle_tree/merkle_tree_check_update_gadget.hpp>

// common gmp values
mpz_t mp_N;
mpz_t mp_POE_L;
mpz_t mp_POE_q;
mpz_t mp_POE_Q;
mpz_t mp_MSMOD;
mpz_t mp_G;
mpz_t mp_BN128_ORDER;

#if MEM_INTEGRITY == MERKLE_TREE

std::string hexToChar(const char c)
{
    switch (tolower(c))
    {
    case '0':
        return "0000";
    case '1':
        return "0001";
    case '2':
        return "0010";
    case '3':
        return "0011";
    case '4':
        return "0100";
    case '5':
        return "0101";
    case '6':
        return "0110";
    case '7':
        return "0111";
    case '8':
        return "1000";
    case '9':
        return "1001";
    case 'a':
        return "1010";
    case 'b':
        return "1011";
    case 'c':
        return "1100";
    case 'd':
        return "1101";
    case 'e':
        return "1110";
    case 'f':
        return "1111";
    }
}

libff::bit_vector hexToBin(std::string &str)
{
    libff::bit_vector res;
    for (auto item : str)
    {
        std::string hexItem = hexToChar(item);
        res.push_back(hexItem[0] == '1' ? true : false);
        res.push_back(hexItem[1] == '1' ? true : false);
        res.push_back(hexItem[2] == '1' ? true : false);
        res.push_back(hexItem[3] == '1' ? true : false);
    }
    return res;
}

template <typename HashT>
libff::bit_vector hash256(std::string str)
{
    libff::bit_vector operand;
    for (int i = 0; i < str.size(); i++)
    {
        char tmpc[5];
        sprintf(tmpc, "%x", str[i]);
        std::string tmps(tmpc);
        libff::bit_vector s = hexToBin(tmps);
        operand.insert(operand.end(), s.begin(), s.end());
    }
    // padding input
    size_t size = operand.size();
    char tmpc[20];
    sprintf(tmpc, "%x", size);
    std::string tmps(tmpc);
    libff::bit_vector s = hexToBin(tmps);
    operand.push_back(1);
    for (int i = size + 1; i < HashT::get_block_len() - s.size(); i++)
    {
        operand.push_back(0);
    }
    operand.insert(operand.end(), s.begin(), s.end());
    libff::bit_vector res = HashT::get_hash(operand);
    return res;
}

template <typename HashT>
void calcAllLevels(std::vector<std::vector<libff::bit_vector>> &levels, size_t level)
{
    // level 1 upper layer
    for (int i = level; i > 0; i--)
    {
        for (int j = 0; j < levels[i].size(); j += 2)
        {
            libff::bit_vector input = levels[i][j];
            input.insert(input.end(), levels[i][j + 1].begin(), levels[i][j + 1].end());
            levels[i - 1].push_back(HashT::get_hash(input));
        }
    }
}

#endif

// using namespace NTL;

std::chrono::milliseconds timespan(SIM_NET_LATENCY);

#define POS_TO_STR(p) PyString_AsString(PyTuple_GET_ITEM(res, p))

uint32_t litmus_lock = 0;
string PEPPER_DIR = "../pequin/pepper";
vector<uint32_t> *inputList;

const char *cHeader = "#include <stdint.h>\n#include <gmp.h>";
const char *cFunc = "void compute(struct In* input, struct Out* output){\nuint64_t localDigest=1;";

vector<pthread_t> verifyWorkers;

// EX_TYPE * HashPrimes;

char *cCode;

FieldT convertToField(mpz_class x)
{
    mpz_t xmpzt;
    mpz_init(xmpzt);
    mpz_mod(xmpzt, x.get_mpz_t(), mp_BN128_ORDER);
    return FieldT(xmpzt);
}

void xgcd(uint32_t a, uint32_t b, uint32_t &res_x, uint32_t &res_y)
{
    bool swapped = false;
    if (a < b)
    {
        swap(a, b);
        swapped = true;
    }
    uint32_t prevx = 1, x = 0, prevy = 0, y = 1;
    while (b > 0)
    {
        uint32_t q = a / b;
        uint32_t xt = x;
        x = prevx - q * x;
        prevx = xt;
        uint32_t yt = y;
        y = prevy - q * y;
        prevy = yt;
        uint32_t bt = b;
        b = a % b;
        a = bt;
    }
    res_x = prevx;
    res_y = prevy;
    if (swapped)
        swap(res_x, res_y);
}

void prove(txn_man *t)
{
    while (!ATOM_CAS(litmus_lock, 0, 1))
        PAUSE;
    ofstream myfile(PEPPER_DIR + "/prover_verifier_shared/ycsb_wrapped.inputs");
    for (uint32_t i = 0; i < t->row_cnt; i++)
    {
        if (t->accesses[i]->type == RD)
        {
            myfile << t->accesses[i]->orig_row << endl
                   << t->accesses[i]->orig_data << endl; // TODO: change this to meaningful real data
        }
    }
    myfile.flush();
    myfile.close();
    int ret = system(("cd " + PEPPER_DIR + " && ./bin/pepper_prover_ycsb_wrapped prove ycsb_wrapped.pkey ycsb_wrapped.inputs ycsb_wrapped.outputs ycsb_wrapped.proof").c_str());
    if (ret != 0)
    {
        printf("Error in proving\n");
    }
    litmus_lock = 0;
    return;
}

uint32_t ipow(uint32_t base, uint32_t exp)
{
    uint32_t result = 1;
    for (;;)
    {
        if (exp & 1)
            result = result * base % __LTM_N;
        exp >>= 1;
        if (!exp)
            break;
        base = base * base % __LTM_N;
    }
    return result;
}

template <class T>
T ipow_fp(T base, T exp)
{
    T result = 1;
    for (;;)
    {
        if (exp & 1)
            result = result * base % __LTM_N;
        exp = exp >> 1;
        if (exp == 0)
            break;
        base = base * base % __LTM_N;
    }
    return result;
}

void parseTraces()
{
    assert(false); // deprecated

#if false    
    assert(LOG_ALGORITHM == LOG_SERIAL);
    assert(WORKLOAD == YCSB);
    assert(LOG_TYPE == LOG_DATA);
    assert(g_num_logger == 1);
    // scan the serial log and generate the code
    /*
    //int fd_prog = open((PEPPER_DIR + "/apps/wrapped_transaction.c").c_str(),
    //                   O_TRUNC | O_WRONLY | O_CREAT, 0664);
    */
    int fd_input = open((PEPPER_DIR + "/prover_verifier_shared/wrapped_transaction.inputs").c_str(),
                        O_TRUNC | O_WRONLY | O_CREAT, 0664);

    log_manager->_logger[0]->flushRestNClose();
    uint64_t numEntries = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        numEntries += stats->_stats[tid]->_int_stats[STAT_num_log_entries];
    }

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    string defAddrRead;
    string defAddrWrite;
    string inputStructure;
    string txnContent;

    uint32_t memDigest = 1;
    uint32_t currentProd = 1;
    uint32_t memCounter = 0;

    vector<uint32_t> *accList = (vector<uint32_t> *)_mm_malloc(sizeof(vector<uint32_t>), ALIGN_SIZE);
    new (accList) vector<uint32_t>();

    vector<uint32_t> *prodList = (vector<uint32_t> *)_mm_malloc(sizeof(vector<uint32_t>), ALIGN_SIZE);
    new (prodList) vector<uint32_t>();

    inputList = (vector<uint32_t> *)_mm_malloc(sizeof(vector<uint32_t>), ALIGN_SIZE);
    new (inputList) vector<uint32_t>();

    map<uint32_t, uint32_t> *latestWrittenIndex = (map<uint32_t, uint32_t> *)_mm_malloc(sizeof(map<uint32_t, uint32_t>), ALIGN_SIZE);
    new (latestWrittenIndex) map<uint32_t, uint32_t>();

    map<uint32_t, uint32_t> &lwMap = *latestWrittenIndex;
    string accumulatedReadValCheck;
    bool firstElementInBatch = true;
    uint32_t prod_init_hash = 1;
    uint32_t lastAB = -1;
    uint32_t lastABInd = 1;
    uint32_t lastABRI = 1;
    uint32_t writeExpProd = 1;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE //usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        //recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        //cout << "numkeys " << num_keys << endl;
        string addrReadPerTxn;
        string addrWritePerTxn;
        string inputStructurePerTxn;
        string readValCheckPerTxn;
        string txnBody;
        string txnLogicPerTxn;
        string updateWritePerTxn;

        uint32_t wi = 0;
        uint32_t ri = 0;

        uint32_t generated_batch = 0;
        

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;

#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl * wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item, 
                0, 
                GET_THD_ID
            );
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif

            //row_t * row = ((row_t *)m_item->location);

            uint32_t v_addr = table_id * g_synth_table_size + key;
            if (accessType == RD)
            {
                //addrReadPerTxn += renderTemplateREAD(LM_addrRead, count, ri, v_addr);
                inputStructurePerTxn += renderTemplateREAD(LM_inputStructure, count, ri);

                // prepare the proofs
                inputList->push_back(val);

                if (lwMap.find(v_addr) == lwMap.end())
                {
#if CC_ALG == DETRESERVE
                    readValCheckPerTxn += renderTemplateREAD(LM_readValCheckInit, count, ri, v_addr);
#else
                    readValCheckPerTxn += renderTemplateREAD(LM_readValCheckInit + " && " + LM_readValCheckNonMemBatched, count, ri, v_addr);
#endif
                    prod_init_hash = prod_init_hash * H_addr(v_addr);
                    // initial value
                    inputList->push_back(0);
                    inputList->push_back(0);
                    inputList->push_back(0);
                    inputList->push_back(0);
                    lastAB = inputList->size();
                    lastABInd = count;
                    lastABRI = ri;
                    uint32_t A, B;
                    xgcd(currentProd, H_addr(v_addr), A, B);
                    inputList->push_back(A);
                    inputList->push_back(B);
                }
                else
                {
                    readValCheckPerTxn += renderTemplateREAD(LM_readValCheck, count, ri, v_addr);
                    // modified value
                    uint32_t ind = lwMap[v_addr];

                    inputList->push_back(accList->at(ind));
                    inputList->push_back(accList->at(ind + 1));

                    // TODO: fix this hack
                    if ( prodList->at(ind) == 0)  prodList->at(ind) = 1;
                    uint32_t prodfromInd = currentProd / prodList->at(ind);
                    inputList->push_back(prodfromInd);

                    inputList->push_back(ipow((uint32_t)__LTM_G, prodfromInd));

                    uint32_t A, B;
                    xgcd(prodfromInd, H_addr(v_addr), A, B);
                    inputList->push_back(A);
                    inputList->push_back(B);
                }
                ri++;
            }
            else //if(accessType == WR)
            {
                //cout << "Processing for a write " << accessType << endl;
                // prepare the circuit
                //addrWritePerTxn += renderTemplateWrite(LM_addrWrite, count, wi, v_addr);
                txnLogicPerTxn += renderTemplateWrite(LM_txnLogic, count, wi);
#if CC_ALG == DETRESERVE
                writeExpProd *= H(wi, v_addr);
#else
                updateWritePerTxn += renderTemplateWrite(LM_updateWrite, count, wi, v_addr);
#endif
                // update the digests on the server side
                val = H(v_addr, val);
                accList->push_back(memDigest);
                prodList->push_back(currentProd);
                memDigest = ipow(memDigest, val);
                currentProd = currentProd * val;

                lwMap[v_addr] = memCounter;

                memCounter++;
                wi++;
            }
        }

        bool samebatch = false;

#if CC_ALG == DETRESERVE
        // unpack the batch number
        uint32_t batch_num;
        UNPACK(log_entry, batch_num, offset);
        //cout << "batch num " << batch_num << endl;
        if(batch_num == generated_batch) // TODO: bug here about generated_batch
        {
            samebatch = true;
            accumulatedReadValCheck += "if (" + readValCheckPerTxn + ") ";
            if(firstElementInBatch)
            {
                txnBody = "{ACC_READ_VAL_CHECK} {\n" + renderTxnBody("", txnLogicPerTxn, "", count);
            }
            else
            {
                txnBody = renderTxnBody("", txnLogicPerTxn, "", count);
            }
            firstElementInBatch = false;
        }
        else 
        {
            txnContent += renderTemplateWriteH2(LM_updateWrite, writeExpProd);
            txnContent += "txnpass = 1;} allpass *= txnpass;\n";
            cout << prod_init_hash << endl;
            if(prod_init_hash>1)
            {
                uint32_t A, B;
                xgcd(currentProd, prod_init_hash, A, B);
                (*inputList)[lastAB] = A;
                (*inputList)[lastAB+1] = B;
                accumulatedReadValCheck += " if (" + renderTemplateREAD(LM_readValCheckNonMemBatched, lastABInd, lastABRI, prod_init_hash) + ")";
            }
            replaceOnce(txnContent, "{ACC_READ_VAL_CHECK}",  accumulatedReadValCheck);
            accumulatedReadValCheck = "";
            firstElementInBatch = true;
            prod_init_hash = 1;
            generated_batch = batch_num;
        }
#endif
        if(!samebatch)
            txnBody = renderTxnBody(readValCheckPerTxn, txnLogicPerTxn, updateWritePerTxn, count);
        //defAddrRead += addrReadPerTxn;
        //defAddrWrite += addrWritePerTxn;
        inputStructure += inputStructurePerTxn;
        txnContent += txnBody;

        //printf("size=%d lsn=%ld\n", *(uint32_t*)(entry+4), lsn);
        COMPILER_BARRIER
        //INC_INT_STATS(time_recover_txn, get_sys_clock() - tt2);
        log_manager->_logger[0]->set_gc_lsn(lsn);
        INC_INT_STATS(num_commits, 1);
        count++;
    }
#if CC_ALG == DETRESERVE
    txnContent += renderTemplateWriteH2(LM_updateWrite, writeExpProd);
    txnContent += "txnpass = 1;} allpass *= txnpass; txnpass=0;\n";
    if(prod_init_hash>1)
            {
                uint32_t A, B;
                xgcd(currentProd, prod_init_hash, A, B);
                (*inputList)[lastAB] = A;
                (*inputList)[lastAB+1] = B;
                accumulatedReadValCheck += " if (" + renderTemplateREAD(LM_readValCheckNonMemBatched, lastABInd, lastABRI, prod_init_hash) + ")";
            }
    replaceOnce(txnContent, "{ACC_READ_VAL_CHECK}",  accumulatedReadValCheck);

#endif

    const char *prog = renderWrappedTxn( //defAddrRead, defAddrWrite,
                           inputStructure, txnContent)
                           .c_str();
    size_t progLen = strlen(prog);
    cCode = (char*) _mm_malloc(progLen, ALIGN_SIZE);
    memcpy(cCode, prog, progLen + 1); // the trailing '\0'
    //uint32_t bytes = write(fd_prog, prog, progLen);
    //assert(bytes == progLen);

    char buffer[16];
    for (std::vector<uint32_t>::iterator it = inputList->begin(); it != inputList->end(); it++)
    {
        sprintf(buffer, "%d\n", *it);
        uint32_t bytes = write(fd_input, buffer, strlen(buffer));
        assert(bytes == strlen(buffer));
    }
    // write the inputs

    cout << "Generated Input Length: " << inputList->size() << endl;

    _mm_free(accList);
    _mm_free(prodList);
    
    _mm_free(latestWrittenIndex);
    pthread_join(p_log, NULL);
    fsync(fd_input);
    //close(fd_prog);
    close(fd_input);
#endif
}

void javabridge()
{
    assert(false); // unimplemented
    // java is not used any more in this project.
}

void run_setup(int num_constraints, int num_inputs,
               int num_outputs, int num_vars, mpz_t p,
               string vkey_file, string pkey_file,
               string unprocessed_vkey_file, string AmatStr, string BmatStr, string CmatStr, std::stringstream &vkey, std::stringstream &pkey)
{

    cout << "Matrix sizes " << AmatStr.size() << " " << BmatStr.size() << " " << CmatStr.size() << endl;

    std::stringstream Amat(AmatStr);
    std::stringstream Bmat(BmatStr);
    std::stringstream Cmat(CmatStr);

    libsnark::default_r1cs_gg_ppzksnark_pp::init_public_params();
    libsnark::r1cs_constraint_system<FieldT> q;

    int Ai, Aj, Bi, Bj, Ci, Cj;
    mpz_t Acoef, Bcoef, Ccoef;
    mpz_init(Acoef);
    mpz_init(Bcoef);
    mpz_init(Ccoef);

    Amat >> Ai;
    Amat >> Aj;
    Amat >> Acoef;

    if (mpz_cmpabs(Acoef, p) > 0)
    {
        // gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Acoef, p);
        mpz_mod(Acoef, Acoef, p);
    }
    if (mpz_sgn(Acoef) == -1)
    {
        mpz_add(Acoef, p, Acoef);
    }

    //    std::cout << Ai << " " << Aj << " " << Acoef << std::std::endl;

    Bmat >> Bi;
    Bmat >> Bj;
    Bmat >> Bcoef;
    if (mpz_cmpabs(Bcoef, p) > 0)
    {
        // gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Bcoef, p);
        mpz_mod(Bcoef, Bcoef, p);
    }
    if (mpz_sgn(Bcoef) == -1)
    {
        mpz_add(Bcoef, p, Bcoef);
    }

    Cmat >> Ci;
    Cmat >> Cj;
    Cmat >> Ccoef;

    if (mpz_cmpabs(Ccoef, p) > 0)
    {
        // gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Ccoef, p);
        mpz_mod(Ccoef, Ccoef, p);
    }
    if (mpz_sgn(Ccoef) == -1)
    {
        mpz_mul_si(Ccoef, Ccoef, -1);
    }
    else if (mpz_sgn(Ccoef) == 1)
    {
        mpz_mul_si(Ccoef, Ccoef, -1);
        mpz_add(Ccoef, p, Ccoef);
    }

    int num_intermediate_vars = num_vars;
    int num_inputs_outputs = num_inputs + num_outputs;

    q.primary_input_size = num_inputs_outputs;
    q.auxiliary_input_size = num_intermediate_vars;

    for (int currentconstraint = 1; currentconstraint <= num_constraints; currentconstraint++)
    {
        libsnark::linear_combination<FieldT> A, B, C;

        while (Aj == currentconstraint && Amat)
        {
            if (Ai <= num_intermediate_vars && Ai != 0)
            {
                Ai += num_inputs_outputs;
            }
            else if (Ai > num_intermediate_vars)
            {
                Ai -= num_intermediate_vars;
            }

            FieldT AcoefT(Acoef);
            A.add_term(Ai, AcoefT);
            if (!Amat)
            {
                break;
            }
            Amat >> Ai;
            Amat >> Aj;
            Amat >> Acoef;
            if (mpz_cmpabs(Acoef, p) > 0)
            {
                gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Acoef, p);
                mpz_mod(Acoef, Acoef, p);
            }
            if (mpz_sgn(Acoef) == -1)
            {
                mpz_add(Acoef, p, Acoef);
            }
        }

        while (Bj == currentconstraint && Bmat)
        {
            if (Bi <= num_intermediate_vars && Bi != 0)
            {
                Bi += num_inputs_outputs;
            }
            else if (Bi > num_intermediate_vars)
            {
                Bi -= num_intermediate_vars;
            }
            //         std::cout << Bi << " " << Bj << " " << Bcoef << std::std::endl;
            FieldT BcoefT(Bcoef);
            B.add_term(Bi, BcoefT);
            if (!Bmat)
            {
                break;
            }
            Bmat >> Bi;
            Bmat >> Bj;
            Bmat >> Bcoef;
            if (mpz_cmpabs(Bcoef, p) > 0)
            {
                gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Bcoef, p);
                mpz_mod(Bcoef, Bcoef, p);
            }
            if (mpz_sgn(Bcoef) == -1)
            {
                mpz_add(Bcoef, p, Bcoef);
            }
        }

        while (Cj == currentconstraint && Cmat)
        {
            if (Ci <= num_intermediate_vars && Ci != 0)
            {
                Ci += num_inputs_outputs;
            }
            else if (Ci > num_intermediate_vars)
            {
                Ci -= num_intermediate_vars;
            }
            // Libsnark constraints are A*B = C, vs. A*B - C = 0 for Zaatar.
            // Which is why the C coefficient is negated.

            // std::cout << Ci << " " << Cj << " " << Ccoef << std::std::endl;
            FieldT CcoefT(Ccoef);
            C.add_term(Ci, CcoefT);
            if (!Cmat)
            {
                break;
            }
            Cmat >> Ci;
            Cmat >> Cj;
            Cmat >> Ccoef;
            if (mpz_cmpabs(Ccoef, p) > 0)
            {
                gmp_printf("WARNING: Coefficient larger than prime (%Zd > %Zd).\n", Ccoef, p);
                mpz_mod(Ccoef, Ccoef, p);
            }
            if (mpz_sgn(Ccoef) == -1)
            {
                mpz_mul_si(Ccoef, Ccoef, -1);
            }
            else if (mpz_sgn(Ccoef) == 1)
            {
                mpz_mul_si(Ccoef, Ccoef, -1);
                mpz_add(Ccoef, p, Ccoef);
            }
        }

        q.add_constraint(libsnark::r1cs_constraint<FieldT>(A, B, C));

        // dump_constraint(r1cs_constraint<FieldT>(A, B, C), va, variable_annotations);
    }

    libff::start_profiling();
    libsnark::r1cs_gg_ppzksnark_keypair<libsnark::default_r1cs_gg_ppzksnark_pp> keypair = libsnark::r1cs_gg_ppzksnark_generator<libsnark::default_r1cs_gg_ppzksnark_pp>(q);
    libsnark::r1cs_gg_ppzksnark_processed_verification_key<libsnark::default_r1cs_gg_ppzksnark_pp> pvk = libsnark::r1cs_gg_ppzksnark_verifier_process_vk<libsnark::default_r1cs_gg_ppzksnark_pp>(keypair.vk);

    vkey << pvk;
    pkey << keypair.pk;

    if (unprocessed_vkey_file.length() > 0)
    {
        std::ofstream unprocessed_vkey(unprocessed_vkey_file);
        unprocessed_vkey << keypair.vk;
        unprocessed_vkey.close();
    }
}

template <typename HashT>
void fakeCalcAllLevels(std::vector<std::vector<libff::bit_vector>> &levels, size_t level, size_t digest_len)
{
    // level 1 upper layer
    for (int i = level; i > 0; i--)
    {
        for (int j = 0; j < levels[i].size(); j += 2)
        {
            libff::bit_vector other(digest_len);
            std::generate(other.begin(), other.end(), [&]()
                          { return std::rand() % 2; });
            levels[i - 1].push_back(other);
        }
    }
}

void proveInteractiveMerkleTree()
{
#if MEM_INTEGRITY == MERKLE_TREE

    uint64_t time_start = get_sys_clock();

    typedef sha256_two_to_one_hash_gadget<FieldT> HashT;
    // assert(g_synth_table_size == 1024 * 1024 * 10);
    g_synth_table_size = 1024;
    size_t tree_depth = 10; // 24;
    const size_t digest_len = HashT::get_digest_len();

#if !MERKLE_SKIP_INIT
    std::vector<std::vector<libff::bit_vector>> levels(tree_depth);

    libff::bit_vector leaf, root, address_bits(tree_depth);
    int leaf_count = std::pow(2, tree_depth);

    libff::bit_vector zero_cached = hash256<HashT>("0");

    for (int i = 0; i < leaf_count; i++)
    {
        libff::bit_vector tmp = zero_cached;
        // std::cout << *binToHex<HashT>(tmp) << std::endl;
        levels[tree_depth - 1].push_back(tmp);
    }

    fakeCalcAllLevels<HashT>(levels, tree_depth - 1, digest_len); // to save time
#else
    int leaf_count = std::pow(2, tree_depth);
    libff::bit_vector zero_cached = hash256<HashT>("0");
    std::vector<std::vector<libff::bit_vector>> levels(tree_depth);
    levels[tree_depth - 1].reserve(leaf_count);
    libff::bit_vector leaf, root, address_bits(tree_depth);
#endif
    // calcAllLevels<HashT>(levels, tree_depth-1);
    libff::bit_vector input = levels[0][0];

    uint64_t init_time = get_sys_clock();
    INC_INT_STATS(time_merkle_init, init_time - time_start);

    root = input;
    // input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
    // root = HashT::get_hash(input);

    log_manager->_logger[0]->flushRestNClose();
    uint64_t numEntries = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        numEntries += stats->_stats[tid]->_int_stats[STAT_num_log_entries];
    }

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    map<uint32_t, uint32_t> lwMap;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE // usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        // recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        // cout << "numkeys " << num_keys << endl;

        uint32_t wi = 0;
        uint32_t ri = 0;

        g_verification_txn_count++;

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;

#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl *wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item,
                0,
                GET_THD_ID);
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif

            uint32_t v_addr = table_id * g_synth_table_size + key;

            v_addr = v_addr % g_synth_table_size; // Hack, assuming table_id==0

            if (accessType == RD)
            {
                // generate proof
                std::vector<merkle_authentication_node> path(tree_depth);
                uint32_t addr = v_addr;
                libff::bit_vector leaf, address_bits(tree_depth);

                leaf = levels[tree_depth - 1][addr];

                for (int i = 0; i < tree_depth; i++)
                {
                    uint32_t tmp = (addr & 0x01);
                    address_bits[i] = tmp;
                    addr = addr / 2;
                    // std::cout << address_bits[tree_depth-1-i] << std::endl;
                }

                // Fill in the path
                size_t index = v_addr;
                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    libff::bit_vector path_tmp = (address_bits[tree_depth - 1 - i] == 0 ? levels[i][index + 1] : levels[i][index - 1]);
                    index = index / 2;
                }

                // ship path

                INC_INT_STATS(int_comm_cost, tree_depth * digest_len / 8);

                ri++;
            }
            else // accessType == WR
            {
                // update the merkle tree

                std::vector<merkle_authentication_node> prev_path(tree_depth);
                libff::bit_vector prev_load_hash = levels[tree_depth - 1][v_addr];
                libff::bit_vector prev_store_hash = hash256<HashT>(to_string(lwMap[v_addr] * 2));

                libff::bit_vector loaded_leaf = prev_load_hash;
                libff::bit_vector stored_leaf = prev_store_hash;

                libff::bit_vector address_bits(tree_depth);

                uint32_t addr = v_addr;
                for (int i = 0; i < tree_depth; i++)
                {
                    uint32_t tmp = (addr & 0x01);
                    address_bits[i] = tmp;
                    addr = addr / 2;
                    // std::cout << address_bits[tree_depth-1-i] << std::endl;
                }

                uint32_t index = v_addr;

                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    bool computed_is_right = address_bits[tree_depth - 1 - i] == 0;
                    libff::bit_vector other = computed_is_right ? levels[i][index + 1] : levels[i][index - 1];

                    prev_path[i] = other;

                    libff::bit_vector load_block = prev_load_hash;
                    load_block.insert(computed_is_right ? load_block.begin() : load_block.end(), other.begin(), other.end());
                    libff::bit_vector store_block = prev_store_hash;
                    store_block.insert(computed_is_right ? store_block.begin() : store_block.end(), other.begin(), other.end());

                    libff::bit_vector load_h = HashT::get_hash(load_block);
                    libff::bit_vector store_h = HashT::get_hash(store_block);

                    prev_load_hash = load_h;
                    prev_store_hash = store_h;

                    levels[i][index] = store_h; // update the path

                    index = index / 2;
                }

                libff::bit_vector load_root = prev_load_hash;
                libff::bit_vector store_root = prev_store_hash;

                lwMap[v_addr] = 2 * lwMap[v_addr];

                INC_INT_STATS(int_comm_cost, (tree_depth + 4) * digest_len / 8);

                // ship prev_path, load_root, store_root, loaded_leaf, stored_leaf
            }
        }

        std::this_thread::sleep_for(timespan); // simulate the latency of communication between server and client
        // usleep(SIM_NET_LATENCY);
        uint64_t t_verify = get_sys_clock();
        INC_INT_STATS(time_latency_verify, t_verify - tt2);
        INC_INT_STATS(int_latency_num, 1);
    }

    uint64_t end_time = get_sys_clock();
    INC_INT_STATS(time_merkle_prove, end_time - init_time);
    pthread_join(p_log, NULL);
#endif
}

void proveInteractiveMerkleTreeNative() // without using libsnark gadget
{
#if MEM_INTEGRITY == MERKLE_TREE

    uint64_t time_start = get_sys_clock();
    // g_synth_table_size = 1024;
    size_t tree_depth = 24; // for the default 10485760 memory slots

    uint32_t digest_len = 256;

    std::vector<std::vector<uint8_t *>> levels(tree_depth);

    uint8_t leaf[32], address_bits[tree_depth];
    uint8_t *root;

    int leaf_count = std::pow(2, tree_depth);

    MYSHA256 sha;
    sha.update("0");

    uint8_t *zero_cached = sha.digest();

    uint8_t *pool = new uint8_t[2 * leaf_count * 32];
    uint32_t offset = 0;

    for (int i = 0; i < leaf_count; i++)
    {
        uint8_t *tmp = pool + offset;
        offset += 32;
        memcpy(tmp, zero_cached, 32);
        // std::cout << *binToHex<HashT>(tmp) << std::endl;
        levels[tree_depth - 1].push_back(tmp);
    }

    delete[] zero_cached;

#if !MERKLE_SKIP_INIT
    sha.clear();

    for (int i = tree_depth - 1; i > 0; i--)
    {
        for (int j = 0; j < levels[i].size(); j += 2)
        {
            uint8_t *tmp = pool + offset;
            offset += 32;
            sha.clear();
            sha.update(levels[i][j], 64); // 32 * 2
            uint8_t *digest = sha.digest();
            memcpy(tmp, digest, 32);
            levels[i - 1].push_back(tmp);
            delete[] digest;
        }
    }

    cout << "Finished initialization" << endl;
#else
    for (int i = tree_depth - 1; i > 0; i--)
    {
        for (int j = 0; j < levels[i].size(); j += 2)
        {
            uint8_t *tmp = pool + offset;
            offset += 32;
            // leave as it is
            levels[i - 1].push_back(tmp);
        }
    }

    cout << "Skipped initialization" << endl;
#endif
    // calcAllLevels<HashT>(levels, tree_depth-1);
    uint8_t *input = levels[0][0];

    uint64_t init_time = get_sys_clock();
    INC_INT_STATS(time_merkle_init, init_time - time_start);

    root = input;
    // input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
    // root = HashT::get_hash(input);

    log_manager->_logger[0]->flushRestNClose();
    uint64_t numEntries = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        numEntries += stats->_stats[tid]->_int_stats[STAT_num_log_entries];
    }

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    map<uint32_t, uint32_t> lwMap;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE // usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        // recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        // cout << "numkeys " << num_keys << endl;

        uint32_t wi = 0;
        uint32_t ri = 0;

        g_verification_txn_count++;

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;

#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl *wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item,
                0,
                GET_THD_ID);
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif

            uint32_t v_addr = table_id * g_synth_table_size + key;

            v_addr = v_addr % g_synth_table_size; // Hack, assuming table_id==0

            if (accessType == RD)
            {
                // generate proof
                uint8_t path[tree_depth][32];

                uint32_t addr = v_addr;

                // leaf = levels[tree_depth - 1][addr];

                for (int i = 0; i < tree_depth; i++)
                {
                    uint32_t tmp = (addr & 0x01);
                    address_bits[i] = tmp;
                    addr = addr / 2;
                    // std::cout << address_bits[tree_depth-1-i] << std::endl;
                }

                // Fill in the path
                size_t index = v_addr;
                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    uint8_t *path_tmp = (address_bits[tree_depth - 1 - i] == 0 ? levels[i][index + 1] : levels[i][index - 1]);
                    memcpy(path[i], path_tmp, 32);
                    index = index / 2;
                }

                // ship path

                INC_INT_STATS(int_comm_cost, tree_depth * digest_len / 8);

                // verification
                uint8_t currentNode[32], merged[64];
                index = v_addr;
                memcpy(currentNode, levels[tree_depth - 1][v_addr], 32);
                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    sha.clear();
                    if (address_bits[tree_depth - 1 - i] == 0)
                    {
                        memcpy(merged, currentNode, 32);
                        memcpy(merged + 32, levels[i][index + 1], 32);
                    }
                    else
                    {
                        memcpy(merged + 32, currentNode, 32);
                        memcpy(merged, levels[i][index - 1], 32);
                    }
                    sha.update(merged, 64);
                    uint8_t *digest = sha.digest();
                    memcpy(currentNode, digest, 32);
                    delete[] digest;
                    index = index / 2;
                }
                ri++;
            }
            else // accessType == WR
            {
                // update the merkle tree

                uint8_t prev_path[tree_depth][32];
                uint8_t prev_load_hash[32];
                memcpy(prev_load_hash, levels[tree_depth - 1][v_addr], 32);
                sha.clear();
                sha.update(to_string(lwMap[v_addr] * 2));

                uint8_t *prev_store_hash = sha.digest();

                uint8_t *loaded_leaf = prev_load_hash;
                uint8_t *stored_leaf = prev_store_hash;

                uint8_t address_bits[tree_depth];

                uint32_t addr = v_addr;
                for (int i = 0; i < tree_depth; i++)
                {
                    uint32_t tmp = (addr & 0x01);
                    address_bits[i] = tmp;
                    addr = addr / 2;
                    // std::cout << address_bits[tree_depth-1-i] << std::endl;
                }

                uint32_t index = v_addr;

                uint8_t load_block[64], store_block[64];

                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    bool computed_is_right = address_bits[tree_depth - 1 - i] == 0;
                    uint8_t *other = computed_is_right ? levels[i][index + 1] : levels[i][index - 1];

                    memcpy(prev_path[i], other, 32);

                    if (!computed_is_right)
                    {
                        memcpy(load_block, other, 32);
                        memcpy(load_block + 32, prev_load_hash, 32);
                        memcpy(store_block, other, 32);
                        memcpy(store_block + 32, prev_store_hash, 32);
                    }
                    else
                    {
                        memcpy(load_block + 32, other, 32);
                        memcpy(load_block, prev_load_hash, 32);
                        memcpy(store_block + 32, other, 32);
                        memcpy(store_block, prev_store_hash, 32);
                    }
                    sha.clear();
                    sha.update(load_block, 64);
                    uint8_t *load_digest = sha.digest();
                    memcpy(prev_load_hash, load_digest, 32);
                    delete[] load_digest;

                    sha.clear();
                    sha.update(store_block, 64);
                    uint8_t *store_digest = sha.digest();
                    memcpy(prev_store_hash, store_digest, 32);
                    delete[] store_digest;

                    memcpy(levels[i][index], prev_store_hash, 32); // update the path

                    index = index / 2;
                }

                lwMap[v_addr] = 2 * lwMap[v_addr];

                INC_INT_STATS(int_comm_cost, (tree_depth + 4) * digest_len / 8);

                // ship prev_path, load_root, store_root, loaded_leaf, stored_leaf

                // verification
                uint8_t currentNode[32], merged[64];
                index = v_addr;
                memcpy(currentNode, levels[tree_depth - 1][v_addr], 32);
                for (int i = tree_depth - 1; i >= 0; i--)
                {
                    sha.clear();
                    if (address_bits[tree_depth - 1 - i] == 0)
                    {
                        memcpy(merged, currentNode, 32);
                        memcpy(merged + 32, levels[i][index + 1], 32);
                    }
                    else
                    {
                        memcpy(merged + 32, currentNode, 32);
                        memcpy(merged, levels[i][index - 1], 32);
                    }
                    sha.update(merged, 64);
                    uint8_t *digest = sha.digest();
                    memcpy(currentNode, digest, 32);
                    delete[] digest;
                    index = index / 2;
                }
            }
        }

        std::this_thread::sleep_for(timespan); // simulate the latency of communication between server and client
        // usleep(SIM_NET_LATENCY);
        uint64_t t_verify = get_sys_clock();
        INC_INT_STATS(time_latency_verify, t_verify - tt2);
        INC_INT_STATS(int_latency_num, 1);
    }

    uint64_t end_time = get_sys_clock();
    INC_INT_STATS(time_merkle_prove, end_time - init_time);
    pthread_join(p_log, NULL);
#endif
}

void initADProving()
{
    assert(MEM_INTEGRITY == RSA_AD); // && CC_ALG == NO_WAIT);
    mpz_init(mp_POE_L);
    mpz_set_str(mp_POE_L, POE_L, 10);
    mpz_init(mp_POE_q); // to simulate calc Q
    mpz_init(mp_POE_Q); // to simulate calc Q
    mpz_init(mp_N);
    mpz_init(mp_MSMOD);
    // mpz_import(mp_MSMOD, 1, 1, sizeof(g_ms_mod), 0, 0, &g_ms_mod);
    mpz_set_str(mp_MSMOD, MS_MOD, 10);
    mpz_set_str(mp_N, N_IN_USE, 10);
    mpz_init(mp_BN128_ORDER);
    mpz_set_str(mp_BN128_ORDER, BN128_ORDER, 10);

    mpz_init(mp_G);
    mpz_set_ui(mp_G, __LTM_G);
    // init coeffs
#if MS_LIMBS > 2
    for (uint32_t i = 1; i <= 2 * MS_LIMBS - 1; i++)
    {
        multi_scalar_coeff[i][0] = 1;
        for (uint32_t j = 1; j <= MS_LIMBS - 1; j++)
        {
            // coefficient hack
            multi_scalar_coeff[i][j] = multi_scalar_coeff[i][j + MS_LIMBS] = multi_scalar_coeff[i][j - 1] * i;
        }
    }
#endif
}

void proveInteractive()
{

    // assert(false);
    //#if false

    initADProving();

    log_manager->_logger[0]->flushRestNClose();
    uint64_t numEntries = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        numEntries += stats->_stats[tid]->_int_stats[STAT_num_log_entries];
    }

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    NEW_EXTYPE(memDigest, __LTM_G);
    NEW_EXTYPE(currentProd, 1);
    uint32_t memCounter = 0;

    uint32_t localDigest = (uint32_t)__LTM_G;

    map<uint32_t, uint32_t> *latestWrittenIndex = (map<uint32_t, uint32_t> *)_mm_malloc(sizeof(map<uint32_t, uint32_t>), ALIGN_SIZE);
    new (latestWrittenIndex) map<uint32_t, uint32_t>();

    map<uint32_t, uint32_t> &lwMap = *latestWrittenIndex;

    bool firstElementInBatch = true;
    uint32_t prod_init_hash = 1;
    uint32_t lastAB = -1;
    uint32_t lastABInd = 1;
    uint32_t lastABRI = 1;
    uint32_t writeExpProd = 1;

    uint32_t total_inputs = 0;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE // usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        // recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        // cout << "numkeys " << num_keys << endl;

        uint32_t wi = 0;
        uint32_t ri = 0;

        g_verification_txn_count++;

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;

#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl *wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item,
                0,
                GET_THD_ID);
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif

            ////////////////////

            uint32_t v_addr = table_id * g_synth_table_size + key;
            // hack
            v_addr = v_addr % g_synth_table_size;

            if (accessType == RD)
            {
                if (lwMap.find(v_addr) == lwMap.end())
                {
                    NEW_EXTYPE(A, 0)
                    NEW_EXTYPE(B, 0)
                    NEW_EXTYPE(_g, 0)
                    NEW_EXTYPE(C, H_addr(v_addr))
                    mpz_gcdext(_g.get_mpz_t(), A.get_mpz_t(), B.get_mpz_t(), currentProd.get_mpz_t(), C.get_mpz_t());

                    INC_INT_STATS(int_comm_cost, 2 * 128 / 8); // estimate the communication cost: raw value + A + B, the raw value is the same as the init value, so omitted here.
                }
                else
                {
                    NEW_EXTYPE(quotient, 0)
                    mpz_t Hval;
                    mpz_init(Hval);
                    H_mp(Hval, v_addr, val);
                    mpz_cdiv_q(quotient.get_mpz_t(), currentProd.get_mpz_t(), Hval);

                    NEW_EXTYPE(mp_acc1, 0)
                    mpz_powm(mp_acc1.get_mpz_t(), mp_G, quotient.get_mpz_t(), mp_N);

                    INC_INT_STATS(int_comm_cost, 1000 + 1 * 128 / 8); // estimate the communication cost: raw value + pi

                    memDigest = mp_acc1; // asuming no blind writes
                    currentProd = quotient;
                }
                ri++;
            }
            else // accessType == WR
            {

                mpz_t Hval;
                mpz_init(Hval);
                H_mp(Hval, v_addr, val);

                mpz_powm(memDigest.get_mpz_t(), memDigest.get_mpz_t(), Hval, mp_N);

                mpz_mul(currentProd.get_mpz_t(), currentProd.get_mpz_t(), Hval);

                INC_INT_STATS(int_comm_cost, 0);
                // no communication happens for write

                lwMap[v_addr] = memCounter;

                memCounter++;
                wi++;
            }
        }

        std::this_thread::sleep_for(timespan); // simulate the latency of communication between server and client
        // usleep(SIM_NET_LATENCY);
        uint64_t t_verify = get_sys_clock();
        INC_INT_STATS(time_latency_verify, t_verify - tt2);
        INC_INT_STATS(int_latency_num, 1);
    }

    _mm_free(latestWrittenIndex);
    pthread_join(p_log, NULL);
    //#endif
}

/*
FF_TYPE intractibleHash(uint64_t x)
{
    // return a large number based on x
    auto t = hash64(x);
    return FF_INIT_2INT64(t, t);
}
*/

void proveHandWritten()
{
    uint64_t starttime = get_sys_clock();

    default_r1cs_ppzksnark_pp::init_public_params();
    libff::inhibit_profiling_info = true;
    libff::inhibit_profiling_counters = true; // libff profiling is not thread-safe.
    typedef libff::Fr<default_r1cs_ppzksnark_pp> FieldT;

#if MEM_INTEGRITY == RSA_AD
    ////////////////////////////
    initADProving();

#else
                                               /*
                                                   // Init merkle tree
                                                   typedef sha256_two_to_one_hash_gadget<FieldT> HashT;
                                                   assert(g_synth_table_size == 1024 * 1024 * 10);
                                                   size_t tree_depth = 24;
                                                   const size_t digest_len = HashT::get_digest_len();
                                           
                                                   std::vector<std::vector<libff::bit_vector>> levels(tree_depth);
                                                   libff::bit_vector leaf, root, address_bits(tree_depth);
                                                   int leaf_count = std::pow(2, tree_depth);
                                                   for (int i = 0; i < leaf_count; i++) {
                                                           libff::bit_vector tmp = hash256<HashT>("0");
                                                           //std::cout << *binToHex<HashT>(tmp) << std::endl;
                                                           levels[tree_depth - 1].push_back(tmp);
                                                   }
                                                   calcAllLevels<HashT>(levels, tree_depth-1);
                                                   libff::bit_vector input = levels[0][0];
                                                   input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
                                                   root = HashT::get_hash(input);
                                               */
#endif

    map<uint32_t, uint32_t> initMap;

    assert(LOG_ALGORITHM == LOG_SERIAL);
    // assert(WORKLOAD == YCSB); // now we have tpcc
    assert(LOG_TYPE == LOG_DATA);
    assert(g_num_logger == 1);

    EX_TYPE initialDigest = __LTM_G;

    log_manager->_logger[0]->flushRestNClose();

    pthread_t p_prover_thds[g_prover_threads];

    g_log_recover = true;
    log_manager->init(); // re-open the files and prepare to read
    pthread_t p_log;
    logth = (LoggingThread *)_mm_malloc(sizeof(LoggingThread), ALIGN_SIZE);
    new (logth) LoggingThread();
    logth->set_thd_id(0);
    glob_manager->_workload->sim_done = 0;
    pthread_barrier_init(&log_bar, NULL, g_num_logger);
    pthread_create(&p_log, NULL, f_vec, NULL);
    glob_manager->set_thd_id(0); // this is the new thread 0.
    char default_entry[g_max_log_entry_size];
    uint32_t count = 0;

    vector<uint32_t> values;
    vector<uint32_t> addrs;
    vector<uint32_t> txnIDs;

    uint64_t total_num_commits = 0;
    for (uint32_t tid = 0; tid < g_thread_cnt; tid++)
    {
        total_num_commits += stats->_stats[tid]->_int_stats[STAT_num_commits];
    }

    uint64_t proverload = total_num_commits / g_prover_threads + 1;

    uint64_t lastProverThreadCount = 0;
    uint64_t ptcounter = 0;

    uint32_t txnID = 0;

    while (true)
    {
        char *entry = default_entry;
        uint64_t tt = get_sys_clock();
        uint64_t lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
        if (entry == NULL)
        {
            if (log_manager->_logger[0]->iseof())
            {
                entry = default_entry;
                lsn = log_manager->_logger[0]->get_next_log_entry_non_atom(entry, true);
                if (entry == NULL)
                    break;
            }
            else
            {
                PAUSE // usleep(50);
                    INC_INT_STATS(time_io, get_sys_clock() - tt);
                continue;
            }
        }
        uint64_t tt2 = get_sys_clock();
        INC_INT_STATS(time_io, tt2 - tt);
        // Format for serial logging
        // | checksum | size | ... |
        assert(*(uint32_t *)entry == 0xbeef || entry[0] == 0x7f);
        char *log_entry = entry + sizeof(uint32_t) * 2;
        // recover_txn(entry + sizeof(uint32_t) * 2);
        uint32_t offset = 0;

        uint32_t num_keys;

        UNPACK(log_entry, num_keys, offset);
        // cout << "numkeys " << num_keys << endl;

        uint32_t wi = 0;
        uint32_t ri = 0;

        g_verification_txn_count++;

        for (uint32_t i = 0; i < num_keys; i++)
        {

            uint32_t table_id;
            uint64_t key;
            access_t accessType;
            uint32_t data_length;
            char *data;

            UNPACK(log_entry, table_id, offset);
            UNPACK(log_entry, key, offset);
            UNPACK(log_entry, accessType, offset);
            UNPACK(log_entry, data_length, offset);
            data = log_entry + offset;
            offset += data_length;
            uint32_t val = atoi(data);

            itemid_t *m_item;

#if WORKLOAD == YCSB
            ((ycsb_wl *)glob_manager->get_workload())->the_index->index_read(key, m_item, 0, GET_THD_ID);
#elif WORKLOAD == TPCC
            tpcc_wl *wl = (tpcc_wl *)glob_manager->get_workload();
            wl->tpcc_tables[(TableName)table_id]->get_primary_index()->index_read(
                key,
                m_item,
                0,
                GET_THD_ID);
            key = key * NUM_TABLES + table_id; // get unique keys
#else
            assert(0);
#endif

            uint32_t v_addr = table_id * g_synth_table_size + key;
            v_addr = v_addr % g_synth_table_size; // Hack

            addrs.push_back(v_addr);
            values.push_back(val);
            if (accessType == RD)
                txnIDs.push_back(txnID << 1);
            else
                txnIDs.push_back((txnID << 1) | 1);
        }

#if CC_ALG == DETRESERVE
        UNPACK(log_entry, txnID, offset);
        // we will be using the batch_num as txn_id
#else
        txnID++;
#endif
        uint64_t tt3 = get_sys_clock();
        INC_INT_STATS(time_prepareTraces, tt3 - tt2);

        if (g_verification_txn_count - lastProverThreadCount >= proverload)
        {
            cout << lastProverThreadCount << " " << g_verification_txn_count << " to thread " << ptcounter << endl;
            ProverThreadArg *pta;
            pta = (ProverThreadArg *)_mm_malloc(sizeof(ProverThreadArg), ALIGN_SIZE);
            new (pta) ProverThreadArg;
            pta->initDigest = initialDigest;
            pta->initMap = initMap;
            pta->values = values;
            pta->addrs = addrs;
            pta->txnIDs = txnIDs;
            pta->id = ptcounter;
#if MEM_INTEGRITY == MERKLE_TREE
/*
            // refresh root
            calcAllLevels<HashT>(levels, tree_depth-1);
            input = levels[0][0];
            input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
            root = HashT::get_hash(input);
            pta->root = root;
            */
#endif
            INC_INT_STATS(time_dispatcher_latency, get_sys_clock() - starttime);
            INC_INT_STATS(int_dispatcher_latency_num, 1);

            pthread_create(&p_prover_thds[ptcounter++], NULL, proveHandWrittenThread, (void *)pta);

            // apply the changes to initMap and initialDigest;
            uint32_t opLength = values.size();
            for (uint32_t i = 0; i < opLength; i++)
                if (txnIDs[i] & 1)
                {
                    initMap[addrs[i]] = values[i];
#if MEM_INTEGRITY == RSA_AD
                    // initialDigest = ipow_fp<EX_TYPE>(initialDigest, H(addrs[i], values[i]));
                    mpz_t Hval;
                    mpz_init(Hval);
                    H_mp(Hval, addrs[i], values[i]);
                    mpz_powm(initialDigest.get_mpz_t(), initialDigest.get_mpz_t(), Hval, mp_N);
#else
/*
                    // Update Merkle Tree
                    levels[tree_depth - 1][addrs[i] % g_synth_table_size] = hash256<HashT>(to_string(values[i]));
*/
#endif
                }
            values.clear();
            addrs.clear();
            txnIDs.clear();
            lastProverThreadCount = g_verification_txn_count;

            INC_INT_STATS(time_copyTraces, get_sys_clock() - tt3);
        }
    }

    if (lastProverThreadCount < g_verification_txn_count)
    {
        cout << lastProverThreadCount << " " << g_verification_txn_count << " to thread " << ptcounter << endl;
        ProverThreadArg *pta;

        pta = (ProverThreadArg *)_mm_malloc(sizeof(ProverThreadArg), ALIGN_SIZE);
        new (pta) ProverThreadArg;

        pta->initDigest = initialDigest;
        pta->initMap = initMap;
        pta->values = values;
        pta->addrs = addrs;
        pta->txnIDs = txnIDs;
        pta->id = ptcounter;
#if MEM_INTEGRITY == MERKLE_TREE
/*
            calcAllLevels<HashT>(levels, tree_depth-1);
            input = levels[0][0];
            input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
            root = HashT::get_hash(input);
            pta->root = root;
*/
#endif
        INC_INT_STATS(time_dispatcher_latency, get_sys_clock() - starttime);
        INC_INT_STATS(int_dispatcher_latency_num, 1);

        proveHandWrittenThread((void *)pta);
    }

    pthread_join(p_log, NULL);
    for (uint32_t ptd_index = 0; ptd_index < ptcounter; ptd_index++)
    {
        pthread_join(p_prover_thds[ptd_index], NULL);
    }

    cout << "Provers finished." << endl;
}

void *proveHandWrittenThread(void *vpta)
{
    uint64_t starttime = get_sys_clock();

    ProverThreadArg *pta = (ProverThreadArg *)vpta;

    protoboard<FieldT> pb;
    vector<pb_variable<FieldT>> mp_N_pbv(MS_LIMBS);

    uint32_t num_limbs = MS_LIMBS;
    for (uint32_t i = 0; i < num_limbs; i++)
    {
        mp_N_pbv[i].allocate(pb, "mp_N_pbv");
    }
    get_fields_from_mpz(pb, mp_N, mp_N_pbv, num_limbs, mp_MSMOD);

    pb_variable<FieldT> mp_L_pb;
    mp_L_pb.allocate(pb, "mp_L_pb");
    pb.val(mp_L_pb) = FieldT(mp_POE_L);

#if MEM_INTEGRITY == MERKLE_TREE

    // Init merkle tree
    typedef sha256_two_to_one_hash_gadget<FieldT> HashT;
    assert(g_synth_table_size == 1024 * 1024 * 10);
    size_t tree_depth = 24;
    const size_t digest_len = HashT::get_digest_len();

    std::vector<std::vector<libff::bit_vector>> levels(tree_depth);
    libff::bit_vector leaf, root, address_bits(tree_depth);
    int leaf_count = std::pow(2, tree_depth);
    libff::bit_vector zero_hash = hash256<HashT>("0");
    for (int i = 0; i < leaf_count; i++)
    {
        libff::bit_vector tmp = zero_hash;
        if (pta->initMap.find(i) != pta->initMap.end())
            tmp = hash256<HashT>(to_string(pta->initMap[i]));
        // std::cout << *binToHex<HashT>(tmp) << std::endl;
        levels[tree_depth - 1].push_back(tmp);
    }
    calcAllLevels<HashT>(levels, tree_depth - 1);
    libff::bit_vector input = levels[0][0];
    input.insert(input.end(), levels[0][1].begin(), levels[0][1].end());
    root = HashT::get_hash(input);

    digest_variable<FieldT> init_root(pb, digest_len, "init_root");
    init_root.generate_r1cs_witness(root); // initial value

    vector<digest_variable<FieldT>> pbv_roots;
    pbv_roots.push_back(init_root);
#endif

    uint32_t opLength = pta->values.size();
    uint32_t my_thread_id = g_thread_cnt + g_num_logger + pta->id;
    glob_manager->set_thd_id(my_thread_id);

    if (opLength == 0)
        return NULL;

    vector<pb_variable<FieldT>> pbv_val;
    vector<pb_variable<FieldT>> pbv_addr;
    vector<pb_variable<FieldT>> pbv_aux;
    vector<vector<pb_variable<FieldT>>> pbv_acc1;
    vector<vector<pb_variable<FieldT>>> pbv_acc2;
    vector<pb_variable<FieldT>> pbv_prod;
    vector<pb_variable<FieldT>> pbv_acc_prime;
    vector<pb_variable<FieldT>> pbv_A;
    vector<pb_variable<FieldT>> pbv_B;
    vector<vector<pb_variable<FieldT>>> pbv_local_digest;
    vector<pb_variable<FieldT>> pb_G(MS_LIMBS);

    allocate_pb_multi_scalar(pb, pb_G, MS_LIMBS, "g");
    pb.ADD_CONSTRAINT(pb_G[0], 1, __LTM_G); // pb_G == g
    pb.val(pb_G[0]) = __LTM_G;

    // pb_variable<FieldT> ldigest;
    // ldigest.allocate(pb, "local-digest-init");
    // pb.ADD_CONSTRAINT(ldigest, 1, __LTM_G);
    pbv_local_digest.push_back(pb_G);

    ////////////////////////////////////////////

    // FF_TYPE memDigest = FF_INIT_2INT64(0, __LTM_G);
    NEW_EXTYPE(memDigest, __LTM_G);
    NEW_EXTYPE(currentProd, 1);

    uint32_t memCounter = 0;

    /*
    vector<EX_TYPE> *accList = (vector<EX_TYPE> *)_mm_malloc(sizeof(vector<EX_TYPE>), ALIGN_SIZE);
    new (accList) vector<EX_TYPE>();

    vector<EX_TYPE> *prodList = (vector<EX_TYPE> *)_mm_malloc(sizeof(vector<EX_TYPE>), ALIGN_SIZE);
    new (prodList) vector<EX_TYPE>();
    */

    // inputList = (vector<uint32_t> *)_mm_malloc(sizeof(vector<uint32_t>), ALIGN_SIZE);
    // new (inputList) vector<uint32_t>();

    map<uint32_t, uint32_t> &lwMap = pta->initMap;

    // uint32_t prod_init_hash = 1;
    // uint32_t writeExpProd = 1;

    uint32_t total_inputs = 0;

    uint32_t lastBatch = 0;
    vector<pb_variable<FieldT>> pbv_wrList;
    NEW_EXTYPE(batchVal, 1)
    vector<pb_variable<FieldT>> pbv_initReadList;
    mpz_t readlist_exponent;
    mpz_init(readlist_exponent);
    mpz_t init_read_list_back;
    mpz_init(init_read_list_back);

    NEW_EXTYPE(initReadBatchVal, 1)
    vector<pb_variable<FieldT>> pbv_readBatchList;
    NEW_EXTYPE(readBatchVal, 1)

    uint32_t lastTxnID = pta->txnIDs[0] >> 1;

    uint64_t tt = get_sys_clock();
    INC_INT_STATS(time_proverInit, tt - starttime);

#if MEM_INTEGRITY == RSA_AD

    for (uint32_t ind = 0; ind < opLength; ind++)
    {
        uint64_t loopstart = get_sys_clock();

        uint32_t val = pta->values[ind];
        uint32_t v_addr = pta->addrs[ind];
        uint32_t txnID = pta->txnIDs[ind] >> 1;
        uint32_t accessType = pta->txnIDs[ind] & 1;

        if (txnID != lastTxnID && pbv_wrList.size() > 0)
        {
            // deal with read
            // initread
            if (pbv_initReadList.size() > 0)
            {

                pbv_A.push_back(pb_variable<FieldT>());
                auto &pb_A = pbv_A.back();
                pb_A.allocate(pb, "A");

                pbv_B.push_back(pb_variable<FieldT>());
                auto &pb_B = pbv_B.back();
                pb_B.allocate(pb, "B");

                vector<pb_variable<FieldT>> pb_A_Q(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_B_Q(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_A_Q_raised(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_B_Q_raised(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux1_POE(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux1_premod(2 * MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux1_q(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux2_POE(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux2_premod(2 * MS_LIMBS);
                vector<pb_variable<FieldT>> pb_aux2_q(MS_LIMBS);

                pb_variable<FieldT> readlist_exp_pbv;
                readlist_exp_pbv.allocate(pb, "readlist_exp_pbv");

                total_inputs += 2;
                // uint32_t A, B;
                // xgcd(currentProd, initReadBatchVal, A, B);
                NEW_EXTYPE(A, 0)
                NEW_EXTYPE(B, 0)
                NEW_EXTYPE(_g, 0)
                mpz_gcdext(_g.get_mpz_t(), A.get_mpz_t(), B.get_mpz_t(), currentProd.get_mpz_t(), initReadBatchVal.get_mpz_t());

                pb.val(pb_A) = TO_FIELD(A);
                pb.val(pb_B) = TO_FIELD(B);

                vector<pb_variable<FieldT>> aux1(MS_LIMBS), aux2(MS_LIMBS), aux3(MS_LIMBS), aux4(MS_LIMBS);

                for (uint32_t idx = 0; idx < MS_LIMBS; idx++)
                {
                    aux1[idx].allocate(pb, "aux1");
                    aux2[idx].allocate(pb, "aux2");
                    aux3[idx].allocate(pb, "aux3");
                    aux4[idx].allocate(pb, "aux4");
                    pb_A_Q[idx].allocate(pb, "pb_A_Q");
                    pb_B_Q[idx].allocate(pb, "pb_B_Q");
                    pb_A_Q_raised[idx].allocate(pb, "pb_A_Q_raised");
                    pb_B_Q_raised[idx].allocate(pb, "pb_B_Q_raised");
                    pb_aux1_POE[idx].allocate(pb, "pb_aux1_POE");
                    pb_aux2_POE[idx].allocate(pb, "pb_aux2_POE");
                    pb_aux1_q[idx].allocate(pb, "pb_aux1_q");
                    pb_aux2_q[idx].allocate(pb, "pb_aux2_q");
                }

                allocate_pb_multi_scalar(pb, pb_aux1_premod, 2 * MS_LIMBS, "pb_aux1_premod");
                allocate_pb_multi_scalar(pb, pb_aux2_premod, 2 * MS_LIMBS, "pb_aux2_premod");

                // aux1 = pbv_local_digest.back() ^ pb_A

                // simulate computation cost of server-side PoE preparation
                prepare_POE(A.get_mpz_t());
                prepare_POE(B.get_mpz_t());
                // TODO: set values for pb_A_Q, pb_A_Q_raised, pb_aux1_POE, pb_B_Q, pb_B_Q_raised, pb_aux2_POE

                pow_mod_p_multi_scalar<FieldT> pmp(pb, pbv_local_digest.back(), mp_N_pbv, pb_A, pb_aux1_POE, POE_BITS, MS_LIMBS, mp_MSMOD);
                pow_mod_p_multi_scalar<FieldT> pmp_POE_A(pb, pb_A_Q, mp_N_pbv, mp_L_pb, pb_A_Q_raised, POE_BITS, MS_LIMBS, mp_MSMOD);
                mul_multi_scalar<FieldT> mul_POE_A(pb, pb_A_Q_raised, pb_aux1_POE, pb_aux1_premod, MS_LIMBS, mp_MSMOD);
                mod_p_multi_scalar<FieldT> mul_mod_A(pb, pb_aux1_premod, mp_N_pbv, pb_aux1_q, aux1, POE_BITS, MS_LIMBS, mp_MSMOD);
                pmp.generate_r1cs_constraints();
                pmp_POE_A.generate_r1cs_constraints();
                mul_POE_A.generate_r1cs_constraints();
                mul_mod_A.generate_r1cs_constraints();
                // aux2 = g^(pbv_initReadList.back() * pb_B)

                pb.ADD_CONSTRAINT(pbv_initReadList.back(), pb_B, readlist_exp_pbv);

                pb.val(pbv_initReadList.back()).as_bigint().to_mpz(init_read_list_back);
                mpz_mul(readlist_exponent, B.get_mpz_t(), init_read_list_back);
                mpz_mod(readlist_exponent, readlist_exponent, mp_POE_L); // PoE hack
                pb.val(readlist_exp_pbv) = FieldT(readlist_exponent);
                pow_mod_p_multi_scalar<FieldT> pmp2(pb, pb_G, mp_N_pbv, readlist_exp_pbv, pb_aux2_POE, POE_BITS, MS_LIMBS, mp_MSMOD);
                pow_mod_p_multi_scalar<FieldT> pmp_POE_B(pb, pb_B_Q, mp_N_pbv, mp_L_pb, pb_B_Q_raised, POE_BITS, MS_LIMBS, mp_MSMOD);
                mul_multi_scalar<FieldT> mul_POE_B(pb, pb_B_Q_raised, pb_aux2_POE, pb_aux2_premod, MS_LIMBS, mp_MSMOD);
                mod_p_multi_scalar<FieldT> mul_mod_B(pb, pb_aux2_premod, mp_N_pbv, pb_aux2_q, aux2, POE_BITS, MS_LIMBS, mp_MSMOD);

                pmp2.generate_r1cs_constraints();
                pmp_POE_B.generate_r1cs_constraints();
                mul_POE_B.generate_r1cs_constraints();
                mul_mod_B.generate_r1cs_constraints();

                // due to integer intractability assumption, the gcd might not be 1
                // in future version we will move to real primes.

                // pb.ADD_CONSTRAINT(aux1, aux2, aux3);
                mod_p_multi_scalar<FieldT> g(pb, aux3, mp_N_pbv, aux4, pb_G, POE_BITS, MS_LIMBS, mp_MSMOD);

                g.generate_r1cs_constraints();

                NEW_EXTYPE(mp_aux1, 0)
                NEW_EXTYPE(mp_aux2, 0)
                NEW_EXTYPE(mp_aux3, 0)
                NEW_EXTYPE(mp_aux4, 0)

                mpz_powm(mp_aux1.get_mpz_t(), memDigest.get_mpz_t(), A.get_mpz_t(), mp_N);
                mpz_powm(mp_aux2.get_mpz_t(), initReadBatchVal.get_mpz_t(), B.get_mpz_t(), mp_N);
                mpz_mul(mp_aux3.get_mpz_t(), mp_aux1.get_mpz_t(), mp_aux2.get_mpz_t());
                mpz_cdiv_q(mp_aux4.get_mpz_t(), mp_aux3.get_mpz_t(), mp_N);

                get_fields_from_mpz(pb, mp_aux1.get_mpz_t(), aux1, MS_LIMBS, mp_MSMOD);
                get_fields_from_mpz(pb, mp_aux2.get_mpz_t(), aux2, MS_LIMBS, mp_MSMOD);
                get_fields_from_mpz(pb, mp_aux3.get_mpz_t(), aux3, MS_LIMBS, mp_MSMOD);
                get_fields_from_mpz(pb, mp_aux4.get_mpz_t(), aux4, MS_LIMBS, mp_MSMOD);

                pmp.generate_r1cs_witness();
                pmp_POE_A.generate_r1cs_witness();
                mul_POE_A.generate_r1cs_witness();
                mul_mod_A.generate_r1cs_witness();

                pmp2.generate_r1cs_witness();
                pmp_POE_B.generate_r1cs_witness();
                mul_POE_B.generate_r1cs_witness();
                mul_mod_B.generate_r1cs_witness();

                g.generate_r1cs_witness();

                total_inputs += 4;

                pbv_initReadList.clear();
                initReadBatchVal = 1;
            }

            // normal read batch
            if (pbv_readBatchList.size() > 0)
            {

                pbv_acc1.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
                auto &pb_acc1 = pbv_acc1.back();
                allocate_pb_multi_scalar(pb, pb_acc1, MS_LIMBS, "acc1");

                // acc1^pbv_readBatchList = pbv_localdigest
                vector<pb_variable<FieldT>> pb_rbl_Q(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_rbl_Q_raised(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_localDigest_POE(MS_LIMBS);
                vector<pb_variable<FieldT>> pb_localDigest_premod(2 * MS_LIMBS);
                vector<pb_variable<FieldT>> pb_localDigest_q(MS_LIMBS);

                allocate_pb_multi_scalar(pb, pb_rbl_Q, MS_LIMBS, "pb_rbl_Q");
                allocate_pb_multi_scalar(pb, pb_rbl_Q_raised, MS_LIMBS, "pb_rbl_Q_raised");
                allocate_pb_multi_scalar(pb, pb_localDigest_POE, MS_LIMBS, "pb_localDigest_POE");
                allocate_pb_multi_scalar(pb, pb_localDigest_premod, MS_LIMBS, "pb_localDigest_premod");
                allocate_pb_multi_scalar(pb, pb_localDigest_q, MS_LIMBS, "pb_localDigest_q");

                pow_mod_p_multi_scalar<FieldT> pmp_rbl(
                    pb,
                    pb_rbl_Q,
                    mp_N_pbv,
                    mp_L_pb,
                    pb_rbl_Q_raised,
                    POE_BITS,
                    MS_LIMBS,
                    mp_MSMOD);

                pow_mod_p_multi_scalar<FieldT> pmp3(
                    pb,
                    pb_acc1,
                    mp_N_pbv,
                    pbv_readBatchList.back(),
                    pb_localDigest_POE,
                    POE_BITS,
                    MS_LIMBS,
                    mp_MSMOD);

                mul_multi_scalar<FieldT> mul_POE_rbl(
                    pb,
                    pb_rbl_Q_raised,
                    pb_localDigest_POE,
                    pb_localDigest_premod,
                    MS_LIMBS,
                    mp_MSMOD);

                mod_p_multi_scalar<FieldT> mul_mod_rbl(
                    pb,
                    pb_localDigest_premod,
                    mp_N_pbv,
                    pb_localDigest_q,
                    pbv_local_digest.back(),
                    POE_BITS,
                    MS_LIMBS,
                    mp_MSMOD);

                pmp3.generate_r1cs_constraints();
                pmp_rbl.generate_r1cs_constraints();
                mul_POE_rbl.generate_r1cs_constraints();
                mul_mod_rbl.generate_r1cs_constraints();

                total_inputs += 1;

                NEW_EXTYPE(quotient, 0)

                // quotient = currendProd // readBatchVal
                mpz_cdiv_q(quotient.get_mpz_t(), currentProd.get_mpz_t(), readBatchVal.get_mpz_t());
                NEW_EXTYPE(mp_acc1, 0)
                // simulate POE cost
                prepare_POE(quotient.get_mpz_t());
                // mp_acc1 = g^quotient
                mpz_powm(mp_acc1.get_mpz_t(), mp_G, quotient.get_mpz_t(), mp_N);

                get_fields_from_mpz(pb, mp_acc1.get_mpz_t(), pb_acc1, MS_LIMBS, mp_MSMOD);
                // pb.val(pb_acc1) = TO_FIELD(mp_acc1);

                pmp3.generate_r1cs_witness();
                pmp_rbl.generate_r1cs_witness();
                mul_POE_rbl.generate_r1cs_witness();
                mul_mod_rbl.generate_r1cs_witness();

                // hack: modify local digests right here because the digest are always updated per batch.

                pbv_local_digest.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
                auto &pb_localdigest = pbv_local_digest.back();
                allocate_pb_multi_scalar(pb, pb_localdigest, MS_LIMBS, "local-digest");
                // pb_localdigest.allocate(pb, "local-digest");

                eq_constraint_multi_scalar(pb, pb_acc1, pb_localdigest, MS_LIMBS);
                // pb.ADD_CONSTRAINT(pb_acc1, 1, pb_localdigest); // now latest local digest inside the circuit equals pi = g^(S/H)
                assign_multi_scalar(pb, pb_localdigest, pb_acc1, MS_LIMBS);
                // pb.val(pb_localdigest) = pb.val(pb_acc1);

                memDigest = mp_acc1;    // update digest in plaintext
                currentProd = quotient; // S <- S / H.

                // pb.val(pb_acc1) = ipow(__LTM_G, quotient);
                readBatchVal = 1;
                pbv_readBatchList.clear();
            }

            // deal with write
            // auto & prev_local_digest = pbv_local_digest.back();

            // pbv_local_digest.push_back(pb_variable<FieldT>());
            // auto & pb_localdigest = pbv_local_digest.back();
            // pb_localdigest.allocate(pb, "local-digest");

            pbv_local_digest.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
            auto &pb_localdigest = pbv_local_digest.back();
            allocate_pb_multi_scalar(pb, pb_localdigest, MS_LIMBS, "local-digest");

            auto &wrSoFar = pbv_wrList.back();

            pow_mod_p_multi_scalar<FieldT> pmp4(
                pb,
                pbv_local_digest[pbv_local_digest.size() - 2],
                mp_N_pbv,
                wrSoFar,
                pb_localdigest,
                POE_BITS,
                MS_LIMBS,
                mp_MSMOD);
            pmp4.generate_r1cs_constraints();

            // at this time, both pbv_local_digest[pbv_local_digest.size() - 2], wrSoFar should be good.

            pmp4.generate_r1cs_witness();

            // accList->push_back(memDigest);
            // prodList->push_back(currentProd);

            // memDigest = ipow(memDigest, batchVal);

            mpz_powm(memDigest.get_mpz_t(), memDigest.get_mpz_t(), batchVal.get_mpz_t(), mp_N);

            // TODOTODO

            // mpz_powm_ui(memDigest, memDigest, batchVal, mp_N);

            currentProd = currentProd * batchVal;
            // mpz_mul_ui(currentProd, currentProd, batchVal);

            // TODO: init values

            // pb.val(pb_localdigest) = ipow(pb.val(prev_local_digest).as_ulong(), pb.val(wrSoFar).as_ulong()) % __LTM_N;

            // pb.val(pb_localdigest) = TO_FIELD(ipow_fp<EX_TYPE>(pb.val(prev_local_digest), pb.val(wrSoFar)));

            // already filled in pmp4 witness
            // pb.val(pb_localdigest) = FieldT(memDigest);
            get_fields_from_mpz(pb, memDigest.get_mpz_t(), pb_localdigest, MS_LIMBS, mp_MSMOD);

            total_inputs += 1; // aux_h2 and new local digest

            // erase pbv_wrList
            pbv_wrList.clear();
            batchVal = 1;

            INC_INT_STATS(time_proverFinishBatch, get_sys_clock() - loopstart);
            // */
        }

        lastTxnID = txnID;

        pbv_val.push_back(pb_variable<FieldT>());
        auto &pb_val = pbv_val.back();
        pb_val.allocate(pb, "val");

        pbv_addr.push_back(pb_variable<FieldT>());
        auto &pb_addr = pbv_addr.back();
        pb_addr.allocate(pb, "addr");

        pb.val(pb_val) = LARGE_DIV_INTRACTABLE_INT + val;
        pb.val(pb_addr) = LARGE_DIV_INTRACTABLE_INT + v_addr;

        total_inputs += 2;

        if (accessType == 0) // RD
        {
            // note that in this part we do not compute H through circuit logic, but rely on the client
            // to check the well-formedness of each multiplier to initReadList or readBatchList.
            if (lwMap.find(v_addr) == lwMap.end())
            {
                pb.ADD_CONSTRAINT(pb_val, 1, LARGE_DIV_INTRACTABLE_INT + 0); // val == 0
                if (pbv_initReadList.size() > 0)
                {
                    // auto & pb_ir_prev = pbv_initReadList.back();
                    pbv_initReadList.push_back(pb_variable<FieldT>());
                    auto &pb_ir = pbv_initReadList.back();
                    pb_ir.allocate(pb, "ir-item");
                    pb.ADD_CONSTRAINT(pbv_initReadList[pbv_initReadList.size() - 2], pb_addr, pb_ir); // accumulate H(addr).
                    pb.val(pb_ir) = pb.val(pbv_initReadList[pbv_initReadList.size() - 2]) * pb.val(pb_addr);
                }
                else
                {
                    pbv_initReadList.push_back(pb_variable<FieldT>());
                    auto &pb_ir = pbv_initReadList.back();
                    pb_ir.allocate(pb, "ir-item");
                    pb.ADD_CONSTRAINT(1, pb_addr, pb_ir); // accumulate this.
                    pb.val(pb_ir) = pb.val(pb_addr);
                }
                total_inputs += 1;
                // hack: to make initReadBatchVal short enough
                // real implementation of PoKE is in future work.
                initReadBatchVal = initReadBatchVal * v_addr;
                // mpz_mul(initReadBatchVal, initReadBatchVal, v_addr);
            }
            else
            {

                mpz_t Hval;
                mpz_init(Hval);
                H_mp(Hval, v_addr, val); // Hval = (I + v_addr) * (I + val) * (I + v_addr + val)
                if (pbv_readBatchList.size() > 0)
                {
                    // auto & pb_rb_prev = pbv_readBatchList.back();
                    // auto prev_ind = pb_rb_prev.index;
                    pbv_readBatchList.push_back(pb_variable<FieldT>());
                    auto &pb_rb = pbv_readBatchList.back();
                    pb_rb.allocate(pb, "rb-item");

                    auto hav = FieldT(Hval);
                    pb.ADD_CONSTRAINT(pbv_readBatchList[pbv_readBatchList.size() - 2], hav, pb_rb); // accumulate this.
                    // pb_rb_prev.index = prev_ind; // HACK
                    pb.val(pb_rb) = pb.val(pbv_readBatchList[pbv_readBatchList.size() - 2]) * hav; // the computation of hash does not need to be in the constraint system as we assume that the verifier is capable of computing the hashes.
                }
                else
                {
                    pbv_readBatchList.push_back(pb_variable<FieldT>());
                    auto &pb_rb = pbv_readBatchList.back();
                    pb_rb.allocate(pb, "rb-item");
                    pb.ADD_CONSTRAINT(1, FieldT(Hval), pb_rb); // accumulate this.
                    pb.val(pb_rb) = FieldT(Hval);
                }
                total_inputs += 1;
                // if(H(v_addr, val) != 0)
                {

                    // readBatchVal = readBatchVal * Hval;

                    mpz_mul(readBatchVal.get_mpz_t(), readBatchVal.get_mpz_t(), Hval);

                    // mpz_mul(readBatchVal, readBatchVal, H(v_addr, val));
                }
            }
            // ri ++;
        }
        else // WR
        {

            pb_variable<FieldT> aux_h2; // we need to encode H because the circuit needs to compute H correctly.
            aux_h2.allocate(pb, "aux_h2");
            pb_variable<FieldT> aux_h3;
            aux_h3.allocate(pb, "aux_h3");

            pb.ADD_CONSTRAINT(pb_addr, pb_val, aux_h3);
            pb.val(aux_h3) = pb.val(pb_addr) * pb.val(pb_val);

            pb.ADD_CONSTRAINT(aux_h3, pb_addr + pb_val - LARGE_DIV_INTRACTABLE_INT, aux_h2);
            pb.val(aux_h2) = pb.val(aux_h3) * (v_addr + val + LARGE_DIV_INTRACTABLE_INT); // pb.val(pb_addr).as_ulong() * val * 2 % __LTM_N;

            // NEW_EXTYPE(aux_h3val, v_addr + LARGE_DIV_INTRACTABLE_INT);
            // mpz_mul(aux_h3val.get_mpz_t(), aux_h3val.get_mpz_t(), (val + LARGE_DIV_INTRACTABLE_INT).get_mpz_t());
            // aux_h3val = aux_h3val * (val + LARGE_DIV_INTRACTABLE_INT);

            mpz_t Hval;
            mpz_init(Hval);
            H_mp(Hval, v_addr, val); // Hval = (I + v_addr) * (I + val) * (I + v_addr + val)

            // NEW_EXTYPE(Hval, v_addr + LARGE_DIV_INTRACTABLE_INT);
            // mpz_mul_ui(Hval.get_mpz_t(), Hval.get_mpz_t(), val + LARGE_DIV_INTRACTABLE_INT);
            // mpz_mul_ui(Hval.get_mpz_t(), Hval.get_mpz_t(), v_addr + val + LARGE_DIV_INTRACTABLE_INT);

            if (pbv_wrList.size() > 0)
            {
                // auto & pb_wr_prev = pbv_wrList.back();
                pbv_wrList.push_back(pb_variable<FieldT>());
                auto &pb_wr = pbv_wrList.back();
                pb_wr.allocate(pb, "wr-item");
                pb.ADD_CONSTRAINT(pbv_wrList[pbv_wrList.size() - 2], aux_h2, pb_wr); // accumulate this.
                pb.val(pb_wr) = pb.val(pbv_wrList[pbv_wrList.size() - 2]) * pb.val(aux_h2);
            }
            else
            {
                pbv_wrList.push_back(pb_variable<FieldT>());
                auto &pb_wr = pbv_wrList.back();
                pb_wr.allocate(pb, "wr-item");
                pb.ADD_CONSTRAINT(1, aux_h2, pb_wr); // accumulate this.
                pb.val(pb_wr) = pb.val(aux_h2);
            }

            total_inputs += 2; // aux_h2 and aux_h3

            // batchVal = batchVal * Hval;
            mpz_mul(batchVal.get_mpz_t(), batchVal.get_mpz_t(), Hval);

            lwMap[v_addr] = memCounter;
            memCounter++;
            // wi++;
        }
        INC_INT_STATS(time_proverProcessTxn, get_sys_clock() - loopstart);
    }

    // after everything:

    if (pbv_wrList.size() > 0)
    {
        if (pbv_initReadList.size() > 0)
        {
            pbv_A.push_back(pb_variable<FieldT>());
            auto &pb_A = pbv_A.back();
            pb_A.allocate(pb, "A");

            pbv_B.push_back(pb_variable<FieldT>());
            auto &pb_B = pbv_B.back();
            pb_B.allocate(pb, "B");

            vector<pb_variable<FieldT>> pb_A_Q(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_B_Q(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_A_Q_raised(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_B_Q_raised(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux1_POE(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux1_premod(2 * MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux1_q(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux2_POE(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux2_premod(2 * MS_LIMBS);
            vector<pb_variable<FieldT>> pb_aux2_q(MS_LIMBS);

            pb_variable<FieldT> readlist_exp_pbv;
            readlist_exp_pbv.allocate(pb, "readlist_exp_pbv");

            total_inputs += 2;
            // uint32_t A, B;
            // xgcd(currentProd, initReadBatchVal, A, B);
            NEW_EXTYPE(A, 0)
            NEW_EXTYPE(B, 0)
            NEW_EXTYPE(_g, 0)
            mpz_gcdext(_g.get_mpz_t(), A.get_mpz_t(), B.get_mpz_t(), currentProd.get_mpz_t(), initReadBatchVal.get_mpz_t());

            pb.val(pb_A) = TO_FIELD(A);
            pb.val(pb_B) = TO_FIELD(B);

            vector<pb_variable<FieldT>> aux1(MS_LIMBS), aux2(MS_LIMBS), aux3(MS_LIMBS), aux4(MS_LIMBS);

            for (uint32_t idx = 0; idx < MS_LIMBS; idx++)
            {
                aux1[idx].allocate(pb, "aux1");
                aux2[idx].allocate(pb, "aux2");
                aux3[idx].allocate(pb, "aux3");
                aux4[idx].allocate(pb, "aux4");
                pb_A_Q[idx].allocate(pb, "pb_A_Q");
                pb_B_Q[idx].allocate(pb, "pb_B_Q");
                pb_A_Q_raised[idx].allocate(pb, "pb_A_Q_raised");
                pb_B_Q_raised[idx].allocate(pb, "pb_B_Q_raised");
                pb_aux1_POE[idx].allocate(pb, "pb_aux1_POE");
                pb_aux2_POE[idx].allocate(pb, "pb_aux2_POE");
                pb_aux1_q[idx].allocate(pb, "pb_aux1_q");
                pb_aux2_q[idx].allocate(pb, "pb_aux2_q");
            }

            allocate_pb_multi_scalar(pb, pb_aux1_premod, 2 * MS_LIMBS, "pb_aux1_premod");
            allocate_pb_multi_scalar(pb, pb_aux2_premod, 2 * MS_LIMBS, "pb_aux2_premod");

            // aux1 = pbv_local_digest.back() ^ pb_A

            // simulate computation cost of server-side PoE preparation
            prepare_POE(A.get_mpz_t());
            prepare_POE(B.get_mpz_t());
            // TODO: set values for pb_A_Q, pb_A_Q_raised, pb_aux1_POE, pb_B_Q, pb_B_Q_raised, pb_aux2_POE

            pow_mod_p_multi_scalar<FieldT> pmp(pb, pbv_local_digest.back(), mp_N_pbv, pb_A, pb_aux1_POE, POE_BITS, MS_LIMBS, mp_MSMOD);
            pow_mod_p_multi_scalar<FieldT> pmp_POE_A(pb, pb_A_Q, mp_N_pbv, mp_L_pb, pb_A_Q_raised, POE_BITS, MS_LIMBS, mp_MSMOD);
            mul_multi_scalar<FieldT> mul_POE_A(pb, pb_A_Q_raised, pb_aux1_POE, pb_aux1_premod, MS_LIMBS, mp_MSMOD);
            mod_p_multi_scalar<FieldT> mul_mod_A(pb, pb_aux1_premod, mp_N_pbv, pb_aux1_q, aux1, POE_BITS, MS_LIMBS, mp_MSMOD);
            pmp.generate_r1cs_constraints();
            pmp_POE_A.generate_r1cs_constraints();
            mul_POE_A.generate_r1cs_constraints();
            mul_mod_A.generate_r1cs_constraints();
            // aux2 = g^(pbv_initReadList.back() * pb_B)

            pb.ADD_CONSTRAINT(pbv_initReadList.back(), pb_B, readlist_exp_pbv);

            pb.val(pbv_initReadList.back()).as_bigint().to_mpz(init_read_list_back);
            mpz_mul(readlist_exponent, B.get_mpz_t(), init_read_list_back);
            mpz_mod(readlist_exponent, readlist_exponent, mp_POE_L); // PoE hack
            pb.val(readlist_exp_pbv) = FieldT(readlist_exponent);
            pow_mod_p_multi_scalar<FieldT> pmp2(pb, pb_G, mp_N_pbv, readlist_exp_pbv, pb_aux2_POE, POE_BITS, MS_LIMBS, mp_MSMOD);
            pow_mod_p_multi_scalar<FieldT> pmp_POE_B(pb, pb_B_Q, mp_N_pbv, mp_L_pb, pb_B_Q_raised, POE_BITS, MS_LIMBS, mp_MSMOD);
            mul_multi_scalar<FieldT> mul_POE_B(pb, pb_B_Q_raised, pb_aux2_POE, pb_aux2_premod, MS_LIMBS, mp_MSMOD);
            mod_p_multi_scalar<FieldT> mul_mod_B(pb, pb_aux2_premod, mp_N_pbv, pb_aux2_q, aux2, POE_BITS, MS_LIMBS, mp_MSMOD);

            pmp2.generate_r1cs_constraints();
            pmp_POE_B.generate_r1cs_constraints();
            mul_POE_B.generate_r1cs_constraints();
            mul_mod_B.generate_r1cs_constraints();

            // due to integer intractability assumption, the gcd might not be 1
            // in future version we will move to real primes.

            // pb.ADD_CONSTRAINT(aux1, aux2, aux3);
            mod_p_multi_scalar<FieldT> g(pb, aux3, mp_N_pbv, aux4, pb_G, POE_BITS, MS_LIMBS, mp_MSMOD);

            g.generate_r1cs_constraints();

            NEW_EXTYPE(mp_aux1, 0)
            NEW_EXTYPE(mp_aux2, 0)
            NEW_EXTYPE(mp_aux3, 0)
            NEW_EXTYPE(mp_aux4, 0)

            mpz_powm(mp_aux1.get_mpz_t(), memDigest.get_mpz_t(), A.get_mpz_t(), mp_N);
            mpz_powm(mp_aux2.get_mpz_t(), initReadBatchVal.get_mpz_t(), B.get_mpz_t(), mp_N);
            mpz_mul(mp_aux3.get_mpz_t(), mp_aux1.get_mpz_t(), mp_aux2.get_mpz_t());
            mpz_cdiv_q(mp_aux4.get_mpz_t(), mp_aux3.get_mpz_t(), mp_N);

            get_fields_from_mpz(pb, mp_aux1.get_mpz_t(), aux1, MS_LIMBS, mp_MSMOD);
            get_fields_from_mpz(pb, mp_aux2.get_mpz_t(), aux2, MS_LIMBS, mp_MSMOD);
            get_fields_from_mpz(pb, mp_aux3.get_mpz_t(), aux3, MS_LIMBS, mp_MSMOD);
            get_fields_from_mpz(pb, mp_aux4.get_mpz_t(), aux4, MS_LIMBS, mp_MSMOD);

            pmp.generate_r1cs_witness();
            pmp_POE_A.generate_r1cs_witness();
            mul_POE_A.generate_r1cs_witness();
            mul_mod_A.generate_r1cs_witness();

            pmp2.generate_r1cs_witness();
            pmp_POE_B.generate_r1cs_witness();
            mul_POE_B.generate_r1cs_witness();
            mul_mod_B.generate_r1cs_witness();

            g.generate_r1cs_witness();

            total_inputs += 4;

            pbv_initReadList.clear();
            initReadBatchVal = 1;
        }

        // normal read batch
        if (pbv_readBatchList.size() > 0)
        {
            pbv_acc1.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
            auto &pb_acc1 = pbv_acc1.back();
            allocate_pb_multi_scalar(pb, pb_acc1, MS_LIMBS, "acc1");

            // acc1^pbv_readBatchList = pbv_localdigest
            vector<pb_variable<FieldT>> pb_rbl_Q(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_rbl_Q_raised(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_localDigest_POE(MS_LIMBS);
            vector<pb_variable<FieldT>> pb_localDigest_premod(2 * MS_LIMBS);
            vector<pb_variable<FieldT>> pb_localDigest_q(MS_LIMBS);

            allocate_pb_multi_scalar(pb, pb_rbl_Q, MS_LIMBS, "pb_rbl_Q");
            allocate_pb_multi_scalar(pb, pb_rbl_Q_raised, MS_LIMBS, "pb_rbl_Q_raised");
            allocate_pb_multi_scalar(pb, pb_localDigest_POE, MS_LIMBS, "pb_localDigest_POE");
            allocate_pb_multi_scalar(pb, pb_localDigest_premod, MS_LIMBS, "pb_localDigest_premod");
            allocate_pb_multi_scalar(pb, pb_localDigest_q, MS_LIMBS, "pb_localDigest_q");

            pow_mod_p_multi_scalar<FieldT> pmp_rbl(
                pb,
                pb_rbl_Q,
                mp_N_pbv,
                mp_L_pb,
                pb_rbl_Q_raised,
                POE_BITS,
                MS_LIMBS,
                mp_MSMOD);

            pow_mod_p_multi_scalar<FieldT> pmp3(
                pb,
                pb_acc1,
                mp_N_pbv,
                pbv_readBatchList.back(),
                pb_localDigest_POE,
                POE_BITS,
                MS_LIMBS,
                mp_MSMOD);

            mul_multi_scalar<FieldT> mul_POE_rbl(
                pb,
                pb_rbl_Q_raised,
                pb_localDigest_POE,
                pb_localDigest_premod,
                MS_LIMBS,
                mp_MSMOD);

            mod_p_multi_scalar<FieldT> mul_mod_rbl(
                pb,
                pb_localDigest_premod,
                mp_N_pbv,
                pb_localDigest_q,
                pbv_local_digest.back(),
                POE_BITS,
                MS_LIMBS,
                mp_MSMOD);

            pmp3.generate_r1cs_constraints();
            pmp_rbl.generate_r1cs_constraints();
            mul_POE_rbl.generate_r1cs_constraints();
            mul_mod_rbl.generate_r1cs_constraints();

            total_inputs += 1;

            NEW_EXTYPE(quotient, 0)

            // quotient = currendProd // readBatchVal
            mpz_cdiv_q(quotient.get_mpz_t(), currentProd.get_mpz_t(), readBatchVal.get_mpz_t());
            NEW_EXTYPE(mp_acc1, 0)
            // simulate POE cost
            prepare_POE(quotient.get_mpz_t());
            // mp_acc1 = g^quotient
            mpz_powm(mp_acc1.get_mpz_t(), mp_G, quotient.get_mpz_t(), mp_N);

            get_fields_from_mpz(pb, mp_acc1.get_mpz_t(), pb_acc1, MS_LIMBS, mp_MSMOD);
            // pb.val(pb_acc1) = TO_FIELD(mp_acc1);

            pmp3.generate_r1cs_witness();
            pmp_rbl.generate_r1cs_witness();
            mul_POE_rbl.generate_r1cs_witness();
            mul_mod_rbl.generate_r1cs_witness();

            // hack: modify local digests right here because the digest are always updated per batch.

            pbv_local_digest.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
            auto &pb_localdigest = pbv_local_digest.back();
            allocate_pb_multi_scalar(pb, pb_localdigest, MS_LIMBS, "local-digest");
            // pb_localdigest.allocate(pb, "local-digest");

            eq_constraint_multi_scalar(pb, pb_acc1, pb_localdigest, MS_LIMBS);
            // pb.ADD_CONSTRAINT(pb_acc1, 1, pb_localdigest); // now latest local digest inside the circuit equals pi = g^(S/H)
            assign_multi_scalar(pb, pb_localdigest, pb_acc1, MS_LIMBS);
            // pb.val(pb_localdigest) = pb.val(pb_acc1);

            memDigest = mp_acc1;    // update digest in plaintext
            currentProd = quotient; // S <- S / H.

            // pb.val(pb_acc1) = ipow(__LTM_G, quotient);
            readBatchVal = 1;
            pbv_readBatchList.clear();
        }

        // deal with write
        // auto & prev_local_digest = pbv_local_digest.back();

        pbv_local_digest.push_back(vector<pb_variable<FieldT>>(MS_LIMBS));
        auto &pb_localdigest = pbv_local_digest.back();
        allocate_pb_multi_scalar(pb, pb_localdigest, MS_LIMBS, "local-digest");
        // pb_localdigest.allocate(pb, "local-digest");

        auto &wrSoFar = pbv_wrList.back();

        pow_mod_p_multi_scalar<FieldT> pmp4(
            pb,
            pbv_local_digest[pbv_local_digest.size() - 2],
            mp_N_pbv,
            wrSoFar,
            pb_localdigest,
            POE_BITS,
            MS_LIMBS,
            mp_MSMOD);

        pmp4.generate_r1cs_constraints();

        // at this time, both pbv_local_digest[pbv_local_digest.size() - 2], wrSoFar should be good.

        pmp4.generate_r1cs_witness();

        // pmp4.debug(cout);

        // accList->push_back(memDigest);
        // prodList->push_back(currentProd);

        // memDigest = ipow(memDigest, batchVal);

        mpz_powm(memDigest.get_mpz_t(), memDigest.get_mpz_t(), batchVal.get_mpz_t(), mp_N);

        // TODOTODO

        // mpz_powm_ui(memDigest, memDigest, batchVal, mp_N);

        currentProd = currentProd * batchVal;
        // mpz_mul_ui(currentProd, currentProd, batchVal);

        // TODO: init values

        // pb.val(pb_localdigest) = ipow(pb.val(prev_local_digest).as_ulong(), pb.val(wrSoFar).as_ulong()) % __LTM_N;

        // pb.val(pb_localdigest) = TO_FIELD(ipow_fp<EX_TYPE>(pb.val(prev_local_digest), pb.val(wrSoFar)));

        // already filled in pmp4 witness
        get_fields_from_mpz(pb, memDigest.get_mpz_t(), pb_localdigest, MS_LIMBS, mp_MSMOD);
        // pb.val(pb_localdigest) = TO_FIELD(memDigest);

        total_inputs += 1; // aux_h2 and new local digest

        // erase pbv_wrList
        pbv_wrList.clear();
        batchVal = 1;
        // */
    }

#else // Merkle Tree

    cout << "Generating Constraints..." << endl;

    for (uint32_t ind = 0; ind < opLength; ind++)
    {
        uint64_t loopstart = get_sys_clock();

        uint32_t val = pta->values[ind];
        uint32_t v_addr = pta->addrs[ind];
        uint32_t txnID = pta->txnIDs[ind] >> 1;
        uint32_t accessType = pta->txnIDs[ind] & 1;

        if (accessType == 0) // RD
        {
            // generate proof
            std::vector<merkle_authentication_node> path(tree_depth);
            uint32_t addr = v_addr;
            libff::bit_vector leaf, address_bits(tree_depth);

            leaf = levels[tree_depth - 1][addr];

            for (int i = 0; i < tree_depth; i++)
            {
                uint32_t tmp = (addr & 0x01);
                address_bits[i] = tmp;
                addr = addr / 2;
                // std::cout << address_bits[tree_depth-1-i] << std::endl;
            }

            // Fill in the path
            size_t index = v_addr;
            for (int i = tree_depth - 1; i >= 0; i--)
            {
                path[i] = address_bits[tree_depth - 1 - i] == 0 ? levels[i][index + 1] : levels[i][index - 1];
                index = index / 2;
            }

            sample::MerkleCircuit<FieldT, HashT> mc(pb, tree_depth);
            mc.generate_r1cs_constraints();
            mc.generate_r1cs_witness(pb, leaf, root, path, addr, address_bits);
        }
        else
        {
            // update the merkle tree

            std::vector<merkle_authentication_node> prev_path(tree_depth);
            libff::bit_vector prev_load_hash = levels[tree_depth - 1][v_addr];
            libff::bit_vector prev_store_hash = hash256<HashT>(to_string(lwMap[v_addr] * 2));

            libff::bit_vector loaded_leaf = prev_load_hash;
            libff::bit_vector stored_leaf = prev_store_hash;

            libff::bit_vector address_bits(tree_depth);

            uint32_t addr = v_addr;
            for (int i = 0; i < tree_depth; i++)
            {
                uint32_t tmp = (addr & 0x01);
                address_bits[i] = tmp;
                addr = addr / 2;
                // std::cout << address_bits[tree_depth-1-i] << std::endl;
            }

            uint32_t index = v_addr;

            for (int i = tree_depth - 1; i >= 0; i--)
            {
                bool computed_is_right = address_bits[tree_depth - 1 - i] == 0;
                libff::bit_vector other = computed_is_right ? levels[i][index + 1] : levels[i][index - 1];

                prev_path[i] = other;

                libff::bit_vector load_block = prev_load_hash;
                load_block.insert(computed_is_right ? load_block.begin() : load_block.end(), other.begin(), other.end());
                libff::bit_vector store_block = prev_store_hash;
                store_block.insert(computed_is_right ? store_block.begin() : store_block.end(), other.begin(), other.end());

                libff::bit_vector load_h = HashT::get_hash(load_block);
                libff::bit_vector store_h = HashT::get_hash(store_block);

                prev_load_hash = load_h;
                prev_store_hash = store_h;

                levels[i][index] = store_h; // update the path

                index = index / 2;
            }

            libff::bit_vector load_root = prev_load_hash;
            libff::bit_vector store_root = prev_store_hash;

            pb_variable_array<FieldT> address_bits_va;
            address_bits_va.allocate(pb, tree_depth, "address_bits");
            digest_variable<FieldT> prev_leaf_digest(pb, digest_len, "prev_leaf_digest");
            digest_variable<FieldT> prev_root_digest(pb, digest_len, "prev_root_digest");
            merkle_authentication_path_variable<FieldT, HashT> prev_path_var(pb, tree_depth, "prev_path_var");
            digest_variable<FieldT> next_leaf_digest(pb, digest_len, "next_leaf_digest");

            digest_variable<FieldT> next_root_digest(pb, digest_len, "next_root_digest");

            merkle_authentication_path_variable<FieldT, HashT> next_path_var(pb, tree_depth, "next_path_var");
            merkle_tree_check_update_gadget<FieldT, HashT> mls(pb, tree_depth, address_bits_va,
                                                               prev_leaf_digest, prev_root_digest, prev_path_var,
                                                               next_leaf_digest, next_root_digest, next_path_var, ONE, "mls");
            prev_path_var.generate_r1cs_constraints();
            mls.generate_r1cs_constraints();

            address_bits_va.fill_with_bits(pb, address_bits);
            // assert(address_bits_va.get_field_element_from_bits(pb).as_ulong() == address);
            prev_leaf_digest.generate_r1cs_witness(loaded_leaf);
            prev_path_var.generate_r1cs_witness(addr, prev_path);
            next_leaf_digest.generate_r1cs_witness(stored_leaf);
            address_bits_va.fill_with_bits(pb, address_bits);
            mls.generate_r1cs_witness();

            prev_leaf_digest.generate_r1cs_witness(loaded_leaf);
            next_leaf_digest.generate_r1cs_witness(stored_leaf);
            prev_root_digest.generate_r1cs_witness(load_root);
            next_root_digest.generate_r1cs_witness(store_root);

            for (size_t i = 0; i < digest_len; i++)
            {
                // chain
                pb.ADD_CONSTRAINT(prev_root_digest.bits[i], 1, pbv_roots.back().bits[i]);
            }

            root = store_root;
            pbv_roots.push_back(next_root_digest);
        }
    }

#endif

    uint64_t tt2 = get_sys_clock();
    INC_INT_STATS(time_proverProcessBatches, tt2 - tt);

    cout << "Generating Keys..." << endl;

    //_mm_free(accList);
    //_mm_free(prodList);
    //_mm_free(latestWrittenIndex);

    pb.set_input_sizes(total_inputs); // TODO: change this

    const r1cs_constraint_system<FieldT> constraint_system = pb.get_constraint_system();

    uint64_t tt3 = get_sys_clock();
    INC_INT_STATS(time_generateConstraintSystem, tt3 - tt2);

    r1cs_ppzksnark_keypair<default_r1cs_ppzksnark_pp> keypair = r1cs_ppzksnark_generator<default_r1cs_ppzksnark_pp>(constraint_system);

    INC_INT_STATS(int_comm_cost, (keypair.pk.size_in_bits() + keypair.vk.size_in_bits()) / 8);

    uint64_t tt4 = get_sys_clock();
    INC_INT_STATS(time_generateKey, tt4 - tt3);

    cout << "Generating Proofs..." << endl;

    r1cs_ppzksnark_proof<default_r1cs_ppzksnark_pp> proof = r1cs_ppzksnark_prover<default_r1cs_ppzksnark_pp>(keypair.pk, pb.primary_input(), pb.auxiliary_input());

    uint64_t tt5 = get_sys_clock();
    INC_INT_STATS(time_generateProof, tt5 - tt4);

    cout << "Verification..." << endl;

    bool verified = r1cs_ppzksnark_verifier_strong_IC<default_r1cs_ppzksnark_pp>(keypair.vk, pb.primary_input(), proof);

    uint64_t tt6 = get_sys_clock();
    INC_INT_STATS(time_verify, tt6 - tt5);

    std::ofstream proof_file("verification/proofs/litmus-proof" + to_string(pta->id));
    proof_file << proof;

    INC_INT_STATS(int_comm_cost, proof_file.tellp());

    proof_file.close();

    INC_INT_STATS(time_outputProof, get_sys_clock() - tt6);

    INC_INT_STATS(time_latency_verify, (get_sys_clock() - starttime) * opLength);
    INC_INT_STATS(int_latency_num, opLength);

    return NULL;
}

void proveAll()
{
#if false // glued-together, deprecated
#if SKIP_MAT_GEN
        {
            {
                std::ifstream tmA("../pequin/pepper/bin/wrapped_transaction.qap.matrix_a"), tmB("../pequin/pepper/bin/wrapped_transaction.qap.matrix_b"), tmC("../pequin/pepper/bin/wrapped_transaction.qap.matrix_c"), toutWorksheet("../pequin/pepper/bin/wrapped_transaction.pws"), tmParams("../pequin/pepper/bin/wrapped_transaction.params"), tmQAP("../pequin/pepper/bin/wrapped_transaction.qap");

                std::stringstream tmAstr, tmBstr, tmCstr, toutWorksheetstr, tmParamsstr, tmQAPstr;

                tmAstr << tmA.rdbuf();
                tmBstr << tmB.rdbuf();
                tmCstr << tmC.rdbuf();
                toutWorksheetstr << toutWorksheet.rdbuf();
                tmParamsstr << tmParams.rdbuf();
                tmQAPstr << tmQAP.rdbuf();

                std::string outWorkSheet = toutWorksheetstr.str(),
                 mA = tmAstr.str(),
                 mB = tmBstr.str(),
                 mC = tmCstr.str(),
                 mParams = tmParamsstr.str(),
                 mQAP = tmQAPstr.str();
                //char *outWorkSheet = (char*)outWorkSheetStr.c_str();

#else
    // check the JVM
    auto env = &g_jvm.env;
    auto cls = (*env)->FindClass("zcc/ZCC");
        if(cls !=0)
        {
            auto mid = (*env)->GetStaticMethodID(cls, "shortcut", "(Ljava/lang/String;)[Ljava/lang/String;");
            //auto mid = (*env)->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
            if(mid !=0)
            {
                // frontend
                cout << "Source code Length in C++: " << strlen(cCode) << endl;
#if OUTPUT_SOURCE
                ofstream fout("code.c");
                fout << cCode << endl;
                fout.close();
#endif
                auto arg = (*env)->NewStringUTF(cCode);
                /*auto stringClass = (*env)->FindClass("java/lang/String");
                auto args = (*env)->NewObjectArray(3, stringClass, NULL);
                auto arg0 = (*env)->NewStringUTF("-OZAATAR");
                auto arg1 = (*env)->NewStringUTF("ctwa");
                auto arg2 = (*env)->NewStringUTF("../pequin/pepper/apps/wrapped_transaction.c");
                (*env)->SetObjectArrayElement(args, 0, arg0);
                (*env)->SetObjectArrayElement(args, 1, arg1);
                (*env)->SetObjectArrayElement(args, 2, arg2);*/

                //(*env)->CallStaticVoidMethod(cls, mid, args);
                jobjectArray ret = (jobjectArray)(*env)->CallStaticObjectMethod(cls, mid, arg);
                int size = (*env)->GetArrayLength(ret);
                assert(size==4);
                char * retContent[4];
                // ret 0 - circuit (.circuit), 1 - constants (.cons), 2 - uncleaned constraints (.spec_tmp), 3 - cleaned analysis constraints (.spec)
                //https://stackoverflow.com/questions/19591873/get-an-array-of-strings-from-java-to-c-jni
                for (int i=0; i < size; ++i) 
                {
                    jstring string = (jstring)(*env)->GetObjectArrayElement(ret, i);
                    const char* string_str = (*env)->GetStringUTFChars(string, 0);
                    retContent[i] = (char*)_mm_malloc(strlen(string_str), ALIGN_SIZE);
                    strcpy(retContent[i], string_str);
                    (*env)->ReleaseStringUTFChars(string, string_str);
                    (*env)->DeleteLocalRef(string);
                }
#if OUTPUT_CIRCUIT
                ofstream fout_circuit("wrapped.circuit");
                fout_circuit << retContent[0];
                fout_circuit.close();
#endif
                /// Now backend
                // TODO: move the following part to init
                PyObject *pName = PyBytes_FromString("pyBridge");
                PyObject *pModule = PyImport_Import(pName);
                if(pModule == NULL)
                    cout << "pModule NULL" << endl;
                PyObject *pDict = PyModule_GetDict(pModule);
                PyObject *pFunc = PyDict_GetItemString(pDict, "shortcut");
                // ------
                uint64_t time_py_start = get_sys_clock();
                PyObject *res = PyObject_CallFunction(pFunc, "sss", retContent[0], retContent[1], retContent[3]);
                uint64_t time_py = get_sys_clock() - time_py_start;
                cout << "Python time " << time_py / CPU_FREQ / 1e9 << endl;
                PyObject *ptype, *pvalue, *ptraceback;
                PyErr_Print();
                PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                //pvalue contains error message
                //ptraceback contains stack snapshot and many other information
                //(see python traceback structure)
                if(pvalue!=NULL)
                {
                    //Get error message
                    char *pStrErrorMessage = PyString_AsString(pvalue);
                    PyObject* pRepr = PyObject_Repr(ptraceback) ;
                    cout << pStrErrorMessage << endl;
                }
                //outWorkSheet, mA, mB, mC, mParams, mQAP
                assert(PyTuple_Check(res));
                assert(PyTuple_Size(res)==6);
                
                //assert(res!=NULL);
                //printf("Result of intMethod: %d\n", square);

                // run the verifier setup

                std::string outWorkSheet(POS_TO_STR(0)), mA(POS_TO_STR(1)), mB(POS_TO_STR(2)), mC(POS_TO_STR(3)), mParams(POS_TO_STR(4)), mQAP(POS_TO_STR(5));
#endif

                cout << "mParams " << mParams << endl;
                constexpr auto shared_dir="../pequin/pepper/prover_verifier_shared/";
                constexpr auto v_dir = "../pequin/pepper/verification_material/";
                constexpr auto p_dir = "../pequin/pepper/proving_material/";

                std::stringstream pkStr;
                std::stringstream vkStr;
#define SKIP_KEY_GEN false
                string verification_key_fn = std::string(v_dir) + "wrapped_transaction.vkey";
                string proving_key_fn = std::string(p_dir) + "wrapped_transaction.pkey";
                struct comp_params p = parse_params_str(mParams);
                mpz_t prime;
                mpz_init_set_str(prime, "21888242871839275222246405745257275088548364400416034343698204186575808495617", 10);
                // merge code in pepper_verifier.cpp
                {
                    // verifier setup

                    //struct comp_params p = parse_params("../pequin/pepper/bin/" + string(NAME) + ".params");
                    
                    

                    
                    string unprocessed_vkey_fn;
                    
                    std::cout << "Creating proving/verification keys, will write to " << verification_key_fn
                            << ", " << proving_key_fn << std::endl;

#if !SKIP_KEY_GEN
                    
                    run_setup(p.n_constraints, p.n_inputs, p.n_outputs, p.n_vars, prime, verification_key_fn, proving_key_fn, unprocessed_vkey_fn, mA, mB, mC, vkStr, pkStr);
#endif

                    std::cout << "Finished key generation" << std::endl;
                }
                //struct comp_params p = parse_params_str(POS_TO_STR(4));
                // run the prover setup
                {
                    
                    std::cout << "NUMBER OF CONSTRAINTS:  " << p.n_constraints << std::endl;

                    ComputationProver prover(p.n_vars, p.n_constraints, p.n_inputs, p.n_outputs, prime, "default_shared_db", "", true, false);
                }
                
                
                // run the actual proving
                {

                    std::cout << "NUMBER OF CONSTRAINTS:  " << p.n_constraints << std::endl;

                    std::string input_fn = string(shared_dir) + "wrapped_transaction.inputs";

                    // also inputs are here

                    ComputationProver prover(p.n_vars, p.n_constraints, p.n_inputs, p.n_outputs, prime, "default_shared_db", input_fn, false, false, inputList);
                    pws_length = outWorkSheet.size();
                    pws_progress = 0;

                    uint64_t time_pws_start = get_sys_clock();

                    prover.compute_from_pws_strV2(outWorkSheet.c_str());
                    
                    //prover.compute_from_pws("../pequin/pepper/bin/wrapped_transaction.pws");
                    
                    uint64_t time_pws = get_sys_clock() - time_pws_start;
                    cout << "pws time " << time_pws / CPU_FREQ / 1e9 << endl;

                    libsnark::default_r1cs_gg_ppzksnark_pp::init_public_params();

                    libsnark::r1cs_gg_ppzksnark_primary_input<libsnark::default_r1cs_gg_ppzksnark_pp> primary_input;
                    libsnark::r1cs_gg_ppzksnark_auxiliary_input<libsnark::default_r1cs_gg_ppzksnark_pp> aux_input;

                    for (int i = 0; i < p.n_inputs; i++) {
                        FieldT currentVar(prover.input[i]);
                        primary_input.push_back(currentVar);
                    }

                    for (int i = 0; i < p.n_outputs; i++) {
                        FieldT currentVar(prover.output[i]);
                        primary_input.push_back(currentVar);
                    }

                    for (int i = 0; i < p.n_vars; i++) {
                        FieldT currentVar(prover.F1[i]);
                        aux_input.push_back(currentVar);
                    }

                    std::string pk_filename = string(p_dir) + "wrapped_transaction.pkey";
                    std::string output_fn = string(shared_dir) + "wrapped_transaction.outputs";
                    std::string proof_fn = string(shared_dir) + "wrapped_transaction.proof";

                    libsnark::r1cs_gg_ppzksnark_keypair<libsnark::default_r1cs_gg_ppzksnark_pp> keypair;
                    std::cout << "reading proving key from file..." << std::endl;
#if SKIP_KEY_GEN                    
                    ifstream pkF(proving_key_fn);
                    pkF >> keypair.pk;
                    pkF.close();

#else
                    pkStr >> keypair.pk;
#endif

                    libff::start_profiling();
                    libsnark::r1cs_gg_ppzksnark_proof<libsnark::default_r1cs_gg_ppzksnark_pp> proof = libsnark::r1cs_gg_ppzksnark_prover<libsnark::default_r1cs_gg_ppzksnark_pp>(keypair.pk, primary_input, aux_input);

                    // Now `proof` is the resulting proof
                    
                    std::ofstream proof_file(proof_fn);
                    proof_file << proof; 
                    proof_file.close();
                    
                    std::cout << "Circuit Evaluation:" << std::endl;
                    std::ofstream output_file(output_fn);
                    for (int i = 0; i < p.n_outputs; i++) {
                        output_file << prover.input_output_q[p.n_inputs + i] << std::endl;

                        std::cout << prover.input_output_q[p.n_inputs + i]<< std::endl;
                    }
                    output_file.close();
                }
                // finish

            }

        }
        
    
    // prepare the params
    
    // java -cp frontend/bin/ zcc.ZCC --verbose ctw ../pepper/apps/wrapped_transaction.c

    // build the verifier

    // these are glues
    //*/
    /*
    int ret = system(("cd " + PEPPER_DIR + " && sh test.sh wrapped_transaction").c_str());
    if(ret!=0)
    {
        printf("Error in proving\n");
    }
    //*/
#endif
}

#endif