from matplotlib import pyplot as plt
from os import listdir
from os.path import isfile, join
import subprocess
import re
import copy
import math
import numpy as np
from matplotlib.backends.backend_pdf import PdfPages
from collections import defaultdict

plt.rcParams.update({'font.size': 18})

FIG_DIR = './figs/'
sampleConfig = {
    'Ln':4,
    'Lr':0,
    'Tl':300,
    'n':4,
}
nameRE = re.compile(r'rundb_(?P<log_alg>[A-Z])(?P<log_type>[A-Z])_(?P<workload>[A-Z]+)_(?P<cc_alg>[A-Z]+)(_Gx(?P<txn_num>\d+))?_Ln(?P<log_num>\d+)_Lr(?P<rec>\d)(_Tl(?P<Tl>\d+))?(_Tm(?P<Tm>[0-9]*\.?[0-9]*))?(_Tp(?P<Tp>\d+))?(_n(?P<num_wh>\d+))?_t(?P<thd_num>\d+)(_z(?P<zipf_theta>[0-9]*\.?[0-9]*))?_(?P<trial>\d+)')

colorScheme = dict(B='#7EA1BF', S='#A68C6D', N='#36A59A', T='#FF4847',
    time_io='#7EA1BF') 
hatchScheme = dict(
    time_io='/'
)
markerScheme = dict(NOWAIT='o', SILO='1', B='s', S='x', T='d', N='.')
lineScheme = dict(D='--', C='-', O=':')
labelDict = dict(
    log_type='Log Type',
    log_alg='Log Algorithm',
    thd_num='Number of Worker Threads',
    num_wh='Number of Warehouses',
    zipf_theta='Zipf Theta',
    txn_num='Max Number of Transactions per Thread',
    Tp=r'PLV Flush Frequency ($\rho$)',
    Tl='Locktable Eviction Threshold',
    Tm='TPCC Percentage',
    log_bytes='Total Log Size',
    log_num="The Number of Loggers",
    Throughput="Throughput (million txn/sec)",
    int_aux_bytes="Average Size of Auxiliary Bytes",
    locktable_avg_volume="Average Volume of Hash Table (k)",
)
tDict = dict(
    log_type=dict(
        D='Data',
        C='Command',
        O='',
    ),
    rec={'0':'Logging', '1':'Recovery'},
    log_alg=dict(
        B='SiloR',
        S='Serial',
        T='Taurus',
        N='No Logging'
    ),
    cc_alg=dict(
        NOWAIT='2PL',
        SILO='Silo'
    ),
    log_num={str(s):"Log Num %d" % s for s in [1, 2, 4]}
)
import zlib

def hashDictVal(dic, key):
    t = sorted(list(dic.values()))
    return t[zlib.adler32(key.encode('ascii')) % len(t)]


def chooseBarStyle(dic, measure):
    styleDict = dict()
    styleDict['color'] = hashDictVal(colorScheme, measure + dic['log_alg'] + dic['log_type'] + dic['cc_alg'])
    return styleDict

def chooseColor(dic):
    pass

def chooseStyle(dic):
    styleDict = dict(linewidth=2)
    styleDict['color'] = colorScheme[dic['log_alg']] # hashDictVal(colorScheme, dic['log_alg']) #  + dic['log_num'])
    styleDict['linestyle'] = lineScheme[dic['log_type']]
    # styleDict['marker'] = markerScheme[dic['cc_alg']]
    styleDict['marker'] = markerScheme[dic['log_alg']]
    return styleDict

def translateDict(dic):
    mydic = copy.deepcopy(dic)
    for k, v in mydic.items():
        if k in tDict.keys() and v in tDict[k].keys():
            mydic[k] = tDict[k][v]
    return mydic

def fetchData(resDir):
    global commandLineDir
    if commandLineDir != '':
        resDir = commandLineDir
    onlyfiles = [f for f in listdir(resDir) if f[-4:] == '.txt' and isfile(join(resDir, f))]
    return onlyfiles

def getRange(dataFiles, label):
    return list(set(map(lambda s: nameRE.match(s).groupdict()[label], dataFiles)))

