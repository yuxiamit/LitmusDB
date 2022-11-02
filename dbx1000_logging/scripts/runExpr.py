NUM_TRIALS = 4

import os.path
from os import path
import subprocess
from subprocess import TimeoutExpired
import re
import sys
import os
import datetime
import socket
hostname = socket.gethostname()

RUNEXPR_OVERRIDE = int(os.getenv("RUNEXPR_OVERRIDE", '1'))
RUNEXPR_CLEAN = int(os.getenv("RUNEXPR_CLEAN", '0'))
COLLECT_DATA = int(os.getenv("COLLECT_DATA", '0'))

if 'ip' in hostname or 'dev' in hostname:
        DROP_CACHE = "echo 3 > /proc/sys/vm/drop_caches"
else:
        DROP_CACHE = "/usr/local/bin/drop_caches"

RETRY_TIMES = 3
TIMEOUT = None

# we use MaxThr because Throughput sometimes becomes -nan due to 0 running time for other threads for detreserve
normalDBResultRE = re.compile(r'MaxThr:\s+([\d\.\+e\-]+)')
resultRE = re.compile(r'Full-Span-Thr:\s+([\d\.\+e\-]+)')
CCLatRE = re.compile(r'CC Lat:\s+([\d\.\+e\-]+)')
DLatRE = re.compile(r'D Lat:\s+([\d\.\+e\-]+)')
VLatRE = re.compile(r'V Lat:\s+([\d\.\+e\-]+)')
commRE = re.compile(r'\s\s\s\s:\s+([\d\.\+e\-]+)')
modification = 'recover_'

GLOBAL_DIR = ''

def getResDir(modification):
    if GLOBAL_DIR:
        return GLOBAL_DIR
    label = subprocess.check_output(["git", "describe", "--tags"]).strip()
    res = './results/' + modification + label.decode('ascii')
    if not os.path.exists(res):
        os.makedirs(res)
    return res

resDir = getResDir(modification)

paramOverride = ''

# run main experiment

def fillinName(kwargs):
    workload = ''
    log_type = ''
    log_alg = ''
    mem_int = ''
    cc_alg = ''
    params = ''
    for k, v in sorted(kwargs.items()):
        if k == 'workload':
            workload = v
        elif k == 'log_type':
            log_type = v
        elif k == 'log_alg':
            log_alg = v
        elif k == 'cc_alg':
            cc_alg = v.replace('_', '')
        elif k == 'mem_int':
            mem_int = v
        else:
            params += ' -%s%s' % (k, v)
    localParam = ' '
    
    #if log_alg == 'S' and workload == 'TPCC' and 'Tm' in kwargs.keys() and cc_alg == 'NOWAIT':
    #    localParam = ' -Ld4096 '
    if log_alg == 'T' and workload == 'TPCC' and cc_alg == 'SILO' and (kwargs['Lr'] == 1 or kwargs['Lr'] == '1'):
        localParam = ' -Lb262144000 ' # improve unscaling issue on t56   ---see diary 13

    return 'rundb_%s%s%s_%s_%s%s' % (log_alg, log_type, mem_int, workload, cc_alg, params), localParam

from shutil import copyfile

def grab(content, key):
    return float(re.findall(key + r':\s*([\d\.\+e\-]+)', content)[0])

brkList = ["time_proverProcessBatches", "time_generateConstraintSystem", "time_generateKey", "time_generateProof", "time_verify", "time_outputProof"]

def parseContent(content, target):
    if target == 1: # throughput
        return float(resultRE.findall(content)[0])
        
    elif target == 2: # latency
        return float(CCLatRE.findall(content)[0]) + float(DLatRE.findall(content)[0]) + float(VLatRE.findall(content)[0])

    elif target == 3: # comm
        return float(commRE.findall(content)[0])
    
    elif target == 4: # timeBreakDown
        brkListVals = [ grab(content, brk) for brk in brkList ]
        totalTime = sum(brkListVals)
        return [brk / totalTime for brk in brkListVals]
    
    elif target == 6: # timeBreakDownAbsolute
        brkListVals = [ grab(content, brk) for brk in brkList ]
        return [brk for brk in brkListVals]
    
    elif target == 5: # normal db throughput
        # print(content)
        return float(normalDBResultRE.findall(content)[0])

    return 0

