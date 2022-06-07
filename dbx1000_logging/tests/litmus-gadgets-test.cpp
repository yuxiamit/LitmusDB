#define CURVE_BN128 // 128 bit security
#define N_TEST "65537"

#include "../verification/litmus-gadgets.hpp"
#include <iostream>
using namespace std;

mpz_t mp_N;
typedef libff::Fr<default_r1cs_ppzksnark_pp> FieldT;

void prove_and_verify(protoboard<FieldT> &pb) {
    const r1cs_constraint_system<FieldT> constraint_system = pb.get_constraint_system();

    r1cs_ppzksnark_keypair<default_r1cs_ppzksnark_pp> keypair = r1cs_ppzksnark_generator<default_r1cs_ppzksnark_pp>(constraint_system);

    cout << "Generating Proofs..." << endl;

    r1cs_ppzksnark_proof<default_r1cs_ppzksnark_pp> proof = r1cs_ppzksnark_prover<default_r1cs_ppzksnark_pp>(keypair.pk, pb.primary_input(), pb.auxiliary_input());

    cout << "Verification..." << endl;

    bool verified = r1cs_ppzksnark_verifier_strong_IC<default_r1cs_ppzksnark_pp>(keypair.vk, pb.primary_input(), proof);

    cout << "Verify:" << verified << endl;
    assert(verified == 1);
}

void test_num2bits() {
    protoboard<FieldT> pb;
    int total_inputs = 0;
    int n_bits = 12;

    // test num_2_bits
    pb_variable<FieldT> in_val;
    in_val.allocate(pb, "in_val");
    pb.ADD_CONSTRAINT(in_val, 1, 800);
    pb_variable<FieldT> * out_local = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits+1), ALIGN_SIZE);
    for (uint32_t i=0; i<n_bits + 1; i++) {
        new (out_local+i) pb_variable<FieldT>;
		out_local[i].allocate(pb, "out_local");
    }
    pb.val(in_val) = 800;
    num_2_bits<FieldT> nb(pb, in_val, out_local, n_bits);
    nb.generate_r1cs_constraints();
    nb.generate_r1cs_witness();

    nb.debug(cout);

    for (uint32_t i=0; i<n_bits + 1; i++) {
        cout << pb.val(out_local[i]) << " ";
    }
    cout << endl;
    
    prove_and_verify(pb);
}

void test_less_than() {
    protoboard<FieldT> pb;
    int n_bits = 12;

    pb_variable<FieldT> in_val1, in_val2, out;
    in_val1.allocate(pb, "in_val1");
    in_val2.allocate(pb, "in_val2");
    out.allocate(pb, "out");
    pb.ADD_CONSTRAINT(in_val1, 1, 800);
    pb.ADD_CONSTRAINT(in_val2, 1, 900);
    pb.val(in_val1) = 800;
    pb.val(in_val2) = 900;
    less_than<FieldT> lt(pb, in_val1, in_val2, out, n_bits);
    lt.generate_r1cs_constraints();
    lt.generate_r1cs_witness();

    cout << pb.val(out) << endl;

    prove_and_verify(pb);
}

void test_mod_p() {
    protoboard<FieldT> pb;
    int n_bits = 30;

    pb_variable<FieldT> a, q, r;
    a.allocate(pb, "a");
    q.allocate(pb, "q");
    r.allocate(pb, "r");
    pb.ADD_CONSTRAINT(a, 1, 65539);
    pb.val(a) = 65539;
    mod_p<FieldT> mp(pb, a, q, r, n_bits);
    mp.generate_r1cs_constraints();
    mp.generate_r1cs_witness();
    mp.debug(cout);
    
    cout << "r=" << pb.val(r) << " q=" << pb.val(q) << endl;

    prove_and_verify(pb);
}

void test_pow_mod_p() {
    protoboard<FieldT> pb;
    int n_bits = 30;

    pb_variable<FieldT> a, x, out;
    a.allocate(pb, "a");
    x.allocate(pb, "x");
    out.allocate(pb, "out");

    pb.ADD_CONSTRAINT(a, 1, 5);
    pb.ADD_CONSTRAINT(x, 1, 10);
    pb.val(a) = 5;
    pb.val(x) = 10;
    pow_mod_p<FieldT> pmp(pb, a, x, out, n_bits);
    pmp.generate_r1cs_constraints();
    pmp.generate_r1cs_witness();
    pmp.debug(cout);

    cout << "out=" << pb.val(out) << endl;

    prove_and_verify(pb);
}

int main() {
    default_r1cs_ppzksnark_pp::init_public_params();
    libff::inhibit_profiling_info = true;
    libff::inhibit_profiling_counters = true; // libff profiling is not thread-safe.
    
    mpz_init(mp_N);
    mpz_set_str(mp_N, N_TEST, 10);

    test_num2bits();
    test_less_than();
    test_mod_p();
    test_pow_mod_p();
    
    return 0;
}