def fileContentRes(fileName, item, resDir):
    global commandLineDir
    if commandLineDir != '':
        resDir = commandLineDir
    content = open(resDir + '/' + fileName, 'r').read()
    def grab(key):
        return float(re.findall(key + r':\s+([\d\.\+e\-]+)', content)[0])
    #print(content)
    #print(item + r':\s+([\d\.\+e\-]+)')
    if item == 'locktable_avg_volume':
        return grab('int_locktable_volume') / grab('int_num_get_row')
    if item == 'int_aux_bytes':
        return grab('int_aux_bytes') / grab('num_commits') # grab('num_log_entries')

    return grab(item)
    
def stringify(dic):
    for k, v in dic.items():
        if not isinstance(v, str):
            dic[k] = str(v)
    return dic

def dummy(finalRes):
    # output nothing
    return

def drawFig(dataFiles, dataDir, measure, xLabel, groupLabel, completeFunc, groupHidden=[], figPrefix='', auxDataOutput=dummy, **kwargs):
    print(dataDir)
    kwargs = stringify(kwargs)
    def genFilterFunc(dic):
        def filterFunc(t):
            matchedraw = nameRE.match(t)
            if matchedraw == None:
                return False
            matched = matchedraw.groupdict()
            for k,v in dic.items():
                if not k in matched or matched[k] is None or matched[k] != v:
                    # print(matched, k, v)
                    return False
            return True
        return filterFunc
    corFiles = list(filter(genFilterFunc(kwargs), dataFiles))
    # print(corFiles)
    if len(corFiles) == 0:
        return # no data is available
    xRange = getRange(corFiles, xLabel)
    
    if None in xRange:
        xRange.remove(None)

    sortedX = sorted([float(x) for x in xRange])
    groupLabels = groupLabel.split('.')
    groupRanges = [getRange(corFiles, gl) for gl in groupLabels]

    trialRange = getRange(corFiles, 'trial')
    # print(corFiles[0])
    numParameter = len({k:v for k, v in nameRE.match(corFiles[0]).groupdict().items() if v != None}.keys())
    print('Parameter detected', nameRE.match(corFiles[0]).groupdict())
    f = plt.figure(figsize=(32, 20))
    f, ax = plt.subplots(1)
    ax.yaxis.grid(True)
    # do not set to sci.
    #if measure == 'Throughput':
    #    ax.ticklabel_format(axis='y', style='sci', scilimits=(0,0), useOffset=True)
    #    print('y axis set to sci.')
    # print(len(xRange), (float(xRange[-1]) - float(xRange[0])) / (float(xRange[1]) - float(xRange[0])), 10 * len(xRange))
    if len(xRange) > 2 and (float(sortedX[-1]) - float(sortedX[0])) / (float(sortedX[1]) - float(sortedX[0])) > 10 * len(sortedX):
        ax.set_xscale('log')
    plt.xlabel(labelDict[xLabel])
    plt.ylabel(labelDict[measure.replace(':','')])
    currentDict = [copy.deepcopy(kwargs)]
    currentLabel = ['']
    groupCounter = [0]
    finalRes = {}
    def recursiveFill(ind):
        if ind == len(groupLabels):
            xyList = []
            
            # print(xRange)
            for x in sorted(xRange):
                currentDict[0][xLabel] = x
                # print(currentDict[0])
                currentDict[0] = completeFunc(currentDict[0])
                # print(numParameter, len(currentDict[0].keys()), currentDict[0].keys())
                assert(numParameter == len(currentDict[0].keys()) + 1)
                currentList = list(filter(genFilterFunc(currentDict[0]), corFiles))
                numTrials = len(currentList)
                if numTrials < len(trialRange):
                    print('numTrials < len(trialRange)', numTrials, len(trialRange))
                    continue
                # print(currentList, len(trialRange))
                assert(numTrials == len(trialRange))
                numList = []
                for realFile in currentList:
                    val = fileContentRes(realFile, measure, dataDir)
                    print(realFile, measure, val)
                    numList.append(val)
                
                yValue = sum(numList) / float(numTrials)
                print(x, numList)
                quality = np.std(numList) / yValue
                if quality > 0.1:
                    print('Warning', (x, yValue, np.std(numList) / yValue, numList))
                if measure == 'Throughput':
                    yValue = yValue / 1e6
                if measure == 'locktable_avg_volume':
                    yValue = yValue / 1e3
                xyList.append((float(x), yValue, np.std(numList)))
                # print(x, yValue)
            
            if len(xyList) == 0:
                return
            plotStyle = chooseStyle(currentDict[0])
            print('plotted', currentLabel[0], plotStyle)
            legendLabel = currentLabel[0][:-1]
            if 'No Logging' in legendLabel:
                legendLabel = 'No Logging'
            if '2PL' in legendLabel:
                legendLabel = legendLabel.replace(' 2PL','')
            plotStyle['label'] = legendLabel
            
            xList, yList, errorList = zip(*sorted(xyList, key=lambda xy: float(xy[0])))
            finalRes[currentLabel[0][:-1]] = [xList, yList, errorList]
            print(xList, yList, errorList)
            plt.plot(xList, yList, **plotStyle)
            groupCounter[0] += 1
            # plt.errorbar(xList, yList, yerr=errorList, **plotStyle)
            return
            # the graph is well-defined, #kwargs + x + group + num_trial
        tempLabel = copy.deepcopy(currentLabel[0])
        for r in groupRanges[ind]:
            if r in groupHidden:
                continue
            currentDict[0][groupLabels[ind]] = r
            currentLabel[0] = tempLabel + tDict[groupLabels[ind]][r] + ' '
            recursiveFill(ind+1)
        currentLabel[0] = tempLabel
    recursiveFill(0)
    def makeTitile(dic, measure):
        mydic = translateDict(dic)
        return '%s %s %s' % (mydic['workload'], mydic['rec'], measure)
    def decideFileName(dic, measure):
        name = figPrefix + '%s_Lr%s_%s_vs_%s.pdf' % (dic['workload'], dic['rec'], labelDict[xLabel], measure) # , subprocess.check_output(["git", "describe"]).strip().decode('ascii').replace('.','-'))
        return name.replace('_', '-').replace(r' ($\rho$)', '').replace(' ','-')
    # plt.title(makeTitile(kwargs, measure))
    ax.set_ylim(bottom=0)
    ncol = groupCounter[0] # int(math.ceil(groupCounter[0] / 2.0))
    propsize = 18
    if groupCounter[0] == 1:
        plt.subplots_adjust(left=.13, bottom=.15, right=0.98, top=0.97)
        # plt.legend(loc='upper center', bbox_to_anchor=(0.45, 1.15), ncol=1, markerscale=0.7, prop={'size': propsize}) # ['log 1', 'log 1 prime', 'log 2 prime', 'log 4 prime', 'log 2', 'log 4'])
    else:
        if ncol > 2:
            propsize = 18
        leftPos = .13
        if measure == 'locktable_avg_volume':
            leftPos = .16
        plt.subplots_adjust(left=leftPos, bottom=.15, right=0.98, top=0.75)
        # plt.legend(loc='upper center', bbox_to_anchor=(0.45, 1.41), ncol=ncol, markerscale=0.7, prop={'size': propsize}) # ['log 1', 'log 1 prime', 'log 2 prime', 'log 4 prime', 'log 2', 'log 4'])
        # plt.legend(loc='upper center', bbox_to_anchor=(0.45, 1.41), ncol=ncol, markerscale=0.7, prop={'size': propsize}) # ['log 1', 'log 1 prime', 'log 2 prime', 'log 4 prime', 'log 2', 'log 4'])
    filename = FIG_DIR + decideFileName(kwargs, measure)
    pdffig = PdfPages(filename)

    plt.savefig(pdffig, format='pdf')
    metadata = pdffig.infodict()
    metadata['Title'] = filename.replace('.pdf', subprocess.check_output(["git", "describe"]).strip().decode('ascii').replace('.','-'))
    metadata['Author'] = dataDir
    pdffig.close()
    handles, labels = ax.get_legend_handles_labels()
    labels, handles = zip(*sorted(zip(labels, handles), key=lambda t: t[0])) # deterministic order
    # plt.clf()
    legendfig = plt.figure(figsize=(ncol * 3, 1))
    plt.figlegend(handles, labels, ncol=ncol, prop={'size': 15})
    legendfig.savefig(filename.replace('.pdf', '_legend.pdf'))
    auxDataOutput(finalRes)

    print('Finished', filename)

