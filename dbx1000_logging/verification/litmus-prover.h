#pragma once

#include "config.h"
#include "global.h"
#include "manager.h"
#include <jni.h>
#include <Python.h>
#include <gmpxx.h>
//#include <NTL/ZZ.h>

#include <vector>
#include <map>

void outputElle();

#if VERIFICATION


#define CURVE_BN128 // 128 bit security
#define NAME "wrapped_transaction"
#define DB_HASH_NUM_BITS 256

#include <libff/algebra/fields/bigint.hpp>
#include <libff/common/utils.hpp>
#include "merkle_tree_circuit.h"

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;

#define N_BITS 128 // 256 // 64

#define N_128BIT "283759148081449791630669252667505103767"
#define BN128_ORDER "21888242871839275222246405745257275088696311157297823662689037894645226208582"
//#define FF_TYPE ZZ_p
//#define FF_TYPE uint128_t
//#define FF_INIT_2INT64(x, y) ((uint128_t(x) << 64) + y)
//#define EX_TYPE mpz_t
#define EX_TYPE mpz_class

#define TO_FIELD convertToField

extern mpz_t mp_BN128_ORDER;

#define NEW_EXTYPE(x, y) mpz_class x(y);
//#define NEW_EXTYPE(x, y) mpz_t x; mpz_init(x); mpz_set_ui(x, y);


extern uint32_t litmus_lock;
extern vector<uint32_t> *inputList;
extern mpz_t mp_N;

//extern FF_TYPE * HashPrimes;

//FF_TYPE intractibleHash(uint64_t x);

void prove(txn_man *);
void proveAll();
void proveInteractive();
void proveInteractiveMerkleTree();
void proveHandWritten();
void * proveHandWrittenThread(void * vpta);


void parseTraces();

#define LARGE_DIV_INTRACTABLE_INT 7758800764337290897

// TODO: PoKE implementation
#define POKE_TRUNCATE_INT 18446744073709551557

#define H_addr(addr) (addr + LARGE_DIV_INTRACTABLE_INT)
#define H(addr, val) ((addr + LARGE_DIV_INTRACTABLE_INT)*(val)) // dummy hash function for now

/*
inline EX_TYPE H(uint32_t addr, uint32_t val) //, mpz_t ret)
{
    NEW_EXTYPE(tmp, LARGE_DIV_INTRACTABLE_INT + addr)
    //mpz_mul_ui(tmp, tmp, (LARGE_DIV_INTRACTABLE_INT + val));
    //mpz_mul_ui(tmp, tmp, (LARGE_DIV_INTRACTABLE_INT + tmp + val));
    //return tmp;
    return tmp * (LARGE_DIV_INTRACTABLE_INT + val) * (LARGE_DIV_INTRACTABLE_INT + tmp + val);
    // (I+addr)*(I+val)*(I+addr+val)
    // H(addr) * H(val) * H2(addr+val)
}
*/

inline void H_mp(mpz_t target, uint32_t addr, uint32_t val)
{
    mpz_set_ui(target, LARGE_DIV_INTRACTABLE_INT + addr);
    mpz_mul_ui(target, target, (LARGE_DIV_INTRACTABLE_INT + val));
    mpz_mul_ui(target, target, (LARGE_DIV_INTRACTABLE_INT + addr + val));
    return;
}



//#define H(addr, val) (mpz_t ret, _beforehash(addr, val, ret), ret)



enum Mode {proof, only_setup, witness_export };

extern vector<pthread_t> verifyWorkers;

struct ProverThreadArg{
	EX_TYPE initDigest;
	map<uint32_t, uint32_t> initMap;
	vector<uint32_t> values;
	vector<uint32_t> addrs;
	vector<uint32_t> txnIDs; // the LSB indicates READ-0 / WRITE-1
	uint32_t id;
    //libff::bit_vector merkleroot;
	ProverThreadArg(){}
};



#endif
