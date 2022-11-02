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

#include "litmus-prover.h"
// extern mpz_t mp_N;

// extern long long multi_scalar_coeff[][]; // c^i, c = [1 .. 2M - 1], i = [0 .. M - 1]

// TODO: improve performance by aligning MSMOD with the GMP base

// helper function
void get_mpz_from_fields(protoboard<FieldT> &pb, mpz_t target, vector<pb_variable<FieldT>> &arr, uint32_t num_limbs, mpz_t ms_mod)
{
	mpz_set_ui(target, 0);
	mpz_t mp_arr;
	mpz_init(mp_arr);
	for (int i = num_limbs - 1; i >= 0; i--)
	{
		mpz_mul(target, target, ms_mod);
		pb.val(arr[i]).as_bigint().to_mpz(mp_arr);
		mpz_add(target, target, mp_arr);	
	}
}

void get_fields_from_mpz(protoboard<FieldT> &pb, mpz_t target, vector<pb_variable<FieldT>> &arr, uint32_t num_limbs, mpz_t ms_mod)
{
	// TODO: move mp_mod out
	mpz_t mp_mod; mpz_t mp_target;
	mpz_init(mp_mod); mpz_init(mp_target); mpz_set(mp_target, target);
	for (uint32_t i = 0; i < num_limbs; i++)
	{
		mpz_tdiv_qr(mp_target, mp_mod, mp_target, ms_mod);
		pb.val(arr[i]) = FieldT(mp_mod); // must be within BN128_ORDER because of mp_MSMOD
	}
}

inline void allocate_pb_multi_scalar(protoboard<FieldT> &pb, vector<pb_variable<FieldT>> &arr, uint32_t num_limbs, const char *name)
{
	for (uint32_t i = 0; i < num_limbs; i++)
	{
		arr[i].allocate(pb, name);
	}
}

inline void eq_constraint_multi_scalar(protoboard<FieldT> &pb, vector<pb_variable<FieldT>> &arr1, vector<pb_variable<FieldT>> &arr2, uint32_t num_limbs)
{
	for (uint32_t i = 0; i < num_limbs; i++)
	{
		pb.ADD_CONSTRAINT(arr1[i], 1, arr2[i]);
	}
}

inline void assign_multi_scalar(protoboard<FieldT> &pb, vector<pb_variable<FieldT>> &arr1, vector<pb_variable<FieldT>> &arr2, uint32_t num_limbs)
{
	for (uint32_t i = 0; i < num_limbs; i++)
	{
		pb.val(arr1[i]) = pb.val(arr2[i]);
	}
}

/*
	Gadget number to bits
	- input: in
	- output: out[n_bits + 1], allocated outside the gadget
	- semantic:
		in = sum_i out[i] * 2^i
		out[i] = 0 or 1
	- witness relies on: in
*/
template <typename FieldT>
class num_2_bits : public gadget<FieldT>
{
private:
	pb_variable<FieldT> *lc;

public:
	const pb_variable<FieldT> *out;
	const pb_variable<FieldT> in;
	uint32_t n_bits;

