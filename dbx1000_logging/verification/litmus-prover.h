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

#define N_BITS 1024 // 256 // 64
#define POE_BITS 126 // Proof-of-Exponentiation, we skip the multiplier Q^l for now 

#define BN128_q "21888242871839275222246405745257275088696311157297823662689037894645226208583" // finite field 
// log_2(BN128_q) / 2 = 126.79834567750108, safe to store 126 bits per element.
#define MS_MOD "85070591730234615865843651857942052864"
#define POE_L "29159453932063132973675167014961"

#define MS_LIMBS (N_BITS / 126 + 1)

#define BN128_r "21888242871839275222246405745257275088548364400416034343698204186575808495617" // order of E(Fq)

#define N_128BIT "283759148081449791630669252667505103767"
// RSA Challenge: https://en.wikipedia.org/wiki/RSA_Factoring_Challenge
#define N_1024BIT "135066410865995223349603216278805969938881475605667027524485143851526510604859533833940287150571909441798207282164471551373680419703964191743046496589274256239341020864383202110372958725762358509643110564073501508187510676594629205563685529475213500852879416377328533906109750544334999811150056977236890927563"
#define N_2048BIT "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784406918290641249515082189298559149176184502808489120072844992687392807287776735971418347270261896375014971824691165077613379859095700097330459748808428401797429100642458691817195118746121515172654632282216869987549182422433637259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133844143603833904414952634432190114657544454178424020924616515723350778707749817125772467962926386356373289912154831438167899885040445364023527381951378636564391212010397122822120720357"
#define BN128_ORDER "21888242871839275222246405745257275088696311157297823662689037894645226208582"

#if N_BITS==128
#define N_IN_USE N_128BIT
#elif N_BITS==1024
#define N_IN_USE N_1024BIT
#elif N_BITS==2048
#define N_IN_USE N_2048BIT
#endif
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
extern mpz_t mp_G;
extern mpz_t mp_POE_L;
extern mpz_t mp_POE_q; //quotient
extern mpz_t mp_POE_Q; // Q element
extern mpz_t mp_MSMOD;
extern uint64_t multi_scalar_coeff[2 * MS_LIMBS][2 * MS_LIMBS]; // c^i, c = [1 .. 2M - 1], i = [0 .. M - 1]

//extern FF_TYPE * HashPrimes;

//FF_TYPE intractibleHash(uint64_t x);

void prove(txn_man *);
void proveAll();
void proveInteractive();
void proveInteractiveMerkleTree();
void proveInteractiveMerkleTreeNative();
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

inline __attribute__((optimize("O0"))) void prepare_POE(mpz_t exponent) {
    mpz_tdiv_q(mp_POE_q, exponent, mp_POE_L);
    mpz_powm(mp_POE_Q, mp_G, mp_POE_q, mp_N);
    mpz_powm(mp_POE_Q, mp_POE_Q, mp_POE_L, mp_N);
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
