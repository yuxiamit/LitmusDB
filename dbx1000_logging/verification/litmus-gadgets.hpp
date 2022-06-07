#include "libff/algebra/fields/field_utils.hpp"
#include "libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/r1cs_ppzksnark.hpp"
#include "libsnark/common/default_types/r1cs_ppzksnark_pp.hpp"
#include "libsnark/gadgetlib1/pb_variable.hpp"

#include "libsnark/gadgetlib1/gadget.hpp"
#include "litmus-template.h"
#include "helper.h"
#include <stdlib.h>
#include <unistd.h>
#include <gmp.h>
#include <gmpxx.h>

#define ADD_CONSTRAINT(a, b, c) add_r1cs_constraint(r1cs_constraint<FieldT>(a, b, c));

using namespace libsnark;

extern mpz_t mp_N;

/*
	Gadget number to bits
	- input: in
	- output: out[n_bits + 1], allocated outside the gadget
	- semantic: 
		in = sum_i out[i] * 2^i
		out[i] = 0 or 1
	- witness relies on: in
*/
template<typename FieldT>
class num_2_bits : public gadget<FieldT> {
private:
	pb_variable<FieldT> * lc;
public:
  const pb_variable<FieldT> *out;
  const pb_variable<FieldT> in;
  uint32_t n_bits;

  num_2_bits(protoboard<FieldT> &pb, const pb_variable<FieldT> &in,
              const pb_variable<FieldT> *out,
               uint32_t n_bits) : 
    gadget<FieldT>(pb, "num_2_bits"), out(out), in(in), n_bits(n_bits)
  {
	  lc = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits+1), ALIGN_SIZE);
	  for(uint32_t i=0; i<n_bits+1; i++){
	  	new (lc+i) pb_variable<FieldT>;
		lc[i].allocate(this->pb, "lc");
	  }
  }

  void generate_r1cs_constraints()
  {
	// lc[0] == 0;
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(lc[0], 1, 0));
	for (uint32_t i=0; i<n_bits; i++)
	{
		// out[i] == 0 or 1
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out[i], out[i] - 1, 0));
		// lc[i] + out[i] * 2^i == lc[i+1]
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(1, lc[i] +  out[i] * (1<<i), lc[i+1]));
	}
	// lc[n_bits] == in
    // this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(lc[n_bits] -in, 1, 0));
  }

  void generate_r1cs_witness()
  {
	this->pb.val(lc[0]) = 0;
    for(uint32_t i=0; i<n_bits; i++)
	{
		this->pb.val(out[i]) = (this->pb.val(in).as_ulong() >> i) % 2;
		this->pb.val(lc[i+1]) = (i==0?0:this->pb.val(lc[i])) + this->pb.val(out[i]) * (1<<i);
	}
  }

  void debug(ostream &out) {
	  for(uint32_t i=0; i<=n_bits; i++) {
		out << this->pb.val(lc[i]) << " ";
	  }
	  out << endl;
  }

  ~num_2_bits() {
	  _mm_free(lc);
  }
};

/*
	Gadget Less Than
	- input: in1, in2
	- output: out
	- semantic:
		out = in1 < in2
	- witness relies on: in1 and in2
*/
template<typename FieldT>
class less_than : public gadget<FieldT> {
private:
	pb_variable<FieldT> * out_local;
	pb_variable<FieldT> aux;
	num_2_bits<FieldT> * ng;
public:
  const pb_variable<FieldT> out;
  const pb_variable<FieldT> in1;
  const pb_variable<FieldT> in2;
  uint32_t n_bits;