def runExpr(xLabel, xRange, resDir=resDir, num_trials=NUM_TRIALS, resultRE=resultRE, **kwargs):
    algP = [[] for _ in range(num_trials)]
    global DROP_CACHE
    # copy configs
    copyfile('tools/compile.py', resDir + '/compile.py')
    copyfile('scripts/runExpr.py', resDir + '/runExpr.py')
    copyfile('config-std.h', resDir + '/config-std.h')
    if isinstance(num_trials, list):
        trialRange = num_trials
    else:
        assert isinstance(num_trials, int)
        trialRange = range(num_trials)
    for k in trialRange: # range(num_trials):
        for x in xRange:
            kw = kwargs
            kw['LR'] = 1
            kw[xLabel] = x
            commandLine, localParameterOverride = fillinName(kw)
            fullCommand = commandLine + localParameterOverride + paramOverride
            

            fileName = resDir + '/' + commandLine.replace(' -', '_') + '_%d.txt' % k

            if COLLECT_DATA > 0:
                rawContent = open(fileName, 'r').read()
                thr = parseContent(rawContent, COLLECT_DATA)
                algP[k].append((x, thr))
                print((commandLine, x, thr))
                continue

            print(fullCommand, datetime.datetime.now())

            if (RUNEXPR_OVERRIDE == 0) and path.exists(fileName):
                print('Skip', fileName)
                continue

            if (RUNEXPR_CLEAN == 1) and path.exists(fileName):
                os.remove(fileName)
                continue
                
            subprocess.check_output(DROP_CACHE, shell=True) # drop the cache
            retry = True
            count = 0
            while retry:
                count += 1
                try:
                
                    # ret = subprocess.check_output("numactl -i all -- ./" + commandLine + paramOverride, shell=True).decode('ascii')
                
                    ret = subprocess.check_output("numactl -i all -- ./" + fullCommand, shell=True, timeout=TIMEOUT).decode('ascii')
                    retry = False
                    break
                except TimeoutExpired:
                    print('Time out')
                    open("timeout.log", "a+").write(fullCommand + '\n')
                    retry = True
                    if count > RETRY_TIMES:
                        ret = 'Throughput: 0\n Broken' # dummy data
                        break

            # print(ret)
            open(fileName, 'w').write(ret)
            # print ret
            thr = float(resultRE.findall(ret)[0])
            algP[k].append((x, thr))
            # print((commandLine, x, thr))
        print('Epoch %d finished' % k)
    res = []
    for t in range(len(algP[0])):
        x, thr = algP[0][t]
        for k in range(1, num_trials):
            if type(thr) is list:
                # multiple values
                for thr_id in range(len(thr)):
                    thr[thr_id] += algP[k][t][1][thr_id]
            else:
                thr += algP[k][t][1]
        if type(thr) is list:
            res.append((x, [float(thr[thr_id]) / num_trials  for thr_id in range(len(thr))]))
        else:
            res.append((x, float(thr)/ num_trials))
    print(res)

def test():
    # for temp local test
    for workload in ['TPCC']: # ['YCSB', 'TPCC']:
        for Lr in [0, 1]:
            runExpr('t', [56], './temp', 1,
                    workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', [56], './temp', 1,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            runExpr('t', [56], './temp', 1,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)
    sys.exit(0)

def rm():
    subprocess.check_output("rm /f0/yuxia/*", shell=True) # drop the old files
    subprocess.check_output("rm /f1/yuxia/*", shell=True)
    subprocess.check_output("rm /f2/yuxia/*", shell=True)
    subprocess.check_output("rm /data/yuxia/*", shell=True)

def LogNum():
    lns = [1, 2, 3, 4]
    resDirShort = getResDir('lognum')
    numTrials = 1
    shortX = [24] # [4, 16, 32, 48, 56]
    global paramOverride
    paramOverride = '-R2 -z0.6 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.05 -Lb10485760' # normal
    
    for ln in lns:
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=ln, Lr=0)
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=ln, Lr=1)
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=ln, Lr=0)
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=ln, Lr=1)
        
    sys.exit(0)