def drawBar(dataFiles, dataDir, measure, xLabel, groupLabel, completeFunc, normalized=False, groupHidden=[], figPrefix='', auxDataOutput=dummy, **kwargs):
    kwargs = stringify(kwargs)
    def genFilterFunc(dic):
        def filterFunc(t):
            matched = nameRE.match(t).groupdict()
            for k,v in dic.items():
                if not k in matched or matched[k] is None or matched[k] != v:
                    return False
            return True
        return filterFunc
    corFiles = list(filter(genFilterFunc(kwargs), dataFiles))
    if len(corFiles) == 0:
        return # no data is available
    xRange = getRange(corFiles, xLabel)
    measures = measure.split('.')
    groupLabels = groupLabel.split('.')
    groupRanges = [getRange(corFiles, gl) for gl in groupLabels]

    trialRange = getRange(corFiles, 'trial')
    usefulParamers = {k:v for k, v in nameRE.match(corFiles[0]).groupdict().items() if v != None}.keys()
    print(usefulParamers)
    numParameter = len(usefulParamers)
    f = plt.figure()
    f, ax = plt.subplots(1)
    ax.yaxis.grid(True)
    plt.xlabel(labelDict[xLabel])
    plt.ylabel(labelDict[measure.replace(':','')])
    currentDict = [copy.deepcopy(kwargs)]
    currentLabel = ['']
    barWidth = 1.
    barSpace = 4
    groupSpace = 1.5
    groupCounter = [0]
    finalRes = {}
    def recursiveFill(ind):
        if ind == len(groupLabels):
            bottomPos = np.zeros(len(xRange))
            for measure in measures:
                xyList = []
                print(xRange)
                for x in sorted(xRange):
                    currentDict[0][xLabel] = x
                    # print(currentDict[0])
                    currentDict[0] = completeFunc(currentDict[0])
                    print(numParameter, currentDict[0].keys())
                    assert(numParameter == len(currentDict[0].keys()) + 1)
                    currentList = list(filter(genFilterFunc(currentDict[0]), corFiles))
                    print(currentDict[0], currentList)
                    numTrials = len(currentList)
                    if numTrials < len(trialRange):
                        continue
                    # print(currentList)
                    assert(numTrials == len(trialRange))
                    numList = []
                    for realFile in currentList:
                        numList.append(fileContentRes(realFile, measure, dataDir))
                    yValue = sum(numList) / float(numTrials)
                    if measure == 'Throughput':
                        yValue = yValue / 1e6
                    xyList.append((x, yValue))
                    # print(x, yValue)
                print('plotted ' + currentLabel[0])
                if len(xyList) == 0:
                    return
                print(currentDict[0])
                plotStyle = chooseBarStyle(currentDict[0], measure)
                plotStyle['label'] = currentLabel[0][:-1]
                xList, yList = zip(*xyList) # *sorted(xyList, key=lambda xy: float(xy[0])))
                # plt.xticks(xList)
                xPos = np.array(range(len(xList))) * (barWidth + barSpace) + barSpace / 4 + groupSpace * groupCounter[0]
                print(yList)
                finalRes[currentLabel[0][:-1]] = (xList, yList)
                # print(bottomPos)
                # print(xPos.tolist(), list(yList), barWidth, bottomPos.tolist())
                plt.bar(xPos, yList, barWidth, bottomPos, **plotStyle)
                bottomPos = np.add(bottomPos, np.array(yList)) # element wise add
            groupCounter[0] += 1
            return
            # the graph is well-defined, #kwargs + x + group + num_trial
        tempLabel = copy.deepcopy(currentLabel[0])
        for r in groupRanges[ind]:
            if r in groupHidden:
                continue
            currentDict[0][groupLabels[ind]] = r
            currentLabel[0] = tempLabel + tDict[groupLabels[ind]][r] + ' '
            recursiveFill(ind+1)
        currentLabel[0] = tempLabel
    recursiveFill(0)
    xTickPos = np.array(range(len(xRange))) * (barWidth + barSpace) + barSpace / 4
    plt.xticks(xTickPos, sorted(xRange))
    plt.subplots_adjust(left=0.13, bottom=.15, right=0.98, top=0.90)
    def makeTitile(dic, measure):
        mydic = translateDict(dic)
        return '%s %s %s' % (mydic['workload'], mydic['rec'], measure)
    def decideFileName(dic, measure):
        name = figPrefix + '%s_Lr%s_%s_vs_%s.pdf' % (dic['workload'], dic['rec'], labelDict[xLabel], measure) # , subprocess.check_output(["git", "describe"]).strip().decode('ascii').replace('.','-'))
        return name.replace('_', '-').replace(' ','-')
    # plt.title(makeTitile(kwargs, measure))
    ax.set_ylim(bottom=0)
    ax.set_xlim(left=0)
    # plt.legend(loc='upper left') # ['log 1', 'log 1 prime', 'log 2 prime', 'log 4 prime', 'log 2', 'log 4'])
    filename = FIG_DIR + decideFileName(kwargs, measure)
    pdffig = PdfPages(filename)
    plt.savefig(pdffig, format='pdf')
    metadata = pdffig.infodict()
    metadata['Title'] = filename.replace('.pdf', subprocess.check_output(["git", "describe"]).strip().decode('ascii').replace('.','-'))
    metadata['Author'] = dataDir
    pdffig.close()
    handles, labels = ax.get_legend_handles_labels()
    labels, handles = zip(*sorted(zip(labels, handles), key=lambda t: t[0])) # deterministic order
    # plt.clf()
    ncol = groupCounter[0]
    legendfig = plt.figure(figsize=(ncol * 4, 1))
    plt.figlegend(handles, labels, ncol=ncol, prop={'size': 15})
    legendfig.savefig(filename.replace('.pdf', '_legend.pdf'))
    auxDataOutput(finalRes)