  less_than(protoboard<FieldT> &pb, const pb_variable<FieldT> &in1,
  				const pb_variable<FieldT> &in2,
            	const pb_variable<FieldT> &out, uint32_t n_bits) : 
    gadget<FieldT>(pb, "less_than"), out(out), in1(in1), in2(in2), n_bits(n_bits)
  {
	  out_local = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits+1), ALIGN_SIZE);
	  for(uint32_t i=0; i<n_bits+1; i++){
	  	new (out_local+i) pb_variable<FieldT>;
		out_local[i].allocate(this->pb, "out_local");
	  }
	  aux.allocate(this->pb, "less-than-aux");
	  ng = new num_2_bits<FieldT>(this->pb, aux, out_local, n_bits+1);
  }

  void generate_r1cs_constraints()
  {
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(in1 + (1<<n_bits) - in2, 1, aux));
	ng->generate_r1cs_constraints();
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out + out_local[n_bits], 1, 1));
  }

  void generate_r1cs_witness()
  {
	this->pb.val(aux) = this->pb.val(in1) + (1<<n_bits) - this->pb.val(in2);
	ng->generate_r1cs_witness();
    this->pb.val(out) = this->pb.val(in1).as_ulong() < this->pb.val(in2).as_ulong();
  }

  ~less_than() {
	  delete ng;
	  _mm_free(out_local);
  }
};

/*
	Gadget mod p
	- input: a
	- output: r
	- semantic:
		r = a % N
		a == N * q + r
		r < N
	- witness relies on: a and p
*/
template<typename FieldT>
class mod_p : public gadget<FieldT> {
private:
	pb_variable<FieldT> cM;
	pb_variable<FieldT> temp;
	pb_variable<FieldT> lt_out;
	less_than<FieldT> * lt;
public:
  const pb_variable<FieldT> r;
  const pb_variable<FieldT> a;
  const pb_variable<FieldT> q;
  uint32_t n_bits;

  mod_p(protoboard<FieldT> &pb, const pb_variable<FieldT> &a,
  				const pb_variable<FieldT> &q,
            	const pb_variable<FieldT> &r, uint32_t n_bits) : 
    gadget<FieldT>(pb, "mod_p"), r(r), a(a), q(q), n_bits(n_bits)
  {
	  temp.allocate(this->pb, "temp");
	  cM.allocate(this->pb, "modulo");
	  lt_out.allocate(this->pb, "less_than");
	  lt = new less_than<FieldT>(pb, r, cM, lt_out, n_bits);
  }

  void generate_r1cs_constraints()
  {
	// r < cM and a == cM * q + r
	// lt->generate_r1cs_constraints();
	// TODO: check if this can be short-cut
	
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(lt_out, 1, 1));
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(cM, 1, libff::bn128_Fr(mp_N)));
	
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(cM, q, temp));
	//this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp + r, 1, a));
	
  }

  void generate_r1cs_witness()
  {
    this->pb.val(cM) = libff::bn128_Fr(mp_N);
	this->pb.val(r) = this->pb.val(a).as_ulong() % this->pb.val(cM).as_ulong();
	this->pb.val(q) = this->pb.val(a).as_ulong() / this->pb.val(cM).as_ulong();
	this->pb.val(temp) = this->pb.val(cM) * this->pb.val(q);
	this->pb.val(lt_out) = 1;
	lt->generate_r1cs_witness();
  }

  ~mod_p() {
	  delete lt;
  }

  void debug(ostream &out) {
	  out << "mod_p a=" << this->pb.val(a) 
	  	  << " q=" << this->pb.val(q) 
		  << " r=" << this->pb.val(r) 
		  << " N=" << this->pb.val(cM) 
		  << " (r<N)=" << this->pb.val(lt_out)
		  << endl;
  }
};