def LogNumEC2():
    lns = [4]
    resDirShort = getResDir('lognum')
    numTrials = 1
    shortX = [48] # [4, 16, 32, 48, 56]
    global paramOverride
    paramOverride = '-R2 -z0.6 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.05 -Lb10485760 -Gx10000' # normal
    
    for ln in lns:
        #runExpr('t', shortX, resDirShort, numTrials,
        #                workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=ln, Lr=0)
        #runExpr('t', shortX, resDirShort, numTrials,
        #                workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=ln, Lr=1)
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=ln, Lr=0)
        runExpr('t', shortX, resDirShort, numTrials,
                        workload='YCSB', log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=ln, Lr=1)
        
    sys.exit(0)

def sensitivityCompression():
    resDirSens = getResDir('compress')
    numTrials = 4
    defaultT = 16
    global paramOverride
    
    # -Ln4 -Lr0 -t4 -z0.0 -Lt1 -R -r0.8 -Tp1 -Tl30000000 -Gx10000
    tpLevel = [100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, 10000000000] # , 100000, 1000000, 10000000]
    
    
    # paramOverride = ' -z0.0 -Lt1 -R1 -r0.8 -Tl30000000 -Gx10000' # increase logging frequency
    '''paramOverride = ' -z0.6 -R2 -r0.5 -Tl30000000 -Gx10000' # increase logging frequency
    for tp in tpLevel:
        for Lr in [0, 1]: # [0,1]:
            for workload in ['YCSB']: # , 'TPCC']:
                #runExpr('Tp', [tp], resDirSens, numTrials, 
                #        workload=workload, log_type='D', log_alg ='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, t=defaultT)
                runExpr('Tp', [tp], resDirSens, numTrials, 
                        workload=workload, log_type='C', log_alg ='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, t=defaultT)
    '''
    # sys.exit(0)
    # paramOverride = ' -z0.6 -n4' # we need appropriate contention for locktable to contain items
    # sys.exit(0)

    '''paramOverride = ' -z0.5 -R1 -r0.5 -Tl30000000 -Gx10000' # increase logging frequency
    for tp in tpLevel:
        for Lr in [0, 1]: # [0,1]:
            for workload in ['YCSB']: # , 'TPCC']:
                #runExpr('Tp', [tp], resDirSens, numTrials, 
                #        workload=workload, log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
                runExpr('Tp', [tp], resDirSens, numTrials, 
                        workload=workload, log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
    # sys.exit(0)
    # paramOverride = ' -z0.6 -n4' # we need appropriate contention for locktable to contain items
    sys.exit(0)'''
    paramOverride = ' -z0.6 -R2 -n16 -Tp0 -l1 -Gx10000'
    defaultT = 4
    tlLevel = [3000, 30000, 300000, 3000000, 30000000, 300000000]
    for tl in tlLevel:
        for Lr in [0, 1]: # [0,1]:
            defaultT = 4 + Lr * 24 # we want to demonstrate high tlLevel could lead to better parallelism in recovery
            for workload in ['YCSB']: # , 'TPCC']:
                runExpr('Tl', [tl], resDirSens, numTrials, 
                        workload=workload, log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
                runExpr('Tl', [tl], resDirSens, numTrials, 
                        workload=workload, log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
        
    sys.exit(0)

def sensitivity():
    resDirSens = getResDir('sensitivity')
    numTrials = 1
    defaultT = 28
    cc = 'NO_WAIT'
    global paramOverride
    zLevel = [0.99, 1.2, 1.4, 1.6] 
    # zLevel = [0.0, 0.2, 0.4, 0.6, 0.8, 0.9, 0.95, 0.99, 1.2, 1.4, 1.6] # , 1.0, 1.2, 1.4]
    # paramOverride = ' -R6 ' # to increase contention
    paramOverride = '-R2 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.05 -Lb10485760' # normal
    for zl in zLevel:
        for Lr in [0, 1]: # [0, 1]:
            runExpr('z', [zl], resDirSens, numTrials, 
                    workload = 'YCSB', log_type='D', log_alg ='S', cc_alg=cc, Ln=1, Lr=Lr, t=defaultT)
            runExpr('z', [zl], resDirSens, numTrials, 
                    workload = 'YCSB', log_type='C', log_alg ='S', cc_alg=cc, Ln=1, Lr=Lr, t=defaultT)
            runExpr('z', [zl], resDirSens, numTrials, 
                    workload = 'YCSB', log_type='D', log_alg ='B', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
            runExpr('z', [zl], resDirSens, numTrials, 
                workload = 'YCSB', log_type='D', log_alg ='T', cc_alg=cc, Ln=4, Lr=Lr, t=defaultT)
            runExpr('z', [zl], resDirSens, numTrials, 
                workload = 'YCSB', log_type='C', log_alg ='T', cc_alg=cc, Ln=4, Lr=Lr, t=defaultT)
        runExpr('z', [zl], resDirSens, numTrials, 
                workload = 'YCSB', log_type='D', log_alg ='N', cc_alg=cc, Ln=4, Lr=0, t=defaultT)
    sys.exit(0)
    
    defaultT = 28 # 48
    nLevel = [1, 2, 4, 8, 16, 32, 64] # 1, 2, 
    for nl in nLevel:
      for Tm in [0.0, 1.0]:
        for Lr in [0, 1]: # [0,1]:
            runExpr('n', [nl], resDirSens, numTrials,
                    workload = 'TPCC', log_type='D', log_alg ='S', cc_alg=cc, Ln=1, Lr=Lr, t=defaultT, Tm=Tm)
            runExpr('n', [nl], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='C', log_alg ='S', cc_alg=cc, Ln=1, Lr=Lr, t=defaultT, Tm=Tm)
            runExpr('n', [nl], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='D', log_alg ='B', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT, Tm=Tm)
            runExpr('n', [nl], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='D', log_alg ='T', cc_alg=cc, Ln=4, Lr=Lr, t=defaultT, Tm=Tm)
            runExpr('n', [nl], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='C', log_alg ='T', cc_alg=cc, Ln=4, Lr=Lr, t=defaultT, Tm=Tm)
    
    sys.exit(0)
    # Contention Level
    # there must be at least 2 ware houses in order for gen_payment to work.
    
    
    

     # Gx: transactions per thread
    gxLevel = [150, 1500, 15000, 150000, 1500000]
    for gx in gxLevel:
        for Lr in [0,1]:
            runExpr('Gx', [gx], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='D', log_alg ='S', cc_alg='SILO', Ln=1, Lr=Lr, t=defaultT)
            runExpr('Gx', [gx], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='C', log_alg ='S', cc_alg='SILO', Ln=1, Lr=Lr, t=defaultT)
            runExpr('Gx', [gx], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='D', log_alg ='B', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
            runExpr('Gx', [gx], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
            runExpr('Gx', [gx], resDirSens, numTrials, 
                    workload = 'TPCC', log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
    sys.exit(0)
    # Log Buffer Size
    lbLevel = [int(524288000/8**7 * 8**i) for i in range(9)]
    for workload in ['YCSB', 'TPCC']:
      for Lr in [0,1]:
        runExpr('Lb', lbLevel, resDirSens, numTrials, 
            workload = workload, log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
        runExpr('Lb', lbLevel, resDirSens, numTrials, 
            workload = workload, log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
    # Lock Table Size
    lSize = [50, 100, 500, 1000, 2000, 5000, 10000]
    for workload in ['YCSB', 'TPCC']:
      for Lr in [0,1]:
        runExpr('l', lSize, resDirSens, numTrials, 
            workload = workload, log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
        runExpr('l', lSize, resDirSens, numTrials, 
            workload = workload, log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
    # Flush Interval
    ltLevel = [1000 * r for r in [60, 600, 6000, 60000, 600000, 6000000]]
    for workload in ['YCSB', 'TPCC']:
      for Lr in [0,1]:
        runExpr('Lt', ltLevel, resDirSens, numTrials, 
            workload = workload, log_type='D', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
        runExpr('Lt', ltLevel, resDirSens, numTrials, 
            workload = workload, log_type='C', log_alg ='T', cc_alg='SILO', Ln=4, Lr=Lr, t=defaultT)
    sys.exit(0)

def shortSingleSerial():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 16, 32, 48, 56]
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=1)

def shortSingleBatch():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 16, 32, 48, 56]
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=1)
    
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=1)



def shortSingleTaurus():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 16, 32, 48, 56]
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)

def shortSingleYCSBTaurus():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 16, 32, 48, 56]
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='YCSB', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    
    sys.exit(0)

def shortSingleTPCCTaurus():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 16, 32, 48, 56]
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    runExpr('t', shortX, resDirShort, numTrials,
                    workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    
    sys.exit(0)

def shortSingleTPCC():
    resDirShort = getResDir('short')
    numTrials = 1
    shortX = [4, 12, 20, 28, 36, 44, 52, 60]
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #sys.exit(0)

    paramOverride = '-R2 -n4 -Ls0.05 -Lb10485760'

    for workload in ['TPCC']: # 'TPCC']: # ['YCSB']: # ['YCSB', 'TPCC']:
      for Tm in [0.0, 1.0]:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, Tm=Tm)
            
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)

            continue

            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            
            # continue
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            # continue     
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='SILO', Ln=4, Lr=0, Tm=Tm)
    sys.exit(0)