	num_2_bits(protoboard<FieldT> &pb, const pb_variable<FieldT> &in,
			   const pb_variable<FieldT> *out,
			   uint32_t n_bits) : gadget<FieldT>(pb, "num_2_bits"), out(out), in(in), n_bits(n_bits)
	{
		lc = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits + 1), ALIGN_SIZE);
		for (uint32_t i = 0; i < n_bits + 1; i++)
		{
			new (lc + i) pb_variable<FieldT>;
			lc[i].allocate(this->pb, "lc");
		}
	}

	void generate_r1cs_constraints()
	{
		// lc[0] == 0;
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(lc[0], 1, 0));
		for (uint32_t i = 0; i < n_bits; i++)
		{
			// out[i] == 0 or 1
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out[i], out[i] - 1, 0));
			// lc[i] + out[i] * 2^i == lc[i+1]
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(1, lc[i] + out[i] * (1 << i), lc[i + 1]));
		}
		// lc[n_bits] == in
		// this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(lc[n_bits] -in, 1, 0));
	}

	void generate_r1cs_witness()
	{
		this->pb.val(lc[0]) = 0;
		for (uint32_t i = 0; i < n_bits; i++)
		{
			this->pb.val(out[i]) = (this->pb.val(in).as_ulong() >> i) % 2;
			this->pb.val(lc[i + 1]) = (i == 0 ? 0 : this->pb.val(lc[i])) + this->pb.val(out[i]) * (1 << i);
		}
	}

	void debug(ostream &out)
	{
		for (uint32_t i = 0; i <= n_bits; i++)
		{
			out << this->pb.val(lc[i]) << " ";
		}
		out << endl;
	}

	~num_2_bits()
	{
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
template <typename FieldT>
class less_than : public gadget<FieldT>
{
private:
	pb_variable<FieldT> *out_local;
	pb_variable<FieldT> aux;
	num_2_bits<FieldT> *ng;

public:
	const pb_variable<FieldT> out;
	const pb_variable<FieldT> in1;
	const pb_variable<FieldT> in2;
	uint32_t n_bits;

	less_than(protoboard<FieldT> &pb, const pb_variable<FieldT> &in1,
			  const pb_variable<FieldT> &in2,
			  const pb_variable<FieldT> &out, uint32_t n_bits) : gadget<FieldT>(pb, "less_than"), out(out), in1(in1), in2(in2), n_bits(n_bits)
	{
		out_local = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits + 1), ALIGN_SIZE);
		for (uint32_t i = 0; i < n_bits + 1; i++)
		{
			new (out_local + i) pb_variable<FieldT>;
			out_local[i].allocate(this->pb, "out_local");
		}
		aux.allocate(this->pb, "less-than-aux");
		ng = new num_2_bits<FieldT>(this->pb, aux, out_local, n_bits + 1);
	}

	void generate_r1cs_constraints()
	{
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(in1 + (1 << n_bits) - in2, 1, aux));
		ng->generate_r1cs_constraints();
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out + out_local[n_bits], 1, 1));
	}

	void generate_r1cs_witness()
	{
		this->pb.val(aux) = this->pb.val(in1) + (1 << n_bits) - this->pb.val(in2);
		ng->generate_r1cs_witness();
		this->pb.val(out) = this->pb.val(in1).as_ulong() < this->pb.val(in2).as_ulong();
	}

	~less_than()
	{
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
template <typename FieldT>
class mod_p : public gadget<FieldT>
{
private:
	pb_variable<FieldT> cM;
	pb_variable<FieldT> temp;
	pb_variable<FieldT> lt_out;
	less_than<FieldT> *lt;

public:
	const pb_variable<FieldT> r;
	const pb_variable<FieldT> a;
	const pb_variable<FieldT> q;
	uint32_t n_bits;

	mod_p(protoboard<FieldT> &pb, const pb_variable<FieldT> &a,
		  const pb_variable<FieldT> &q,
		  const pb_variable<FieldT> &r, uint32_t n_bits) : gadget<FieldT>(pb, "mod_p"), r(r), a(a), q(q), n_bits(n_bits)
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
		// this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp + r, 1, a));
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

	~mod_p()
	{
		delete lt;
	}

	void debug(ostream &out)
	{
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
template <typename FieldT>
class pow_mod_p : public gadget<FieldT>
{
private:
	pb_variable<FieldT> *mul;
	pb_variable<FieldT> *temp;
	pb_variable<FieldT> *temp2;
	pb_variable<FieldT> *out_local;
	pb_variable<FieldT> outMod;
	pb_variable<FieldT> outMod_q;
	num_2_bits<FieldT> *nb;
	mod_p<FieldT> *mp;

public:
	const pb_variable<FieldT> out;
	const pb_variable<FieldT> a;
	const pb_variable<FieldT> x;
	uint32_t n_bits;
	// out = a^x mod N
	pow_mod_p(protoboard<FieldT> &pb, const pb_variable<FieldT> &a,
			  const pb_variable<FieldT> &x,
			  const pb_variable<FieldT> &out, uint32_t n_bits) : gadget<FieldT>(pb, "pow_mod_p"), out(out), a(a), x(x), n_bits(n_bits)
	{
		outMod.allocate(pb, "outMod");
		outMod_q.allocate(pb, "outMod-q");
		// the binary representation of inpput x
		out_local = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits + 1), ALIGN_SIZE);
		// the multiplier everytime doubles itself
		mul = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);

		// temp[i] is out_local[i] * (mul[i] - 1)
		temp = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);
		// temp2[i+1] = temp2[i] * (temp[i] + 1)
		temp2 = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits), ALIGN_SIZE);
		for (uint32_t i = 0; i < n_bits + 1; i++)
		{
			new (out_local + i) pb_variable<FieldT>;
			out_local[i].allocate(this->pb, "out_local");
		}
		for (uint32_t i = 0; i < n_bits; i++)
		{
			new (mul + i) pb_variable<FieldT>;
			mul[i].allocate(this->pb, "mul");
			new (temp + i) pb_variable<FieldT>;
			temp[i].allocate(this->pb, "temp");
			new (temp2 + i) pb_variable<FieldT>;
			temp2[i].allocate(this->pb, "temp2");
		}
		nb = new num_2_bits<FieldT>(pb, x, out_local, n_bits + 1);
		mp = new mod_p<FieldT>(pb, outMod, outMod_q, out, n_bits);
	}

	~pow_mod_p()
	{
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

		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[i], mul[i] - 1, temp[i]));
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp[i] + 1, temp2[i], temp2[i + 1]));
			// TODO: add mod here
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(mul[i], mul[i], mul[i + 1]));
		}

		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[n_bits - 1], mul[n_bits - 1] - 1, temp[n_bits - 1]));

		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp[n_bits - 1] + 1, temp2[n_bits - 1], outMod));

		mp->generate_r1cs_constraints();
	}

	void generate_r1cs_witness()
	{
		this->pb.val(mul[0]) = this->pb.val(a);
		this->pb.val(temp2[0]) = 1;
		nb->generate_r1cs_witness();
		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			this->pb.val(temp[i]) = this->pb.val(out_local[i]) * (this->pb.val(mul[i]) - 1);
			this->pb.val(temp2[i + 1]) = (this->pb.val(temp[i]) + 1) * this->pb.val(temp2[i]);
			this->pb.val(mul[i + 1]) = this->pb.val(mul[i]) * this->pb.val(mul[i]);
		}
		this->pb.val(temp[n_bits - 1]) = this->pb.val(out_local[n_bits - 1]) * (this->pb.val(mul[n_bits - 1]) - 1);
		this->pb.val(outMod) = (this->pb.val(temp[n_bits - 1]) + 1) * this->pb.val(temp2[n_bits - 1]);
		mp->generate_r1cs_witness(); // fill in pb.val(out)
	}

	void debug(ostream &out)
	{
		out << "out_local: ";
		for (uint32_t i = 0; i <= n_bits; i++)
			out << this->pb.val(out_local[i]) << " ";
		out << endl;
		out << "mul: ";
		for (uint32_t i = 0; i < n_bits; i++)
			out << this->pb.val(mul[i]) << " ";
		out << endl;
		out << "temp: ";
		for (uint32_t i = 0; i < n_bits; i++)
			out << this->pb.val(temp[i]) << " ";
		out << endl;
		out << "temp2: ";
		for (uint32_t i = 0; i < n_bits; i++)
			out << this->pb.val(temp2[i]) << " ";
		out << endl;
		out << "OutMod: " << this->pb.val(outMod) << " out " << this->pb.val(this->out) << endl;
	}
};

