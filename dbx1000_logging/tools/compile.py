import sys
import os
import sys
import re
import os.path
import platform
import subprocess


def replace(filename, pattern, replacement):
    f = open(filename)
    s = f.read()
    f.close()
    s = re.sub(pattern, replacement, s)
    f = open(filename, 'w')
    f.write(s)
    f.close()


dbms_cfg = ["config-std.h", "config.h"]
algs = ['no', 'serial', 'parallel']
jobs = {}

def insert_his(alg, workload='YCSB', cc_alg='NO_WAIT', log_type='LOG_DATA', mem_integrity='RSA_AD', verification='true', recovery='false', gc='false', ramdisc='true', max_txn=100000, elle='false'):
    if alg == 'no':
        name = 'N'
    else:
        name = 'S' if alg == 'serial' else 'P'
        if alg == 'serial':
            name = 'S'
        elif alg == 'parallel':
            name = 'P'
        elif alg == 'batch':
            name = 'B'
        elif alg == 'taurus':
            name = 'T'
        else:
            assert(False)
    
    assert log_type == 'LOG_DATA'

    name += 'D'

    if verification == 'false':
        name += 'N'
    else:
        name += mem_integrity[0]
    
    name += '_%s_%s' % (workload, cc_alg.replace('_', ''))
    jobs[name] = {}

    jobs[name]["VERIFICATION"] = verification
    if elle == 'true':
        jobs[name]["ELLE_OUTPUT"] = 'true'
    else:
        jobs[name]["ELLE_OUTPUT"] = 'false'
    if verification:
        jobs[name]["ELLE_OUTPUT"] = 'false'

    if verification:
        jobs[name]["LOG_ALGORITHM"] = "LOG_%s" % alg.upper()
    else:
        jobs[name]["LOG_ALGORITHM"] = "LOG_NO"
    jobs[name]["WORKLOAD"] = workload
    jobs[name]["LOG_TYPE"] = log_type
    jobs[name]["CC_ALG"] = cc_alg
    
    if cc_alg == 'DETRESERVEMP':
        jobs[name]["CC_ALG"] = 'DETRESERVE'
    
    if cc_alg == 'INTERACT':
        jobs[name]["CC_ALG"] = 'NO_WAIT'
        jobs[name]["CLIENT_INTERACT"] = 'true'
        jobs[name]["SIM_NET_LATENCY"] = '100'
    
    if cc_alg == 'INTERACT1ms':
        jobs[name]["CC_ALG"] = 'NO_WAIT'
        jobs[name]["CLIENT_INTERACT"] = 'true'
        jobs[name]["SIM_NET_LATENCY"] = '1'
    
    jobs[name]["MAX_TXNS_PER_THREAD"] = '(150000)'
    
    if alg == 'serial':
        jobs[name]["COMPRESS_LSN_LOG"] = 'false'
        jobs[name]["USE_LOCKTABLE"] = 'true'
        jobs[name]["LOG_BUFFER_SIZE"] = '26214400'
        jobs[name]["MAX_LOG_ENTRY_SIZE"] = '8192'

    if cc_alg == 'DETRESERVE':
        jobs[name]["USE_LOCKTABLE"] = "false"
        jobs[name]["CLIENT_INTERACT"] = "false"
        jobs[name]["PROVER_THREADS"] = 1
    
    if cc_alg == 'DETRESERVEMP':
        jobs[name]["USE_LOCKTABLE"] = "false"
        jobs[name]["CLIENT_INTERACT"] = "false"
        jobs[name]["PROVER_THREADS"] = 75
    
    jobs[name]["MEM_INTEGRITY"] = mem_integrity


    

    
    #jobs[name]["LOG_RECOVER"] = recovery
    #jobs[name]["LOG_GARBAGE_COLLECT"] = gc
    #jobs[name]["LOG_RAM_DISC"] = ramdisc
    #jobs[name]["MAX_TXN_PER_THREAD"] = max_txn



# benchmarks = ['YCSB']
# benchmarks = ['TPCC']
if len(sys.argv) > 1:
    insert_his(*sys.argv[1:])
else:
    benchmarks = ['YCSB', 'TPCC']
    for bench in benchmarks:
        #insert_his('parallel', bench, 'LOG_DATA')
        #insert_his('parallel', bench, 'LOG_COMMAND')
        #insert_his('batch', bench, 'LOG_DATA')
        #insert_his('serial', bench, 'NO_WAIT', 'LOG_DATA')
        
        insert_his('serial', bench, 'DETRESERVE', 'LOG_DATA')
        insert_his('serial', bench, 'DETRESERVEMP', 'LOG_DATA')
        insert_his('serial', bench, 'INTERACT', 'LOG_DATA')
        insert_his('serial', bench, 'INTERACT1ms', 'LOG_DATA')
        insert_his('serial', bench, 'NO_WAIT', 'LOG_DATA')

        # merkle tree
        insert_his('serial', bench, 'DETRESERVE', 'LOG_DATA', 'MERKLE_TREE')
        insert_his('serial', bench, 'DETRESERVEMP', 'LOG_DATA', 'MERKLE_TREE')
        insert_his('serial', bench, 'INTERACT', 'LOG_DATA', 'MERKLE_TREE')
        insert_his('serial', bench, 'INTERACT1ms', 'LOG_DATA', 'MERKLE_TREE')

        # upper bound
        insert_his('serial', bench, 'NO_WAIT', 'LOG_DATA', verification='false')
        insert_his('serial', bench, 'DETRESERVE', 'LOG_DATA', verification='false')
        
       

for (jobname, v) in jobs.items():
    os.system("cp " + dbms_cfg[0] + ' ' + dbms_cfg[1])
    for (param, value) in v.items():
        pattern = r"\#define\s*" + re.escape(param) + r'.*'
        replacement = "#define " + param + ' ' + str(value)
        replace(dbms_cfg[1], pattern, replacement)

    command = "make clean; make -j; cp rundb rundb_%s; cp config.h configs/config_%s" % (jobname, jobname)
    print("start to compile " + jobname)

    subprocess.check_output(command, shell=True) ### for debug

    '''
    proc = subprocess.Popen(command, shell=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    while proc.poll() is None:
        # print proc.stdout.readline()
        commandResult = proc.wait()  # catch return code
        # print commandResult
        if commandResult != 0:
            print("Error in job. " + jobname)
            print(proc.stdout.read())
            # print proc.stderr.read()
            print("Please run 'make' to debug.")
            exit(0)
        else:
            print(jobname + " compile done!")
    '''