def short():
    resDirShort = getResDir('short')
    numTrials = 1 # 4 #4# 4
    shortX = [1, 2, 4, 8, 12, 16, 20, 24, 28] # , 36, 44, 52, 60]
    # shortX = [4, 12, 20, 28, 36, 44, 52, 60]
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #sys.exit(0)
    global paramOverride# parameterOverride
    # paramOverride = '-R1 -z0.6 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.05 -Lb10485760'
    '''paramOverride = '-R2 -z0.6 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.05 -Lb10485760' # normal
    # paramOverride = '-R2 -z0.8 -r0.5 -Ls0.05 -Lb10485760'

    for workload in ['YCSB']: # 'TPCC']: # ['YCSB']: # ['YCSB', 'TPCC']:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)

            continue

            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            
            # continue
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            # continue     
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='NO_WAIT', Ln=4, Lr=0)

    # sys.exit(0)'''

    # paramOverride = '-R2 -n4 -Ls0.05 -Lb10485760'
    paramOverride = '-R2 -n64 -Ls0.05 -Lb10485760'
    for workload in ['TPCC']: # 'TPCC']: # ['YCSB']: # ['YCSB', 'TPCC']:
      for Tm in [0.0, 1.0]:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='NO_WAIT', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr, Tm=Tm)
            
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)

            continue

            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            
            # continue
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            # continue     
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='NO_WAIT', Ln=4, Lr=0, Tm=Tm)
    sys.exit(0)

