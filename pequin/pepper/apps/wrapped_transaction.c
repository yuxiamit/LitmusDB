typedef unsigned int uint32_t;

// this file serves as a template for a single transaction that reads two rows.

struct In {uint32_t val_0_0; uint32_t acc1_0_0; uint32_t acc2_0_0; uint32_t prod_0_0; uint32_t acc_prime_0_0; uint32_t A_0_0; uint32_t B_0_0;uint32_t val_1_0; uint32_t acc1_1_0; uint32_t acc2_1_0; uint32_t prod_1_0; uint32_t acc_prime_1_0; uint32_t A_1_0; uint32_t B_1_0;uint32_t val_2_0; uint32_t acc1_2_0; uint32_t acc2_2_0; uint32_t prod_2_0; uint32_t acc_prime_2_0; uint32_t A_2_0; uint32_t B_2_0;uint32_t val_3_0; uint32_t acc1_3_0; uint32_t acc2_3_0; uint32_t prod_3_0; uint32_t acc_prime_3_0; uint32_t A_3_0; uint32_t B_3_0;uint32_t val_4_0; uint32_t acc1_4_0; uint32_t acc2_4_0; uint32_t prod_4_0; uint32_t acc_prime_4_0; uint32_t A_4_0; uint32_t B_4_0;uint32_t val_5_0; uint32_t acc1_5_0; uint32_t acc2_5_0; uint32_t prod_5_0; uint32_t acc_prime_5_0; uint32_t A_5_0; uint32_t B_5_0;uint32_t val_6_0; uint32_t acc1_6_0; uint32_t acc2_6_0; uint32_t prod_6_0; uint32_t acc_prime_6_0; uint32_t A_6_0; uint32_t B_6_0;uint32_t val_7_0; uint32_t acc1_7_0; uint32_t acc2_7_0; uint32_t prod_7_0; uint32_t acc_prime_7_0; uint32_t A_7_0; uint32_t B_7_0;uint32_t val_8_0; uint32_t acc1_8_0; uint32_t acc2_8_0; uint32_t prod_8_0; uint32_t acc_prime_8_0; uint32_t A_8_0; uint32_t B_8_0;uint32_t val_9_0; uint32_t acc1_9_0; uint32_t acc2_9_0; uint32_t prod_9_0; uint32_t acc_prime_9_0; uint32_t A_9_0; uint32_t B_9_0;uint32_t val_10_0; uint32_t acc1_10_0; uint32_t acc2_10_0; uint32_t prod_10_0; uint32_t acc_prime_10_0; uint32_t A_10_0; uint32_t B_10_0;uint32_t val_11_0; uint32_t acc1_11_0; uint32_t acc2_11_0; uint32_t prod_11_0; uint32_t acc_prime_11_0; uint32_t A_11_0; uint32_t B_11_0;uint32_t val_12_0; uint32_t acc1_12_0; uint32_t acc2_12_0; uint32_t prod_12_0; uint32_t acc_prime_12_0; uint32_t A_12_0; uint32_t B_12_0;uint32_t val_13_0; uint32_t acc1_13_0; uint32_t acc2_13_0; uint32_t prod_13_0; uint32_t acc_prime_13_0; uint32_t A_13_0; uint32_t B_13_0;uint32_t val_14_0; uint32_t acc1_14_0; uint32_t acc2_14_0; uint32_t prod_14_0; uint32_t acc_prime_14_0; uint32_t A_14_0; uint32_t B_14_0;};
struct Out {uint32_t ok;};

uint32_t ipow(uint32_t base, uint32_t exp)
{
    uint32_t result = 1;
    uint32_t i;
    // we cannot use while(1) here
    for(i=0;i<32;i++)
    {
        if (exp>0 && exp % 2 == 1)
            result = result * base % 2577668663;
        exp = exp / 2;
        base = base * base % 2577668663;
    }
    return result;
}