/*
	Gadget multi-scalar addition
	- input: a[], b[]
	- output: c[]
	- semantic:
		c = a + b
	- witness relies on: a[] and b[]
*/
template <typename FieldT>
class add_multi_scalar : public gadget<FieldT>
{
private:
public:
	vector<pb_variable<FieldT>> va;
	vector<pb_variable<FieldT>> vb;
	vector<pb_variable<FieldT>> vout;
	vector<pb_variable<FieldT>> vtemp;
	uint32_t num_limbs; // should be half of the total length
	mpz_t &ms_mod;	// multi-scalar mod
	mpz_t tmp_vout;
	add_multi_scalar(protoboard<FieldT> &pb, vector<pb_variable<FieldT>> &va,
					 vector<pb_variable<FieldT>> &vb, vector<pb_variable<FieldT>> &vout, uint32_t num_limbs, mpz_t &ms_mod) : gadget<FieldT>(pb, "add_multi_scalar"), va(va), vb(vb), vout(vout), num_limbs(num_limbs), ms_mod(ms_mod), vtemp(num_limbs)
	{
		for (uint32_t i = 0; i < num_limbs; i++)
			vtemp[i].allocate(this->pb, "add_ms_temp");
		mpz_init(tmp_vout);
	}

	void generate_r1cs_constraints()
	{
		// temp[i] is the carry bits
		for (uint32_t i = 1; i < num_limbs; i++)
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(vtemp[i], vtemp[i] - 1, 0)); // carry bit = 0 or 1
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(va[0] + vb[0] - FieldT(ms_mod) * vtemp[0], 1, vout[0]));
		for (uint32_t i = 1; i < num_limbs; i++)
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(va[i] + vb[i] - FieldT(ms_mod) * vtemp[i], 1, vout[i] - vtemp[i - 1]));
		// TODO: make carry bits well-defined
		// Hack: actually no need to handle the carries because theoretically they still represent the same number.
	}

	void generate_r1cs_witness()
	{
		for (uint32_t i = 0; i < num_limbs - 1; i++)
		{
			this->pb.val(vout[i]) += this->pb.val(va[i]) + this->pb.val(vb[i]);
			this->pb.val(vout[i]).as_bigint().to_mpz(tmp_vout);
			if (mpz_cmp(tmp_vout, ms_mod) >= 0)
			{
				this->pb.val(vtemp[i]) = 1;
				this->pb.val(vout[i + 1]) += 1;
				this->pb.val(vout[i]) -= FieldT(ms_mod);
			}
		}
	}

	~add_multi_scalar(){
		mpz_clear(tmp_vout);
	}	
};