def shortSilo():
    resDirShort = getResDir('shortSilo')
    numTrials = 1 # 4
    shortX = [1, 2, 4, 8, 12, 16, 20, 24, 28] # , 36, 44, 52, 60]
    # shortX = [4, 12, 20, 28, 36, 44, 52, 60]
    # shortX = [4, 16, 32, 48, 56]
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=0)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #runExpr('t', shortX, resDirShort, numTrials,
    #                workload='TPCC', log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=1)
    #sys.exit(0)
    global paramOverride# parameterOverride
    # paramOverride = ' -R8 -z0.6 -r0.5 -Tp0 -Tl30000000 -Ls0.05' # -Lb10485760' # on paper draft on July 22
    # Ls0.23 -Lb3145720
    paramOverride = '-R2 -z0.6 -n16 -r0.5 -l80003 -Tp0 -Tl30000000 -Ls0.15 -Lb31457280' # -Ls0.23 -Lb20971520' # -Lb10485760' # -Lb10485760'
    for workload in ['YCSB']: # 'TPCC']: # ['YCSB']: # ['YCSB', 'TPCC']:
        for Lr in [0, 1]: # [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)

            continue

            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            
            # continue
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            # continue     
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='SILO', Ln=4, Lr=0)
    # sys.exit(0)

    for workload in ['TPCC']: # 'TPCC']: # ['YCSB']: # ['YCSB', 'TPCC']:
      for Tm in [0.0, 1.0]:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)

            continue

            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr, Tm=Tm)
            
            # continue
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
            #runExpr('t', shortX, resDirShort, numTrials,
            #        workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            
            # continue     
            
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr, Tm=Tm)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='SILO', Ln=4, Lr=0, Tm=Tm)
    sys.exit(0)