# modification = 'recover_'
modification = 'short'
# modification = 'short_no_numa'


def getResDir(modification):
    label = subprocess.check_output(["git", "describe"]).strip()
    res = './results/' + modification + label.decode('ascii')
    return res

#resDir = getResDir(modification)

#resFiles = fetchData(resDir)
#=====================
# main result
#=====================
def completeLn(d):
    if d['log_alg'] == 'S': #  or d['log_alg'] == 'N':
        d['log_num'] = '1'
    else:
        d['log_num'] = '4'
    return d

# main result
'''
for workload in ['YCSB', 'TPCC']:
    for Lr in ['0', '1']:
        drawFig(resFiles, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)
        # drawFig(resFiles, 'MaxThr', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)
'''
import sys

def scalabilityAnalysis(finalRes, name):
    print("%s Scalability: %f (at %s thread) / %f (at %s thread) = %f" % (
        name, finalRes[name][1][-1], finalRes[name][0][-1], finalRes[name][1][0], finalRes[name][0][0], finalRes[name][1][-1] / finalRes[name][1][0]))

def compareAnalysis(finalRes, name1, name2, ind=-1):
    print("%s vs %s: %f / %f (at %s thread) = %f\n\t\tmax %f / %f = %f" % (
        name1, name2, finalRes[name1][1][ind], finalRes[name2][1][ind], finalRes[name1][0][ind], finalRes[name1][1][ind] / finalRes[name2][1][ind], 
        max(finalRes[name1][1]), max(finalRes[name2][1]), max(finalRes[name1][1]) / max(finalRes[name2][1])))

