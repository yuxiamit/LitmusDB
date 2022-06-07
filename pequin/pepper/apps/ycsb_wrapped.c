#define READ_ADDR 0xFFFFFF
#define WRITE_ADDR 0xEEEEEE

#include <stdint.h>
#include <gmp.h>

#define N (2577668663)
#define P (56611) // known to the client only
#define Q (45533) // known to the client only
#define G 7 // easier for debug

#define H(addr, val) (addr*val) // dummy hash function for now
#define H_addr(addr) (addr) 

// this file serves as a template for a single transaction that reads two rows.

struct In {uint32_t val; uint32_t acc1; uint32_t acc2; uint32_t prod; uint32_t acc_prime; uint32_t A; uint32_t B};
struct Out {uint32_t ok;};

uint32_t ipow(uint32_t base, uint32_t exp)
{
    uint32_t result = 1;
    for (;;)
    {
        if (exp & 1)
            result = result * base % N;
        exp >>= 1;
        if (!exp)
            break;
        base = base * base % N;
    }
    return result;
}

void compute(struct In* input, struct Out* output){ // a simple YCSB construction.
    uint32_t localDigest = 1;
    uint32_t allpass = 1;
    // starting transaction 1;
    if( (input->val==0 && ipow(localDigest, input->A) * ipow(G, H_addr(READ_ADDR * input->B)) % N == G) || (
            ipow(input->acc1, H(READ_ADDR, input->val))==input->acc2 &&
            ipow(input->acc2, input->prod)==localDigest &&
            ipow(G, input->prod) == input->acc_prime &&
            ipow(input->acc_prime, input->A) * ipow(G, H_addr(READ_ADDR)) % N == G
        )
    )
    {
        // run the content of the transaction
        uint32_t outval = input->val * 2;
        // update the digest
        localDigest = ipow(localDigest, H(WRITE_ADDR, outval));
        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 1;
    
    output->ok = allpass;   
}