void compute(struct In* input, struct Out* output){ // a simple YCSB construction.
    uint32_t localDigest = 1;
    uint32_t allpass = 1;
        // starting transaction 0;
    if(
               (input->val_0_0==0 && ipow(localDigest, input->A_0_0) * ipow(7, (2778730330) * input->B_0_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_0_0, (2778730330 * input->val_0_0))==input->acc2_0_0 &&
        ipow(input->acc2_0_0, input->prod_0_0)==localDigest &&
        ipow(7, input->prod_0_0) == input->acc_prime_0_0 &&
        ipow(input->acc_prime_0_0, input->A_0_0) * ipow(7, (2778730330) * input->B_0_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_0_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2779571826 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 0;
    // starting transaction 1;
    if(
               (input->val_1_0==0 && ipow(localDigest, input->A_1_0) * ipow(7, (2780842839) * input->B_1_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_1_0, (2780842839 * input->val_1_0))==input->acc2_1_0 &&
        ipow(input->acc2_1_0, input->prod_1_0)==localDigest &&
        ipow(7, input->prod_1_0) == input->acc_prime_1_0 &&
        ipow(input->acc_prime_1_0, input->A_1_0) * ipow(7, (2780842839) * input->B_1_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_1_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2779004023 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 1;
    // starting transaction 2;
    if(
               (input->val_2_0==0 && ipow(localDigest, input->A_2_0) * ipow(7, (2781149405) * input->B_2_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_2_0, (2781149405 * input->val_2_0))==input->acc2_2_0 &&
        ipow(input->acc2_2_0, input->prod_2_0)==localDigest &&
        ipow(7, input->prod_2_0) == input->acc_prime_2_0 &&
        ipow(input->acc_prime_2_0, input->A_2_0) * ipow(7, (2781149405) * input->B_2_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_2_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2784133903 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 2;
    // starting transaction 3;
    if(
               (input->val_3_0==0 && ipow(localDigest, input->A_3_0) * ipow(7, (2786259836) * input->B_3_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_3_0, (2786259836 * input->val_3_0))==input->acc2_3_0 &&
        ipow(input->acc2_3_0, input->prod_3_0)==localDigest &&
        ipow(7, input->prod_3_0) == input->acc_prime_3_0 &&
        ipow(input->acc_prime_3_0, input->A_3_0) * ipow(7, (2786259836) * input->B_3_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_3_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2787247061 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 3;
    // starting transaction 4;
    if(
               (input->val_4_0==0 && ipow(localDigest, input->A_4_0) * ipow(7, (2786483715) * input->B_4_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_4_0, (2786483715 * input->val_4_0))==input->acc2_4_0 &&
        ipow(input->acc2_4_0, input->prod_4_0)==localDigest &&
        ipow(7, input->prod_4_0) == input->acc_prime_4_0 &&
        ipow(input->acc_prime_4_0, input->A_4_0) * ipow(7, (2786483715) * input->B_4_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_4_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2778744056 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 4;
    // starting transaction 5;
    if(
               (input->val_5_0==0 && ipow(localDigest, input->A_5_0) * ipow(7, (2786650697) * input->B_5_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_5_0, (2786650697 * input->val_5_0))==input->acc2_5_0 &&
        ipow(input->acc2_5_0, input->prod_5_0)==localDigest &&
        ipow(7, input->prod_5_0) == input->acc_prime_5_0 &&
        ipow(input->acc_prime_5_0, input->A_5_0) * ipow(7, (2786650697) * input->B_5_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_5_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2778761100 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 5;
    // starting transaction 6;
    if(
               (input->val_6_0==0 && ipow(localDigest, input->A_6_0) * ipow(7, (2782712991) * input->B_6_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_6_0, (2782712991 * input->val_6_0))==input->acc2_6_0 &&
        ipow(input->acc2_6_0, input->prod_6_0)==localDigest &&
        ipow(7, input->prod_6_0) == input->acc_prime_6_0 &&
        ipow(input->acc_prime_6_0, input->A_6_0) * ipow(7, (2782712991) * input->B_6_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_6_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2783884953 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 6;
    // starting transaction 7;
    if(
               (input->val_7_0==0 && ipow(localDigest, input->A_7_0) * ipow(7, (2781873895) * input->B_7_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_7_0, (2781873895 * input->val_7_0))==input->acc2_7_0 &&
        ipow(input->acc2_7_0, input->prod_7_0)==localDigest &&
        ipow(7, input->prod_7_0) == input->acc_prime_7_0 &&
        ipow(input->acc_prime_7_0, input->A_7_0) * ipow(7, (2781873895) * input->B_7_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_7_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2778885739 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 7;
    // starting transaction 8;
    if(
               (input->val_8_0==0 && ipow(localDigest, input->A_8_0) * ipow(7, (2783056052) * input->B_8_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_8_0, (2783056052 * input->val_8_0))==input->acc2_8_0 &&
        ipow(input->acc2_8_0, input->prod_8_0)==localDigest &&
        ipow(7, input->prod_8_0) == input->acc_prime_8_0 &&
        ipow(input->acc_prime_8_0, input->A_8_0) * ipow(7, (2783056052) * input->B_8_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_8_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2786266957 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 8;
    // starting transaction 9;
    if(
               (input->val_9_0==0 && ipow(localDigest, input->A_9_0) * ipow(7, (2781006836) * input->B_9_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_9_0, (2781006836 * input->val_9_0))==input->acc2_9_0 &&
        ipow(input->acc2_9_0, input->prod_9_0)==localDigest &&
        ipow(7, input->prod_9_0) == input->acc_prime_9_0 &&
        ipow(input->acc_prime_9_0, input->A_9_0) * ipow(7, (2781006836) * input->B_9_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_9_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2778855029 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 9;
    // starting transaction 10;
    if(
               (input->val_10_0==0 && ipow(localDigest, input->A_10_0) * ipow(7, (2778733357) * input->B_10_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_10_0, (2778733357 * input->val_10_0))==input->acc2_10_0 &&
        ipow(input->acc2_10_0, input->prod_10_0)==localDigest &&
        ipow(7, input->prod_10_0) == input->acc_prime_10_0 &&
        ipow(input->acc_prime_10_0, input->A_10_0) * ipow(7, (2778733357) * input->B_10_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_10_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2779407312 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 10;
    // starting transaction 11;
    if(
               (input->val_11_0==0 && ipow(localDigest, input->A_11_0) * ipow(7, (2782674235) * input->B_11_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_11_0, (2782674235 * input->val_11_0))==input->acc2_11_0 &&
        ipow(input->acc2_11_0, input->prod_11_0)==localDigest &&
        ipow(7, input->prod_11_0) == input->acc_prime_11_0 &&
        ipow(input->acc_prime_11_0, input->A_11_0) * ipow(7, (2782674235) * input->B_11_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_11_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2780270466 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 11;
    // starting transaction 12;
    if(
               (input->val_12_0==0 && ipow(localDigest, input->A_12_0) * ipow(7, (2783649600) * input->B_12_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_12_0, (2783649600 * input->val_12_0))==input->acc2_12_0 &&
        ipow(input->acc2_12_0, input->prod_12_0)==localDigest &&
        ipow(7, input->prod_12_0) == input->acc_prime_12_0 &&
        ipow(input->acc_prime_12_0, input->A_12_0) * ipow(7, (2783649600) * input->B_12_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_12_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2781394278 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 12;
    // starting transaction 13;
    if(
               (input->val_13_0==0 && ipow(localDigest, input->A_13_0) * ipow(7, (2778866110) * input->B_13_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_13_0, (2778866110 * input->val_13_0))==input->acc2_13_0 &&
        ipow(input->acc2_13_0, input->prod_13_0)==localDigest &&
        ipow(7, input->prod_13_0) == input->acc_prime_13_0 &&
        ipow(input->acc_prime_13_0, input->A_13_0) * ipow(7, (2778866110) * input->B_13_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_13_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2785769395 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 13;
    // starting transaction 14;
    if(
               (input->val_14_0==0 && ipow(localDigest, input->A_14_0) * ipow(7, (2778803015) * input->B_14_0) % 2577668663 == 7 ) ||(
        ipow(input->acc1_14_0, (2778803015 * input->val_14_0))==input->acc2_14_0 &&
        ipow(input->acc2_14_0, input->prod_14_0)==localDigest &&
        ipow(7, input->prod_14_0) == input->acc_prime_14_0 &&
        ipow(input->acc_prime_14_0, input->A_14_0) * ipow(7, (2778803015) * input->B_14_0) % 2577668663 == 7
        )

    )
    {
        // run the content of the transaction
                uint32_t outval_0 = input->val_14_0 * 2;

        // update the digest
                localDigest = ipow(localDigest, (2778787718 * outval_0));

        // calculate the allpass flag
        allpass *= 1;
    }
    else
    {
        allpass = 0;
    }
    // ending transaction 14;

   output->ok = allpass;
}