def auxMain(finalRes):
    print("--- Main Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command 2PL')
    scalabilityAnalysis(finalRes, 'Taurus Data 2PL')
    compareAnalysis(finalRes, 'Taurus Command 2PL', 'Serial Command 2PL')
    compareAnalysis(finalRes, 'Taurus Data 2PL', 'Serial Data 2PL')
    compareAnalysis(finalRes, 'Taurus Command 2PL', 'Taurus Data 2PL')
    print("--- Main Result Analysis ---")
    return

def main():
    shortDir = getResDir('short')
    shortRes = fetchData(shortDir)
    for workload in ['YCSB']:
        for Lr in ['0', '1']:
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, ["SILO"], '', auxMain, workload=workload, rec=Lr)
            # drawFig(resFiles, 'MaxThr', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)
    
    for workload in ['TPCC']:
        for Lr in ['0', '1']:
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, ["SILO"], 'Tm1_', auxMain, workload=workload, rec=Lr, Tm=1.0)
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, ["SILO"], 'Tm0_', auxMain, workload=workload, rec=Lr, Tm=0.0)
            # drawFig(resFiles, 'MaxThr', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)

def auxSilo(finalRes):
    finalRes = defaultdict(lambda :[[0],[1]], finalRes)
    print("--- Silo Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command Silo')
    scalabilityAnalysis(finalRes, 'Taurus Data Silo')
    scalabilityAnalysis(finalRes, 'No Logging Data Silo')
    compareAnalysis(finalRes, 'Taurus Command Silo', 'SiloR Data Silo')
    compareAnalysis(finalRes, 'Taurus Data Silo', 'SiloR Data Silo')
    compareAnalysis(finalRes, 'Taurus Data Silo', 'SiloR Data Silo', 2)
    compareAnalysis(finalRes, 'Taurus Command Silo', 'Taurus Data Silo')
    print("--- Silo Result Analysis ---")
    return

def mainSilo():
    shortDir = getResDir('shortSilo')
    shortRes = fetchData(shortDir)
    for workload in ['YCSB']:
        for Lr in ['0', '1']:
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, [], 'Silo_', auxSilo, workload=workload, rec=Lr)
            # drawFig(resFiles, 'MaxThr', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)
    for workload in ['TPCC']:
        for Lr in ['0', '1']:
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, ['S'], 'Silo_Tm1_', auxSilo, workload=workload, rec=Lr, Tm=1.0)
            drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, ['S'], 'Silo_Tm0_', auxSilo, workload=workload, rec=Lr, Tm=0.0)
            # drawFig(resFiles, 'MaxThr', 'thd_num', 'log_alg.log_type.cc_alg', completeLn, workload=workload, rec=Lr)