/*
	Gadget multi-scalar multiplication
	- input: a[], b[]
	- output: c[]
	- semantic:
		c = a * b
	- witness relies on: a[] and b[]
*/
template <typename FieldT>
class mul_multi_scalar : public gadget<FieldT>
{
private:
public:
	vector<pb_variable<FieldT>> va;
	vector<pb_variable<FieldT>> vb;
	vector<pb_variable<FieldT>> vout;
	uint32_t num_limbs; // should be half of the total length
	mpz_t & ms_mod;		// multi-scalar mod
	mpz_t tmp_q;
	mpz_t tmp_r;
	mpz_t tmp_vout;
	mul_multi_scalar(protoboard<FieldT> &pb, vector<pb_variable<FieldT>> &va,
					 vector<pb_variable<FieldT>> &vb, vector<pb_variable<FieldT>> &vout, uint32_t num_limbs, mpz_t &ms_mod) : gadget<FieldT>(pb, "mul_multi_scalar"), va(va), vb(vb), vout(vout), num_limbs(num_limbs), ms_mod(ms_mod)
	{
		//
		mpz_init(tmp_q);
		mpz_init(tmp_r);
		mpz_init(tmp_vout);
	}

	~mul_multi_scalar() {
		mpz_clear(tmp_q);
		mpz_clear(tmp_r);
		mpz_clear(tmp_vout);
	}