def main():
    resDirShort = getResDir('main')
    shortX = [4, 8, 16, 20, 28, 32, 40, 48, 56, 60]
    numTrials = 4
    for workload in ['YCSB', 'TPCC']:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='NO_WAIT', Ln=1, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='NO_WAIT', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='C', log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)
        runExpr('t', shortX, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='N', cc_alg='SILO', Ln=4, Lr=0)
    sys.exit(0)

def litmusProverThreads():
    resDirShort = getResDir('litmus-proverthreads')
    numTrials = 3
    vpList = [8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    # vpList = [1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    workload = 'YCSB'
    Lr=0
    global paramOverride
    paramOverride = ' -Gx100000 '
    runExpr('Vp', vpList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gx=163840)

def litmusProverThreadsRAMDISK():
    resDirShort = getResDir('litmus-proverthreadsRAM')
    numTrials = 1
    vpList = [8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96] # [1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    workload = 'YCSB'
    Lr=0
    global paramOverride
    paramOverride = ' -LR1 '
    runExpr('Vp', vpList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gz=81920, Gx=655360 * 4)

def litmusProverThreadsRAMDISKTPCC():
    resDirShort = getResDir('litmus-proverthreadsRAM')
    numTrials = 1
    vpList = [8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96] # [1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    workload = 'TPCC'
    Lr=0
    global paramOverride
    paramOverride = ' -LR1 '
    
    runExpr('Vp', vpList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gz=81920, Gx=655360 * 4, Tm=0)
    runExpr('Vp', vpList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gz=81920, Gx=655360 * 4, Tm=1)

def litmusBatchSize():
    resDirShort = getResDir('litmus-batchsize')
    numTrials = 1
    gxList = [80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360] # [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
    workload = 'YCSB'
    Lr=0
    global paramOverride
    paramOverride = ' -LR1 ' # ' -Gx10240 '
    runExpr('Gz', gxList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gx=655360, Vp=75)
    runExpr('Gz', gxList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVE', mem_int='R', Ln=1, Lr=Lr, Gx=10240)
                    # when batch size is small, this downgrades to a sequential
    runExpr('Gz', gxList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVE', mem_int='N', Ln=1, Lr=Lr, Gx=655360)

def litmusBatchSizeTPCC():
    resDirShort = getResDir('litmus-batchsize')
    numTrials = 1
    gxList = [5, 10, 20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360] # [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
    workload = 'TPCC'
    Lr=0
    global paramOverride
    paramOverride = ' -LR1 ' # ' -Gx10240 '
    tpccFactor = 16
    for Tm in [0, 1]:
        runExpr('Gz', gxList, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gx=655360 // tpccFactor, Vp=75, Tm=Tm)
        runExpr('Gz', gxList, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVE', mem_int='R', Ln=1, Lr=Lr, Gx=10240 // tpccFactor, Tm=Tm)
                        # when batch size is small, this downgrades to a sequential
        runExpr('Gz', gxList, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVE', mem_int='N', Ln=1, Lr=Lr, Gx=655360 // tpccFactor, Tm=Tm)


def litmusTxnLength():
    pass

def litmusContention():
    resDirShort = getResDir('litmus-contention')
    numTrials = 1 #3
    gxList = [8192]# [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2]
    workload = 'YCSB'
    Lr=0
    zLevel  = [0.0, 0.2, 0.4, 0.6, 0.8, 0.9, 0.95, 0.99, 1.2, 1.4, 1.6]
    runExpr('z', zLevel, resDirShort, numTrials,
                   workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVE', Ln=1, Lr=Lr, Gx=81920)
    runExpr('z', zLevel, resDirShort, numTrials,
                   workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='NOWAIT', Ln=1, Lr=Lr, Gx=2560)
    runExpr('z', zLevel, resDirShort, numTrials,
                   workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT', Ln=1, Lr=Lr, Gx=5120)
    runExpr('z', zLevel, resDirShort, numTrials,
                   workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, Gx=40960)

    
    runExpr('z', zLevel, resDirShort, numTrials,
                   workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, Gx=128)
    
    #runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
    #               workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT', Ln=1, Lr=Lr)

    # gxList = [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
    global paramOverride
    # paramOverride = ' -Vp60 '
    runExpr('z', zLevel, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVEMP', Ln=1, Lr=Lr, Vp=75, Gx=655360)

    runExpr('z', zLevel, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='NOWAIT', Ln=1, Lr=Lr, Gx=655360, t=64)
    
    runExpr('z', zLevel, resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='DETRESERVE', Ln=1, Lr=Lr, Gx=655360)

def litmusDataSize():
    resDirShort = getResDir('litmus-proverthreadsRAM')
    numTrials = 1
    sList = [1024 * 1024 * 10 * (2**i) for i in range(11) ] # [1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    workload = 'YCSB'
    Lr=0
    global paramOverride
    paramOverride = ' -LR1 '
    runExpr('s', sList, resDirShort, numTrials,
                    workload=workload, log_type='D', log_alg='S', cc_alg='DETRESERVEMP', mem_int='R', Ln=1, Lr=Lr, Gz=81920, Gx=655360 * 4)
    

def litmusDRL(dir_name = 'litmus-drl'):
    resDirShort = getResDir(dir_name)
    numTrials = 1 #3
    gxList = [8192]# [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2]
    
    Lr=0
    LR=1

    for workload in ['YCSB']:
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT', Ln=1, Lr=Lr, LR=1)
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, LR=1)
            #runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
            #            workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, LR=1)
            
            #runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
            #               workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT', Ln=1, Lr=Lr)

            # gxList = [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
            global paramOverride
            # paramOverride = ' -Vp75 '
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2, 655360 * 4], resDirShort, numTrials,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVEMP', Ln=1, Lr=Lr, Vp=96, LR=1)
            # 10000000, ~8000
            ##########################

def litmus(dir_name = 'litmus-load'):
    resDirShort = getResDir(dir_name)
    numTrials = 1 #3
    gxList = [8192]# [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2]
    
    Lr=0
    LR=1

    for workload in ['YCSB']:

            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVE', Ln=1, Lr=Lr, LR=1)
            #runExpr('Gx', [20, 40, 80, 120, 160, 320, 640, 1280, 2560], resDirShort, numTrials,
            runExpr('Gx', [20, 40, 80, 120, 160, 320], resDirShort, numTrials, # due to too large keys
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='NOWAIT', Ln=1, Lr=Lr, LR=1)
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT', Ln=1, Lr=Lr, LR=1)
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, LR=1)

            
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, LR=1)
            
            #runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
            #               workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT', Ln=1, Lr=Lr)

            # gxList = [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
            global paramOverride
            # paramOverride = ' -Vp75 '
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2, 655360 * 4], resDirShort, numTrials,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVEMP', Ln=1, Lr=Lr, Vp=75, LR=1)

            ##########################

            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2, 655360 * 4], resDirShort, 3,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='NOWAIT', Ln=1, Lr=Lr, LR=1, t=64)
            
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2, 655360 * 4], resDirShort, 3,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='NOWAIT', Ln=1, Lr=Lr, LR=1)
            
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2, 655360 * 4], resDirShort, numTrials,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='DETRESERVE', Ln=1, Lr=Lr, LR=1)