/*
	Gadget pow mod p
	- input: a, x
	- output: out
	- semantic:
		out = a^x mod N
	- witness relies on: a and x
*/
template<typename FieldT>
class pow_mod_p : public gadget<FieldT> {
private:
	pb_variable<FieldT> * mul;
	pb_variable<FieldT> * temp;
	pb_variable<FieldT> * temp2;
	pb_variable<FieldT> * out_local;
    pb_variable<FieldT> outMod;
    pb_variable<FieldT> outMod_q;
	num_2_bits<FieldT> * nb;
    mod_p<FieldT>      * mp;
public:
  const pb_variable<FieldT> out;
  const pb_variable<FieldT> a;
  const pb_variable<FieldT> x;
  uint32_t n_bits;
  // out = a^x mod N
  pow_mod_p(protoboard<FieldT> &pb, const pb_variable<FieldT> &a,
  				const pb_variable<FieldT> &x,
            	const pb_variable<FieldT> &out, uint32_t n_bits) : 
    gadget<FieldT>(pb, "pow_mod_p"), out(out), a(a), x(x), n_bits(n_bits)
  {
	  outMod.allocate(pb, "outMod");
	  outMod_q.allocate(pb, "outMod-q");
	  // the binary representation of inpput x
	  out_local = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits+1), ALIGN_SIZE);
	  // the multiplier everytime doubles itself
	  mul = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);

	  // temp[i] is out_local[i] * (mul[i] - 1)
	  temp = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);
	  // temp2[i+1] = temp2[i] * (temp[i] + 1)
	  temp2 = (pb_variable<FieldT> *) _mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);
	  for(uint32_t i=0; i<n_bits+1; i++){
	  	new (out_local+i) pb_variable<FieldT>;
		out_local[i].allocate(this->pb, "out_local");
	  }
	  for(uint32_t i=0; i<n_bits; i++){
		new (mul+i) pb_variable<FieldT>;
		mul[i].allocate(this->pb, "mul");
		new (temp+i) pb_variable<FieldT>;
		temp[i].allocate(this->pb, "temp");
		new (temp2+i) pb_variable<FieldT>;
		temp2[i].allocate(this->pb, "temp2");
	  }
	  nb = new num_2_bits<FieldT>(pb, x, out_local, n_bits+1);
      mp = new mod_p<FieldT>(pb, outMod, outMod_q, out, n_bits);
  }

  ~pow_mod_p() {
	  delete nb;
	  delete mp;
	  _mm_free(out_local);
	  _mm_free(mul);
	  _mm_free(temp);
	  _mm_free(temp2);
  }

  void generate_r1cs_constraints()
  { 
	
	// r < cM and a == cM * q + r
	nb->generate_r1cs_constraints();
	
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp2[0], 1, 1));
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(mul[0], 1, a));
	
	for(uint32_t i=0; i<n_bits - 1; i++)
	{
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[i], mul[i] - 1, temp[i]));
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp[i] + 1, temp2[i], temp2[i+1]));
		// TODO: add mod here
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(mul[i], mul[i], mul[i+1]));
	}
	
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[n_bits-1], mul[n_bits-1] - 1, temp[n_bits-1]));
	
	this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp[n_bits-1] + 1, temp2[n_bits-1], outMod));
	
    mp->generate_r1cs_constraints();
	
  }

  void generate_r1cs_witness()
  {
	this->pb.val(mul[0]) = this->pb.val(a);
	this->pb.val(temp2[0]) = 1;
	nb->generate_r1cs_witness();
	for(uint32_t i=0; i<n_bits - 1; i++)
	{
		this->pb.val(temp[i]) = this->pb.val(out_local[i]) * (this->pb.val(mul[i])-1);
		this->pb.val(temp2[i+1]) = (this->pb.val(temp[i]) +1 )* this->pb.val(temp2[i]);
		this->pb.val(mul[i+1]) = this->pb.val(mul[i]) * this->pb.val(mul[i]);
	}
	this->pb.val(temp[n_bits-1]) = this->pb.val(out_local[n_bits-1]) * (this->pb.val(mul[n_bits-1]) - 1);
	this->pb.val(outMod) = (this->pb.val(temp[n_bits-1]) + 1 )* this->pb.val(temp2[n_bits-1]);
    mp->generate_r1cs_witness(); // fill in pb.val(out)
  }

  void debug(ostream &out) {
	  out << "out_local: ";
	  for(uint32_t i=0; i<= n_bits; i++) out << this->pb.val(out_local[i]) << " ";
	  out << endl;
	  out << "mul: ";
	  for(uint32_t i=0; i< n_bits; i++) out << this->pb.val(mul[i]) << " ";
	  out << endl;
	  out << "temp: ";
	  for(uint32_t i=0; i< n_bits; i++) out << this->pb.val(temp[i]) << " ";
	  out << endl;
	  out << "temp2: ";
	  for(uint32_t i=0; i< n_bits; i++) out << this->pb.val(temp2[i]) << " ";
	  out << endl;
	  out << "OutMod: " << this->pb.val(outMod) << " out " << this->pb.val(this->out) << endl;
  }
};