def auxContention(finalRes):
    print("--- Silo Result Analysis ---") 
    # print(finalRes.keys())
    print("At z=%s, No Logging vs Taurus Command %s / %s = %s, %s." % (
        finalRes['No Logging Data 2PL'][0][3], finalRes['No Logging Data 2PL'][1][3], 
        finalRes['Taurus Command 2PL'][1][3], finalRes['No Logging Data 2PL'][1][3] / finalRes['Taurus Command 2PL'][1][3], 1- finalRes['Taurus Command 2PL'][1][3] / finalRes['No Logging Data 2PL'][1][3]))
    print("At z=%s, No Logging vs Taurus Command %s / %s = %s, %s" % (
        finalRes['No Logging Data 2PL'][0][7], finalRes['No Logging Data 2PL'][1][7], 
        finalRes['Taurus Command 2PL'][1][7], finalRes['No Logging Data 2PL'][1][7] / finalRes['Taurus Command 2PL'][1][7], 1 - finalRes['Taurus Command 2PL'][1][7] / finalRes['No Logging Data 2PL'][1][7]))
    
    print("--- Silo Result Analysis ---")
    return

def sensitivity():
    # sensitivity
    senseDir = getResDir('sensitivity')
    senseFiles = fetchData(senseDir)
    # drawFig(senseFiles, senseDir, 'Throughput', 'txn_num', 'log_alg.log_type.cc_alg', completeLn, workload='TPCC', rec='0', thd_num=48)
    # return 
    # drawFig(senseFiles, senseDir, 'Throughput', 'num_wh', 'log_alg.log_type.cc_alg', completeLn, workload='TPCC', rec=1, thd_num=48)
    for Lr in ['0', '1']:
        drawFig(senseFiles, senseDir, 'Throughput', 'zipf_theta', 'log_alg.log_type.cc_alg', completeLn, ['SILO'], 'contention', auxContention, workload='YCSB', rec=Lr, thd_num=28)
        drawFig(senseFiles, senseDir, 'Throughput', 'num_wh', 'log_alg.log_type.cc_alg', completeLn, ['SILO'], 'contentionTm1_', auxContention, workload='TPCC', rec=Lr, thd_num=28, Tm=1.0)
        drawFig(senseFiles, senseDir, 'Throughput', 'num_wh', 'log_alg.log_type.cc_alg', completeLn, ['SILO'], 'contentionTm0_', auxContention, workload='TPCC', rec=Lr, thd_num=28, Tm=0.0)

