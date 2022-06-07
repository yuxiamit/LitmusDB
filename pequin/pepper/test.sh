echo "Building Verifier ..."

./pepper_compile_and_setup_V.sh $1 $1.vkey $1.pkey

echo "Building Prover ..."

./pepper_compile_and_setup_P.sh $1

echo "Generating Inputs ..."

# bin/pepper_verifier_$1 gen_input $1.inputs 

echo "Proving ..."

bin/pepper_prover_$1 prove $1.pkey $1.inputs $1.outputs $1.proof

echo "Verifying ..."

bin/pepper_verifier_$1 verify $1.vkey $1.inputs $1.outputs $1.proof