	void generate_r1cs_constraints()
	{
		// linear multiplication from xjsnark
		// (sum c^i a[i]) * (sum c^i b[i]) = sum c^i out[i]
		linear_combination<FieldT> la, lb, lout;
		for (uint32_t i = 1; i <= 2 * num_limbs - 1; i++)
		{
			for (uint32_t j = 0; j <= num_limbs - 1; j++)
			{
				la = la + va[j] * multi_scalar_coeff[i][j];
				lb = lb + vb[j] * multi_scalar_coeff[i][j];
				lout = lout + vout[j] * multi_scalar_coeff[i][j];
			}
			for (uint32_t j = num_limbs; j <= 2 * num_limbs - 1; j++)
				lout = lout + vout[j] * multi_scalar_coeff[i][j];
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(la, lb, lout));
		}
	}

	void generate_r1cs_witness()
	{
		for (int i = 0; i <= 2 * num_limbs - 1; i++)
		{
			for (uint32_t j = max(0, i - (int)num_limbs); j <= i && j < num_limbs; j++)
				if (i - j < num_limbs)
				this->pb.val(vout[i]) += this->pb.val(va[j]) * this->pb.val(vb[i - j]);
			// if(this->pb.val(vout[i]) >= ms_mod)
			this->pb.val(vout[i]).as_bigint().to_mpz(tmp_vout);
			if (mpz_cmp(tmp_vout, ms_mod) >= 0)
			{
				mpz_tdiv_qr(tmp_q, tmp_r, tmp_vout, ms_mod);
				if(i+1<num_limbs) // todo: fix this
					this->pb.val(vout[i + 1]) += FieldT(tmp_q);
				this->pb.val(vout[i]) = FieldT(tmp_r);
				// this->pb.val(vout[i+1]) += this->pb.val(vout[i]) / ms_mod;
				// this->pb.val(vout[i]) %= ms_mod;
			}
		}
	}
};

/*
	Gadget multi-scalar mod p
	- input: a[], N[]
	- output: r[]
	- semantic:
		r[] = a[] % N[]
		a[] == N[] * q[] + r[]
		r[] < N[]
	- witness relies on: a, N and p
*/
template <typename FieldT>
class mod_p_multi_scalar : public gadget<FieldT>
{
private:
	pb_variable<FieldT> cM;
	vector<pb_variable<FieldT>> vtemp;
	vector<pb_variable<FieldT>> vQ;
	pb_variable<FieldT> lt_out;
	mul_multi_scalar<FieldT> *mul_ms;
	add_multi_scalar<FieldT> *add_ms;

public:
	vector<pb_variable<FieldT>> vr;
	vector<pb_variable<FieldT>> va;
	vector<pb_variable<FieldT>> vN;
	vector<pb_variable<FieldT>> vq;
	uint32_t n_bits;
	uint32_t num_limbs;
	mpz_t &ms_mod;
	mod_p_multi_scalar(
		protoboard<FieldT> &pb,
		vector<pb_variable<FieldT>> &va,
		vector<pb_variable<FieldT>> &vN,
		vector<pb_variable<FieldT>> &vq,
		vector<pb_variable<FieldT>> &vr,
		uint32_t n_bits,
		const uint32_t num_limbs,
		mpz_t &ms_mod
	) : gadget<FieldT>(pb, "mod_p_multi_scalar"), 
	vr(vr), 
	va(va), 
	vq(vq), 
	vN(vN), 
	n_bits(n_bits), 
	num_limbs(num_limbs),
	ms_mod(ms_mod), 
	vtemp(2 * num_limbs), 
	vQ(num_limbs)
	{
		for (uint32_t i = 0; i < 2 * num_limbs; i++)
			vtemp[i].allocate(this->pb, "temp");
		cM.allocate(this->pb, "modulo");
		lt_out.allocate(this->pb, "less_than");
		mul_ms = new mul_multi_scalar<FieldT>(pb, vN, vQ, vtemp, num_limbs, ms_mod);
		add_ms = new add_multi_scalar<FieldT>(pb, vtemp, vr, va, num_limbs, ms_mod);
	}

	~mod_p_multi_scalar()
	{
		delete mul_ms;
		delete add_ms;
	}

	void generate_r1cs_constraints()
	{
		// r < cM and a == cM * q + r
		// lt->generate_r1cs_constraints();
		// TODO: check if this can be short-cut

		mul_ms->generate_r1cs_constraints();
		add_ms->generate_r1cs_constraints();
		// this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp + r, 1, a));
	}

	inline bool greater_eq(vector<pb_variable<FieldT>> &vr, vector<pb_variable<FieldT>> &vN, int i, int lb)
	{
		if (this->pb.val(vr[i + lb]) != 0)
			return true;
		for (int j = lb - 1; j >= 0; j--)
		{
			if (this->pb.val(vr[j + lb]) > this->pb.val(vN[j]))
				return true;
			if (this->pb.val(vr[j + lb]) < this->pb.val(vN[j]))
				return false;
		}
		return true; // equal
	}