def auxCompress1(finalRes):
    print("--- Silo Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command Silo')
    print("--- Silo Result Analysis ---")
    return

def auxCompress2(finalRes):
    print("--- Silo Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command Silo')
    print("--- Silo Result Analysis ---")
    return

def auxCompress3(finalRes):
    print("--- Silo Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command Silo')
    print("--- Silo Result Analysis ---")
    return

def sensitivityCompression():
    cDir = getResDir('compress')
    cFiles = fetchData(cDir)
    for Lr in ['0', '1']:
        for workload in ['YCSB']: # , 'TPCC']:
            #drawFig(cFiles, cDir, 'Throughput', 'Tp', 'log_alg.log_type.cc_alg', completeLn, [], 'compress', workload=workload, rec=Lr, thd_num=48)
            #drawFig(cFiles, cDir, 'Throughput', 'Tl', 'log_alg.log_type.cc_alg', completeLn, [], 'compress', workload=workload, rec=Lr, thd_num=48)
            drawFig(cFiles, cDir, 'int_aux_bytes', 'Tp', 'log_alg.log_type.cc_alg', completeLn, [], 'compress', auxCompress1, workload=workload, rec=Lr, thd_num=16) # 4)
    for workload in ['YCSB']: #, 'TPCC']:
        drawFig(cFiles, cDir, 'locktable_avg_volume', 'Tl', 'log_alg.log_type.cc_alg', completeLn, [], 'compress', auxCompress2, workload=workload, rec='0', thd_num=4)
        drawFig(cFiles, cDir, 'Throughput', 'Tl', 'log_alg.log_type.cc_alg', completeLn, [], 'compress', auxCompress3, workload=workload, rec='1', thd_num=28)

def completeNone(d):
    return d

def auxLogNum(finalRes):
    print("--- Silo Result Analysis ---")
    scalabilityAnalysis(finalRes, 'Taurus Command Silo')
    print("--- Silo Result Analysis ---")
    return

def lognum():
    shortDir = getResDir('lognum')
    # shortDir = "./results/lognumv1.5-32-g067aef6"
    shortRes = fetchData(shortDir)
    for Lr in ['0', '1']:
        drawBar(shortRes, shortDir, 'Throughput', 'log_num', 'log_alg.log_type.cc_alg', completeNone, False, [], 'lognum_', auxLogNum, workload='YCSB', rec=Lr, thd_num=24)
        # drawFig(shortRes, shortDir, 'Throughput', 'thd_num', 'log_num.log_alg.log_type.cc_alg', completeNone, ["NOWAIT"], 'lognum_', workload='YCSB', rec=Lr)
        
commandLineDir = ''
if __name__ == '__main__':

    if len(sys.argv) > 1:
        if len(sys.argv) > 2:
            commandLineDir = sys.argv[2]
        eval(sys.argv[1] + '()', globals(), locals())
        sys.exit(0)
    # generate all
    sensitivity()

# time breakdown
# drawBar(senseFiles, senseDir, 'time_io', 'log_alg', '', completeLn, workload='YCSB', rec='0', log_type='D', cc_alg='SILO')
# drawBar(senseFiles, senseDir, 'time_io', 'log_alg', '', completeLn, workload='YCSB', rec='1', log_type='D', cc_alg='SILO')

#for workload in ['YCSB', 'TPCC']:
#    for log_type in ['D', 'S']:
#        drawFig(resFiles, workload, log_type, 'Throughput')