def litmusTPCCbs5():
    global paramOverride
    paramOverride = ' -Gz5 '
    litmusTPCC(dir_name='litmus-load-bs5')
    

def litmusTPCC(dir_name = 'litmus-load'):
    resDirShort = getResDir(dir_name)
    numTrials = 1 #3
    gxList = [8192]# [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360, 655360 * 2]
    
    Lr=0
    LR=1

    for workload in ['TPCC']:
        for Tm in [0, 1]:

            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVE', Ln=1, Lr=Lr, Tm=Tm, LR=1)
            runExpr('Gx', [20, 40, 80, 120, 160, 320, 640, 1280], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='NOWAIT', Ln=1, Lr=Lr, Tm=Tm, LR=1)
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT', Ln=1, Lr=Lr, Tm=Tm, LR=1)
            runExpr('Gx', [20, 80, 160, 320, 640, 1280, 2560, 5120], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, Tm=Tm, LR=1)

            
            runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
                        workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT1ms', Ln=1, Lr=Lr, Tm=Tm, LR=1)
            
            #runExpr('Gx', [8, 64, 128], resDirShort, numTrials,
            #               workload=workload, log_type='D', log_alg='S', mem_int = 'M', cc_alg='INTERACT', Ln=1, Lr=Lr)

            # gxList = [160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920, 163840, 327680, 655360]
            global paramOverride
            # paramOverride = ' -Vp75 '

            gxList = [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920] # , 163840] dev9 has only 177G memory, out of memory for 163840 bs5 Tm0

            if Tm == 1:
                # avoid Assertion `base_pri > size' failed.
                gxList = [20, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960]

            runExpr('Gx', gxList, resDirShort, numTrials,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'R', cc_alg='DETRESERVEMP', Ln=1, Lr=Lr, Vp=75, Tm=Tm, LR=1)

            ##########################

            runExpr('Gx', gxList, resDirShort, 3,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='NOWAIT', Ln=1, Lr=Lr, Tm=Tm, LR=1, t=64)
            
            runExpr('Gx', gxList, resDirShort, 3,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='NOWAIT', Ln=1, Lr=Lr, Tm=Tm, LR=1)
            
            runExpr('Gx', gxList, resDirShort, numTrials,
                                workload=workload, log_type='D', log_alg='S', mem_int = 'N', cc_alg='DETRESERVE', Ln=1, Lr=Lr, Tm=Tm, LR=1)