	void generate_r1cs_witness()
	{
		/*
		int la, lb;
		for (la = 2 * num_limbs - 1; la > 0; --la) // upper bound is 2M - 1
			if (this->pb.val(va[la - 1]) != 0) break;
		lb = num_limbs - 1; // N is known to occupy

		//for (lb = num_limbs - 1; lb > 0; --lb)
		//	if (this->pb.val(vN[lb - 1]) != 0) break;


		// high precision div from https://oi-wiki.org/math/bignum/
		// assuming N is not 0
		for(uint32_t i=0; i<num_limbs; i++) this->pb.val(vr[i]) = this->pb.val(va[i]);
		for(int i=la - lb; i>=0; i--)
		{
			while (greater_eq(vr, vN, i, lb))
			{
				for(int j=0; j<lb; j++) {
					if ( this->pb.val(vr[i + j]) < this->pb.val(vN[j]))
					{
						this->pb.val(vr[i + j]) -= this->pb.val(vN[j]);
					}
					else
					{
						this->pb.val(vr[i + j + 1]) -= 1;
						this->pb.val(vr[i + j]) += ms_mod;
					}
				}
				this->pb.val(vQ[i]) += 1;
			}
		}

		// now vr and vq are ready
		*/

		// TODO: use GMP to do div directly
		mpz_t mp_a, mp_r, mp_q;
		mpz_init(mp_a);
		mpz_init(mp_r);
		mpz_init(mp_q);
		get_mpz_from_fields(this->pb, mp_a, va, num_limbs, ms_mod);
		get_mpz_from_fields(this->pb, mp_r, vr, num_limbs, ms_mod);
		get_mpz_from_fields(this->pb, mp_q, vQ, num_limbs, ms_mod);
		mpz_tdiv_qr(mp_q, mp_r, mp_a, mp_N);
		get_fields_from_mpz(this->pb, mp_r, vr, num_limbs, ms_mod);

		mul_ms->generate_r1cs_witness(); // relies on N and Q
		add_ms->generate_r1cs_witness(); // relies on vtemp and vr
										 // q = a / N
		this->pb.val(lt_out) = 1;
		// lt->generate_r1cs_witness();
	}

	void debug(ostream &out)
	{
		out << "mod_p multi-scalar" << endl;
	}
};

/*
	[incomplete] Gadget multi-scalar pow mod p
	- input: a[], x
	- output: out[]
	- semantic:
		out = a^x mod N
	- witness relies on: a[] and x
*/
template <typename FieldT>
class pow_mod_p_multi_scalar : public gadget<FieldT>
{
private:
	pb_variable<FieldT> *out_local; // the binary representation of inpput x

	vector<vector<pb_variable<FieldT>>> mul;   // the multiplier everytime doubles itself
	vector<vector<pb_variable<FieldT>>> mul_q;
	vector<vector<pb_variable<FieldT>>> mul_premod;
	vector<vector<pb_variable<FieldT>>> temp;  // temp[i] is out_local[i] * (mul[i] - 1)
	vector<vector<pb_variable<FieldT>>> temp2; // temp2[i+1] = temp2[i] * (temp[i] + 1)
	vector<vector<pb_variable<FieldT>>> temp2_q;
	vector<vector<pb_variable<FieldT>>> temp2_premod;

