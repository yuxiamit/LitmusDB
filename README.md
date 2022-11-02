# vDBx1000: The Litmus Project

## Important: Before You Use This Codebase

**You should never ever use it in production as it is a preliminary build. The code is provided as it is with no warranty or support.**

This implementation is a proof-of-concept preliminary build for performance estimating purposes. As discussed in the paper, there are three limitations:

1. We include the key generation in the critical path while it is unnecessary. This factor results in performance underestimation.
2. We use a **small** RSA group to avoid the engineering effort of multi-scalar operations in the constraint system. For how to extend our implementation to large groups, [this work](https://github.com/akosba/xjsnark) and [this work](https://github.com/alex-ozdemir/bellman-bignat) would be helpful.
3. We assume division-intractibility in very large odd integers. This brings two effects: (a) the non-existence proof of key lookups might fail because the large integers could have common divisors. We commented one constraint at line ??? of file ???. This does not affect the performance much since the prover workload depends on the number of constraints and variables. You may uncomment the constraint back but then you need to comment the assertion *verified==true* at line ??? of file ???. (b) This skips the Pocklington verification. It results in performance overestimation (by a constant factor).

Please do not hesitate fo submit an issue or contact us directly if you found any bugs.

## How to Install

1. Navigate into pequin/ and follow the instruction in GETTING_STARTED.md and INSTALLING.md
2. Navigate into dbx1000/ and run install_deps.sh
3. Run `python3 tools/compile.py`
4. Create the `logs` and `results` folder.

You should now see a number of executables whose names start with `rundb`. They represent the baselines.

## How to Reproduce the Evaluation

Before you run the evaluation, make sure you create a `tmpfs` filesystem larger than 20GB and mount it to the folder `logs`.

`python3 scripts/runExpr.py litmus`

`python3 scripts/runExpr.py litmusBatchSize`

`python3 scripts/runExpr.py litmusProverThreadsRAMDISK`

To collect the data add `COLLECT_DATA=x` to the above lines.

`COLLECT_DATA=1`: Throughput

`COLLECT_DATA=2`: Latency

`COLLECT_DATA=3`: Communication Cost

`COLLECT_DATA=4`: Time Breakdown

`COLLECT_DATA=5`: Throughput for No-Verification baselines (removing all the overhead)