if __name__ == '__main__':

    if len(sys.argv) > 1:
        if len(sys.argv) > 2:
            GLOBAL_DIR = sys.argv[2]
            if len(sys.argv) > 3:
                paramOverride = ''.join(sys.argv[3:])
                if paramOverride[0] != ' ':
                    paramOverride = ' ' + paramOverride
                print('Param overriden:', str(paramOverride))
        starttime = datetime.datetime.now()
        eval(sys.argv[1] + '()', globals(), locals())
        endtime = datetime.datetime.now()
        print('time', endtime - starttime)

    print('Finished')
    sys.exit(0)

    # NO logging baseline
    longX = [1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64]
    shortX = [4, 8, 16, 20, 28, 32, 40, 48, 56, 60]

    # temp

    workload = 'TPCC'
    for log_type in ['D', 'C']:
        for Lr in [0, 1]:
            runExpr('t', shortX, resDir,
                workload=workload, log_type=log_type, log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
            runExpr('t', shortX, resDir,
                workload=workload, log_type=log_type, log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)


    sys.exit(0)

    # baseline for LOG_NO
    #for workload in ['YCSB', 'TPCC']:
    #  runExpr('t', shortX, workload='YCSB', log_type='O', log_alg='N', cc_alg='SILO', Ln=1, Lr=0)

    # SiloR recovery scalability
    for workload in ['YCSB', 'TPCC']:
        for log_type in ['D']:
            for Lr in [0, 1]: # Lr should be in the inner loop because we share same names in different CC
                runExpr('t', shortX, resDir,
                    workload=workload, log_type=log_type, log_alg='B', cc_alg='SILO', Ln=4, Lr=Lr)
        for log_type in ['D', 'C']:
            for Lr in [0, 1]:
                runExpr('t', shortX, resDir,
                    workload=workload, log_type=log_type, log_alg='T', cc_alg='SILO', Ln=4, Lr=Lr)
                runExpr('t', shortX, resDir,
                    workload=workload, log_type=log_type, log_alg='S', cc_alg='SILO', Ln=1, Lr=Lr)

    sys.exit(0)

    #=================
    # main experiment
    #=================

    for workload in ['YCSB']:
        for log_type in ['C']:
            for Lr in [0, 1]:
                runExpr('t', [1, 2, 5, 10, 15, 20, 25, 30, 36, 40, 42, 48, 52, 56, 60, 64], resDir,
                    workload=workload, log_type=log_type, log_alg='T', Ln=4, Lr=Lr)