	vector<pb_variable<FieldT>> outMod;
	vector<pb_variable<FieldT>> outMod_q;
	num_2_bits<FieldT> *nb;
	mod_p_multi_scalar<FieldT> *mp;
	mul_multi_scalar<FieldT> **mul_ms_temp;
	mul_multi_scalar<FieldT> **mul_ms_mul;
	mod_p_multi_scalar<FieldT> ** modp_ms_temp;
	mod_p_multi_scalar<FieldT> ** modp_ms_mul;
public:
	vector<pb_variable<FieldT>> vout;
	vector<pb_variable<FieldT>> va;
	vector<pb_variable<FieldT>> vN;
	const pb_variable<FieldT> x;
	uint32_t n_bits;
	uint32_t num_limbs;
	mpz_t &ms_mod;
	// out = a^x mod N
	pow_mod_p_multi_scalar(
		protoboard<FieldT> &pb,
		vector<pb_variable<FieldT>> &va,
		vector<pb_variable<FieldT>> &vN,
		const pb_variable<FieldT> &x, // x is a single element number thanks to PoE
		vector<pb_variable<FieldT>> &vout,
		uint32_t n_bits,
		uint32_t num_limbs,
		mpz_t &ms_mod
	) : 
		gadget<FieldT>(pb, "pow_mod_p"), 
		vout(vout), 
		va(va), 
		vN(vN), 
		x(x), 
		n_bits(n_bits), 
		num_limbs(num_limbs), 
		ms_mod(ms_mod), 
		outMod(num_limbs), 
		outMod_q(num_limbs),
		mul(n_bits + 1, vector<pb_variable<FieldT>>(num_limbs)),
		mul_q(n_bits + 1, vector<pb_variable<FieldT>>(num_limbs)),
		mul_premod(n_bits + 1, vector<pb_variable<FieldT>>(2 * num_limbs)),
		temp(n_bits, vector<pb_variable<FieldT>>(num_limbs)),
		temp2(n_bits + 1, vector<pb_variable<FieldT>>(num_limbs)),
		temp2_q(n_bits + 1, vector<pb_variable<FieldT>>(num_limbs)),
		temp2_premod(n_bits + 1, vector<pb_variable<FieldT>>(2 * num_limbs))
	{

		for (uint32_t i = 0; i < num_limbs; i++)
		{
			outMod[i].allocate(pb, "outMod");
			outMod_q[i].allocate(pb, "outMod-q");
		}

		out_local = (pb_variable<FieldT> *)_mm_malloc(sizeof(pb_variable<FieldT>) * (n_bits + 1), ALIGN_SIZE);
		for (uint32_t i = 0; i < n_bits + 1; i++)
		{
			new (out_local + i) pb_variable<FieldT>;
			out_local[i].allocate(this->pb, "out_local");
		}
		for (uint32_t i = 0; i < n_bits; i++)
		{
			for (uint32_t j = 0; j < num_limbs; j++)
			{
				temp[i][j].allocate(this->pb, "temp");
			}
		}
		for (uint32_t i = 0; i <= n_bits; i++)
		{
			for (uint32_t j = 0; j < num_limbs; j++)
			{
				mul[i][j].allocate(this->pb, "mul");
				mul_q[i][j].allocate(this->pb, "mul_q");
				temp2[i][j].allocate(this->pb, "temp2");
				temp2_q[i][j].allocate(this->pb, "temp2_q");
			}
			for (uint32_t j = 0; j < 2 * num_limbs; j++)
			{
				mul_premod[i][j].allocate(this->pb, "mul_premod");
				temp2_premod[i][j].allocate(this->pb, "temp2_premod");
			}
		}
		nb = new num_2_bits<FieldT>(pb, x, out_local, n_bits + 1);
		/*
		mp = new mod_p_multi_scalar<FieldT>(
			pb, 
			outMod, 
			vN, 
			outMod_q, 
			vout, 
			n_bits, num_limbs, ms_mod
		); */
		mul_ms_temp = new mul_multi_scalar<FieldT> *[n_bits];
		mul_ms_mul = new mul_multi_scalar<FieldT> *[n_bits];
		modp_ms_temp = new mod_p_multi_scalar<FieldT> *[n_bits];
		modp_ms_mul = new mod_p_multi_scalar<FieldT> *[n_bits];
		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			mul_ms_temp[i] = new mul_multi_scalar<FieldT>(pb, temp[i], temp2[i], temp2_premod[i + 1], num_limbs, ms_mod);
			mul_ms_mul[i] = new mul_multi_scalar<FieldT>(pb, mul[i], mul[i], mul_premod[i + 1], num_limbs, ms_mod);
			modp_ms_temp[i] = new mod_p_multi_scalar<FieldT>(
				pb, temp2_premod[i+1], vN, temp2_q[i+1], temp2[i+1], n_bits, num_limbs, ms_mod
			);
			modp_ms_mul[i] = new mod_p_multi_scalar<FieldT>(
				pb, mul_premod[i+1], vN, mul_q[i+1], mul[i+1], n_bits, num_limbs, ms_mod
			);
		}
		mul_ms_temp[n_bits - 1] = new mul_multi_scalar<FieldT>(pb, temp[n_bits - 1], temp2[n_bits - 1], temp2_premod[n_bits], num_limbs, ms_mod);
		modp_ms_temp[n_bits - 1] = new mod_p_multi_scalar<FieldT>(
				pb, temp2_premod[n_bits], vN, temp2_q[n_bits], outMod, n_bits, num_limbs, ms_mod
		);
	}

	~pow_mod_p_multi_scalar()
	{
		delete nb;
		// delete mp;
		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			delete mul_ms_temp[i];
			delete mul_ms_mul[i];
			delete modp_ms_temp[i];
			delete modp_ms_mul[i];
		}
		delete[] mul_ms_temp;
		delete[] mul_ms_mul;
		delete[] modp_ms_temp;
		delete[] modp_ms_mul;
		_mm_free(out_local);
	}

	void generate_r1cs_constraints()
	{

		// r < cM and a == cM * q + r
		nb->generate_r1cs_constraints();
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp2[0][0], 1, 1));
		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(mul[0][0], 1, va[0]));
		for (uint32_t j = 1; j < num_limbs; j++)
		{
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(temp2[0][j], 1, 0));
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(mul[0][j], 1, va[j]));
		}

		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			// out_local[i] <= 1, so no need multi-scalar
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[i], mul[i][0] - 1, temp[i][0] - 1));
			for (uint32_t j = 1; j < num_limbs; j++)
			{
				this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[i], mul[i][j], temp[i][j]));
			}
			mul_ms_temp[i]->generate_r1cs_constraints();
			mul_ms_mul[i]->generate_r1cs_constraints();
			modp_ms_temp[i]->generate_r1cs_constraints();
			modp_ms_mul[i]->generate_r1cs_constraints();
			// TODO: per round mod
		}

		this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[n_bits - 1], mul[n_bits - 1][0] - 1, temp[n_bits - 1][0] - 1));
		for (uint32_t j = 1; j < num_limbs; j++)
		{
			this->pb.add_r1cs_constraint(r1cs_constraint<FieldT>(out_local[n_bits - 1], mul[n_bits - 1][j], temp[n_bits - 1][j]));
		}
		mul_ms_temp[n_bits - 1]->generate_r1cs_constraints(); // now outMod is ready

		//mp->generate_r1cs_constraints();
	}

	void generate_r1cs_witness()
	{
		for (uint32_t j = 0; j < num_limbs; j++)
		{
			this->pb.val(mul[0][j]) = this->pb.val(va[j]);
			this->pb.val(temp2[0][j]) = 0;
		}

		this->pb.val(temp2[0][0]) = 1;
		nb->generate_r1cs_witness();
		for (uint32_t i = 0; i < n_bits - 1; i++)
		{
			this->pb.val(temp[i][0]) = this->pb.val(out_local[i]) * (this->pb.val(mul[i][0]) - 1) + 1;
			for (uint32_t j = 1; j < num_limbs; j++)
				this->pb.val(temp[i][j]) = this->pb.val(out_local[i]) * this->pb.val(mul[i][j]);
			mul_ms_temp[i]->generate_r1cs_witness();
			mul_ms_mul[i]->generate_r1cs_witness();
			modp_ms_temp[i]->generate_r1cs_witness();
			modp_ms_mul[i]->generate_r1cs_witness();
		}
		this->pb.val(temp[n_bits - 1][0]) = this->pb.val(out_local[n_bits - 1]) * (this->pb.val(mul[n_bits - 1][0]) - 1) + 1;
		for (uint32_t j = 1; j < num_limbs; j++)
			this->pb.val(temp[n_bits - 1][j]) = this->pb.val(out_local[n_bits - 1]) * this->pb.val(mul[n_bits - 1][j]);
		mul_ms_temp[n_bits - 1]->generate_r1cs_witness();
		// mp->generate_r1cs_witness(); // fill in pb.val(out)
	}

	void debug(ostream &out)
	{
		out << "pow_mod_p_multi_scalar" << endl;
	}
};
