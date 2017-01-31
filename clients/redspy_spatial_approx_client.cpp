// @COPYRIGHT@
// Licensed under MIT license.
// See LICENSE.TXT file in the project root for more information.
// ==============================================================

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <list>
#include <sys/mman.h>
#include <sstream>
#include <functional>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "pin.H"
extern "C" {
#include "xed-interface.h"
#include "xed-common-hdrs.h"
}

//enable Data-centric
#define USE_TREE_BASED_FOR_DATA_CENTRIC
#define USE_TREE_WITH_ADDR
#include "cctlib.H"
using namespace std;
using namespace PinCCTLib;

/* Other footprint_client settings */
#define MAX_REDUNDANT_CONTEXTS_TO_LOG (1000)
#define THREAD_MAX (1024)


#define MAX_WRITE_OP_LENGTH (512)
#define MAX_WRITE_OPS_IN_INS (8)

#ifdef ENABLE_SAMPLING

#define WINDOW_ENABLE 1000000
#define WINDOW_DISABLE 1000000000

#endif

#define SAME_RATE (0.1)
#define SAME_RECORD_LIMIT (0)
#define RED_RATE (0.9)

#define delta 0.01

#define MAKE_CONTEXT_PAIR(a, b) (((uint64_t)(a) << 32) | ((uint64_t)(b)))

typedef struct indexRange{
    uint32_t start;
    uint32_t end;
}IndexRange;

typedef struct dataObjectStatus{
    uint32_t numOfReads; //num of reads
    uint8_t secondWrite;
    uint64_t startAddr;
    uint32_t lastWCtxt;
    uint8_t accessLen;
    uint32_t size;
}DataObjectStatus;

typedef struct intraRedIndexPair{
    double redundancy;
    uint32_t curCtxt;
    uint32_t len;
    list<IndexRange> indexes;
    list<uint32_t> spatialRedInd;
}IntraRedIndexPair;

struct RedSpyThreadData{
    unordered_map<uint32_t,DataObjectStatus> dynamicDataObjects;
    unordered_map<uint32_t,DataObjectStatus> staticDataObjects;
    long NUM_INS;
    bool Sample_flag;
};

//helper struct used to 

// key for accessing TLS storage in the threads. initialized once in main()
static  TLS_KEY client_tls_key;

// function to access thread-specific data
inline RedSpyThreadData* ClientGetTLS(const THREADID threadId) {
    RedSpyThreadData* tdata =
    static_cast<RedSpyThreadData*>(PIN_GetThreadData(client_tls_key, threadId));
    return tdata;
}


static INT32 Usage() {
    PIN_ERROR("Pin tool to gather calling context on each load and store.\n" + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

// Main for RedSpy, initialize the tool, register instrumentation functions and call the target program.
static FILE* gTraceFile;
uint32_t lastStatic;
// Initialized the needed data structures before launching the target program
static void ClientInit(int argc, char* argv[]) {
    // Create output file
    char name[MAX_FILE_PATH] = "redspy_spatial_approx.out.";
    char* envPath = getenv("CCTLIB_CLIENT_OUTPUT_FILE");
    
    if(envPath) {
        // assumes max of MAX_FILE_PATH
        strcpy(name, envPath);
    }
    
    gethostname(name + strlen(name), MAX_FILE_PATH - strlen(name));
    pid_t pid = getpid();
    sprintf(name + strlen(name), "%d", pid);
    cerr << "\n Creating log file at:" << name << "\n";
    gTraceFile = fopen(name, "w");
    // print the arguments passed
    fprintf(gTraceFile, "\n");
    
    for(int i = 0 ; i < argc; i++) {
        fprintf(gTraceFile, "%s ", argv[i]);
    }
    
    fprintf(gTraceFile, "\n");
}

static unordered_map<uint64_t, list<IntraRedIndexPair>> dyIntraDataRed[THREAD_MAX];
static unordered_map<uint64_t, list<IntraRedIndexPair>> stIntraDataRed[THREAD_MAX];

int inline FindRedPair(list<IntraRedIndexPair> redlist,IntraRedIndexPair redpair){
    list<IntraRedIndexPair>::iterator it;
    for(it = redlist.begin();it != redlist.end(); ++it){
        if((*it).redundancy == redpair.redundancy && (*it).curCtxt == redpair.curCtxt)
            return 1;
    }
    return 0;
}

#ifdef ENABLE_SAMPLING

static ADDRINT IfEnableSample(THREADID threadId){
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    if(tData->Sample_flag){
        return 1;
    }
    return 0;
}

static inline VOID EmptyCtxt(RedSpyThreadData* tData){
    
    unordered_map<uint32_t,DataObjectStatus>::iterator it;
    
    for( it = tData->dynamicDataObjects.begin(); it != tData->dynamicDataObjects.end(); ++it){
        it->second.numOfReads = 0;
    }
    for( it = tData->staticDataObjects.begin(); it != tData->staticDataObjects.end(); ++it){
        it->second.numOfReads = 0;
    }
}

#endif

//type:0 means dynamic data object while 1 means static
VOID inline RecordIntraArrayRedundancy(uint32_t dataObj,uint32_t lastW, IntraRedIndexPair redPair,THREADID threadId,uint8_t type){
    uint64_t data = (uint64_t)dataObj;
    uint64_t context = (uint64_t)lastW;
    uint64_t key = (data << 32) | context;
  
    if(type == 0){
        unordered_map<uint64_t,list<IntraRedIndexPair>>::iterator it;
        it = dyIntraDataRed[threadId].find(key);
        if(it == dyIntraDataRed[threadId].end()){
            list<IntraRedIndexPair> newlist;
            newlist.push_back(redPair);
            dyIntraDataRed[threadId].insert(std::pair<uint64_t,list<IntraRedIndexPair>>(key,newlist));
        }else{
            if(!FindRedPair(it->second,redPair))
                it->second.push_back(redPair);
        }
    }else{
        unordered_map<uint64_t,list<IntraRedIndexPair>>::iterator it;
        it = stIntraDataRed[threadId].find(key);
        if(it == stIntraDataRed[threadId].end()){
            list<IntraRedIndexPair> newlist;
            newlist.push_back(redPair);
            stIntraDataRed[threadId].insert(std::pair<uint64_t,list<IntraRedIndexPair>>(key,newlist));
        }else{
            if(!FindRedPair(it->second,redPair))
                it->second.push_back(redPair);
        }
    }
}

/* update the reading access pattern */
VOID UpdateReadAccess(void *addr, THREADID threadId, const uint32_t opHandle){

        RedSpyThreadData* const tData = ClientGetTLS(threadId);
 
        DataHandle_t dataHandle = GetDataObjectHandle(addr,threadId);
        if(dataHandle.objectType == DYNAMIC_OBJECT){
            unordered_map<uint32_t,DataObjectStatus>::iterator it;
            it = tData->dynamicDataObjects.find(dataHandle.pathHandle);
            if(it != tData->dynamicDataObjects.end()){
                it->second.numOfReads += 1;
            }
        }else if(dataHandle.objectType == STATIC_OBJECT){
            unordered_map<uint32_t,DataObjectStatus>::iterator it;
            it = tData->staticDataObjects.find(dataHandle.symName);
            if(it != tData->staticDataObjects.end()){
                it->second.numOfReads += 1;
            }
        }
}

inline VOID CheckAndRecordIntraArrayRedundancy(uint32_t nameORpath, uint32_t lastWctxt, uint32_t curCtxt, uint16_t accessLen, uint64_t begaddr, uint64_t endaddr,THREADID threadId, uint8_t type ){
    
        uint64_t address;
        uint32_t index;
        if(accessLen >= 4){
            unordered_map<double,IndexRange> valuesMap1;
            unordered_map<double,IndexRange>::iterator it1;
            list<uint32_t> spatialRedIndex;
            address = begaddr;
            index = 0;
            double valueLast = *static_cast<double *>((void *)address);
            address += accessLen;
            double value = valueLast;
            IndexRange newIndPair;
            newIndPair.start=index;
            newIndPair.end=index;
            valuesMap1.insert(std::pair<double,IndexRange>(value,newIndPair));
            index++;
            while(address < endaddr){
            
                double value1 = *static_cast<double *>((void *)address);
                double diffR = (value1-valueLast)/valueLast;
                if(diffR < delta && diffR > -delta)
                    spatialRedIndex.push_back(index);
                diffR = (value1-value)/value;
                if(diffR < delta && diffR > -delta){
                    it1 = valuesMap1.find(value);
                    if(it1 == valuesMap1.end()){
                       IndexRange nlist;
                       nlist.start=index;
                       nlist.end=index;
                       valuesMap1.insert(std::pair<double,IndexRange>(value,nlist));
                    }else{
                       it1->second.end=index;
                    }
                }else
                    value = value1;
                address += accessLen;
                index++;
                valueLast = value1;
            }            
            uint32_t numUniqueValue = valuesMap1.size();
            double redRate = (double)(index - numUniqueValue)/index;
            list<IndexRange> maxList;
            for (it1 = valuesMap1.begin(); it1 != valuesMap1.end(); ++it1){
                if((it1->second.end-it1->second.start) > index*SAME_RATE){
                    maxList.push_back(it1->second);
                }
            }
            if(redRate > RED_RATE || maxList.size() > SAME_RECORD_LIMIT){
                IntraRedIndexPair newpair;
                newpair.redundancy = redRate;
                newpair.curCtxt = curCtxt;
                newpair.len = index;
                newpair.indexes = maxList;                            
                newpair.spatialRedInd = spatialRedIndex;
                RecordIntraArrayRedundancy(nameORpath, lastWctxt, newpair,threadId,type);
            }
        }else{
            ;//printf("\nHaven't thought about how to handle this case\n"); 
        }
}

//check whether there are same elements inside the data objects
VOID CheckIntraArrayElements(void *addr, uint16_t AccessLen, THREADID threadId, const uint32_t opHandle){

    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    DataHandle_t dataHandle = GetDataObjectHandle(addr,threadId);
    uint32_t curCtxt = GetContextHandle(threadId, opHandle);

    if(dataHandle.objectType == DYNAMIC_OBJECT){
        uint32_t arraySize = (dataHandle.end_addr - dataHandle.beg_addr)/AccessLen; 
        if(arraySize <= 1)
            return;
        unordered_map<uint32_t,DataObjectStatus>::iterator it;
        it = tData->dynamicDataObjects.find(dataHandle.pathHandle);
        if(it != tData->dynamicDataObjects.end()){
            if(it->second.numOfReads > arraySize/4){
                CheckAndRecordIntraArrayRedundancy(dataHandle.pathHandle, it->second.lastWCtxt, curCtxt, AccessLen, dataHandle.beg_addr, dataHandle.end_addr, threadId, 0);
            }
            if(it->second.numOfReads != 0)
                it->second.secondWrite+=1;
            it->second.numOfReads = 0;
            it->second.lastWCtxt = curCtxt;
        }else{
            DataObjectStatus newStatus;
            newStatus.numOfReads = 0;
            newStatus.secondWrite = 0;
            newStatus.startAddr = dataHandle.beg_addr;
            newStatus.accessLen = AccessLen;
            newStatus.lastWCtxt = curCtxt;
            tData->dynamicDataObjects.insert(std::pair<uint32_t,DataObjectStatus>(dataHandle.pathHandle,newStatus)); 
        }
    }else if(dataHandle.objectType == STATIC_OBJECT){
        uint32_t arraySize = (dataHandle.end_addr - dataHandle.beg_addr)/AccessLen;  
        if(arraySize <= 1)
            return;      
        unordered_map<uint32_t,DataObjectStatus>::iterator it;
        it = tData->staticDataObjects.find(dataHandle.symName);
        if(it != tData->staticDataObjects.end()){
            if(it->second.numOfReads > arraySize/4){
                CheckAndRecordIntraArrayRedundancy(dataHandle.symName, it->second.lastWCtxt, curCtxt, AccessLen, dataHandle.beg_addr, dataHandle.end_addr, threadId, 1);
            }
            if(it->second.numOfReads != 0)
                it->second.secondWrite+=1;
            it->second.numOfReads = 0;
            it->second.lastWCtxt = curCtxt;
        }else{
            DataObjectStatus newStatus;
            newStatus.numOfReads = 0;
            newStatus.secondWrite = 0;
            newStatus.startAddr = dataHandle.beg_addr;
            newStatus.accessLen = AccessLen;
            newStatus.lastWCtxt = curCtxt;
            tData->staticDataObjects.insert(std::pair<uint32_t,DataObjectStatus>(dataHandle.symName,newStatus)); 
        }
    }
}

static inline int GetNumWriteOperandsInIns(INS ins, UINT32 & whichOp){
    int numWriteOps = 0;
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for(UINT32 memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            numWriteOps++;
            whichOp = memOp;
        }
    }
    return numWriteOps;
}

static inline bool IsFloatInstruction(ADDRINT ip) {
    xed_decoded_inst_t  xedd;
    xed_state_t  xed_state;
    xed_decoded_inst_zero_set_mode(&xedd, &xed_state);
    
    if(XED_ERROR_NONE == xed_decode(&xedd, (const xed_uint8_t*)(ip), 15)) {
        unsigned int NumOperands = xed_decoded_inst_noperands(&xedd);
        for(unsigned int i = 0; i < NumOperands; ++i){
            xed_operand_element_type_enum_t TypeOperand = xed_decoded_inst_operand_element_type(&xedd,i);
            if(TypeOperand == XED_OPERAND_ELEMENT_TYPE_SINGLE || TypeOperand == XED_OPERAND_ELEMENT_TYPE_DOUBLE || TypeOperand == XED_OPERAND_ELEMENT_TYPE_FLOAT16 || TypeOperand == XED_OPERAND_ELEMENT_TYPE_LONGDOUBLE)
                return true;
        }
        return false;
    } else {
        assert(0 && "failed to disassemble instruction");
        return false;
    }
}

#ifdef ENABLE_SAMPLING

#define HANDLE_READ() \
INS_InsertIfPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END); \
INS_InsertThenPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) UpdateReadAccess, IARG_MEMORYOP_EA, memop, IARG_THREAD_ID, IARG_UINT32,opaqueHandle, IARG_END)

#define HANDLE_WRITE() \
INS_InsertIfPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)IfEnableSample, IARG_THREAD_ID,IARG_END); \
INS_InsertThenPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) CheckIntraArrayElements, IARG_MEMORYOP_EA, memop, IARG_UINT32, refSize, IARG_THREAD_ID, IARG_UINT32, opaqueHandle, IARG_END)

#else

#define HANDLE_READ() \
INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) UpdateReadAccess, IARG_MEMORYOP_EA, memop, IARG_THREAD_ID, IARG_UINT32,opaqueHandle, IARG_END)

#define HANDLE_WRITE() \
INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) CheckIntraArrayElements, IARG_MEMORYOP_EA, memop, IARG_UINT32, refSize, IARG_THREAD_ID, IARG_UINT32, opaqueHandle, IARG_END)

#endif

static VOID InstrumentInsCallback(INS ins, VOID* v, const uint32_t opaqueHandle) {
    if (!INS_IsMemoryRead(ins) && !INS_IsMemoryWrite(ins)) return;
   // if (INS_IsStackRead(ins) || INS_IsStackWrite(ins)) return;
    if (INS_IsBranchOrCall(ins) || INS_IsRet(ins)) return;
    
    if(!IsFloatInstruction(INS_Address(ins)))
        return;
    
    UINT32 memOperands = INS_MemoryOperandCount(ins);
   
    // Special case, if we have only one write operand
    UINT32 whichOp = 0;
    if(GetNumWriteOperandsInIns(ins, whichOp) == 1){
        // Read the value at location before and after the instruction
        for(UINT32 memop = 0; memop < memOperands; memop++){
           if(INS_MemoryOperandIsRead(ins,memop) && !INS_MemoryOperandIsWritten(ins,memop)){
               HANDLE_READ();
           }else if(INS_MemoryOperandIsWritten(ins,memop)){
               UINT32 refSize = INS_MemoryOperandSize(ins, memop);
               HANDLE_WRITE();
           }
        }
        return;
    }
    
    for(UINT32 memop = 0; memop < memOperands; memop++) {
        if(INS_MemoryOperandIsRead(ins,memop) && !INS_MemoryOperandIsWritten(ins,memop)){
            HANDLE_READ();
        }       
 
        if(!INS_MemoryOperandIsWritten(ins, memop))
            continue;
        
        UINT32 refSize = INS_MemoryOperandSize(ins, memop);
        HANDLE_WRITE();
    }
}

#ifdef ENABLE_SAMPLING

inline VOID InsInTrace(uint32_t count, THREADID threadId) {
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    if(tData->Sample_flag){
        tData->NUM_INS += count;
        if(tData->NUM_INS > WINDOW_ENABLE){
            tData->Sample_flag = false;
            tData->NUM_INS = 0;
            EmptyCtxt(tData);
        }
    }else{
        tData->NUM_INS += count;
        if(tData->NUM_INS > WINDOW_DISABLE){
            tData->Sample_flag = true;
            tData->NUM_INS = 0;
        }
    }
}

//instrument the trace, count the number of ins in the trace, decide to instrument or not
static void InstrumentTrace(TRACE trace, void* f) {
    uint32_t TotInsInTrace = 0;
    unordered_map<ADDRINT,BBL> headers;
    unordered_map<ADDRINT,BBL>::iterator headIter;
    unordered_map<ADDRINT,double> BBLweight;
    unordered_map<ADDRINT,double>::iterator weightIter;
    list<BBL> bblsToCheck;
    list<BBL> bblsChecked;
    
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        headers[INS_Address(BBL_InsHead(bbl))]=bbl;
    }
    
    BBL curbbl = TRACE_BblHead(trace);
    BBLweight[BBL_Address(curbbl)] = 1.0;
    bblsToCheck.push_back(curbbl);
    
    while (!bblsToCheck.empty()) {
        curbbl = bblsToCheck.front();
        double curweight = BBLweight[BBL_Address(curbbl)];
        INS curTail = BBL_InsTail(curbbl);
        if( INS_IsDirectBranchOrCall(curTail)){
            curweight /= 2;
            ADDRINT next = INS_DirectBranchOrCallTargetAddress(curTail);
            headIter = headers.find(next);
            if (headIter != headers.end()) {
                BBL bbl = headIter->second;
                ADDRINT bblAddr = BBL_Address(bbl);
                weightIter = BBLweight.find(bblAddr);
                if (weightIter == BBLweight.end()) {
                    BBLweight[bblAddr] = curweight;
                }else{
                    weightIter->second += curweight;
                }
                bool found = (std::find(bblsToCheck.begin(), bblsToCheck.end(), bbl) != bblsToCheck.end());
                bool foundChecked = (std::find(bblsChecked.begin(), bblsChecked.end(), bbl) != bblsChecked.end());
                if(!found && !foundChecked) bblsToCheck.push_back(bbl);
            }
            if( INS_HasFallThrough(curTail)){
                next = INS_Address(INS_Next(curTail));
                headIter = headers.find(next);
                if (headIter != headers.end()) {
                    BBL bbl = headIter->second;
                    ADDRINT bblAddr = BBL_Address(bbl);
                    weightIter = BBLweight.find(bblAddr);
                    if (weightIter == BBLweight.end()) {
                        BBLweight[bblAddr] = curweight;
                    }else{
                        weightIter->second += curweight;
                    }
                    bool found = (std::find(bblsToCheck.begin(), bblsToCheck.end(), bbl) != bblsToCheck.end());
                    bool foundChecked = (std::find(bblsChecked.begin(), bblsChecked.end(), bbl) != bblsChecked.end());
                    if(!found && !foundChecked) bblsToCheck.push_back(bbl);
                }
            }
        }
        bblsToCheck.pop_front();
        bblsChecked.push_back(curbbl);
    }
    
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        weightIter = BBLweight.find(BBL_Address(bbl));
        if (weightIter != BBLweight.end()) {
            TotInsInTrace += (uint32_t)(weightIter->second * BBL_NumIns(bbl));
        } else {
            TotInsInTrace += BBL_NumIns(bbl);
        }
    }
    
    if(TotInsInTrace)
        TRACE_InsertCall(trace,IPOINT_BEFORE, (AFUNPTR)InsInTrace, IARG_UINT32, TotInsInTrace, IARG_THREAD_ID, IARG_END);
}
#endif

struct RedundacyData {
    ContextHandle_t dead;
    ContextHandle_t kill;
    uint64_t frequency;
};

static inline string ConvertListToString(list<uint32_t> inlist){

    list<uint32_t>::iterator it = inlist.begin();
    uint32_t tmp = (*it);
    string indexlist = "[" + to_string(tmp) + ",";
    it++;
    while(it != inlist.end()){
        if(*it == tmp + 1){
            tmp = *it;
        }
        else{
            indexlist += to_string(tmp) + "],[" + to_string(*it)+ ",";
            tmp = *it;
        }
        it++;
    }
    indexlist += to_string(tmp) + "]";
    return indexlist;
}


static inline bool RedundacyCompare(const struct RedundacyData &first, const struct RedundacyData &second) {
    return first.frequency > second.frequency ? true : false;
}

static void PrintRedundancyPairs(THREADID threadId) {
    vector<RedundacyData> tmpList;
    vector<RedundacyData>::iterator tmpIt;

    uint64_t grandTotalRedundantBytes = 0;
    fprintf(gTraceFile,"\n*************** Intra Array Redundancy of Thread %d ***************\n",threadId);
    unordered_map<uint64_t,list<IntraRedIndexPair>>::iterator itIntra;
    uint8_t account = 0;
    fprintf(gTraceFile,"========== Static Dataobjecy Redundancy ==========\n");
    for(itIntra = stIntraDataRed[threadId].begin(); itIntra != stIntraDataRed[threadId].end(); ++itIntra){
        uint64_t keyhash = itIntra->first;
        uint32_t dataObj = keyhash >> 32;
        uint32_t contxt = keyhash & 0xffffffff;
        char *symName = GetStringFromStringPool(dataObj);
        fprintf(gTraceFile,"\nVariable %s at \n",symName);
        PrintFullCallingContext(contxt);
        list<IntraRedIndexPair>::iterator listit,listit2;
        for(listit = itIntra->second.begin(); listit != itIntra->second.end(); ++listit){
            for(listit2 = itIntra->second.begin();listit2 != listit;++listit2){
               if(IsSameSourceLine((*listit).curCtxt,(*listit2).curCtxt))
                  break;
            }
            if(listit2 == listit){
               fprintf(gTraceFile,"\nRed:%.2f, unique Indexes:",(*listit).redundancy);
               string indexlist = "[";
               list<IndexRange>::iterator IndIt=(*listit).indexes.begin();
               float frac = (float)((*IndIt).end-(*IndIt).start)*100/(*listit).len;
               indexlist += to_string((*IndIt).start) + "," + to_string((*IndIt).end) + "," +  to_string(frac)+ "%]";
               IndIt++;
               while(IndIt != (*listit).indexes.end()){
                  frac = (float)((*IndIt).end-(*IndIt).start)*100/(*listit).len;
                  indexlist += ",[" + to_string((*IndIt).start) + "," + to_string((*IndIt).end) + "," + to_string(frac) + "%]";
                  IndIt++;
               }
               fprintf(gTraceFile,"%s\n",indexlist.c_str());
               indexlist = ConvertListToString((*listit).spatialRedInd);
               fprintf(gTraceFile,"redundant spatial indexes:%s\n",indexlist.c_str());
               PrintFullCallingContext((*listit).curCtxt);
            }
        }
        fprintf(gTraceFile,"\n----------------------------");
        account++;
        if(account > MAX_REDUNDANT_CONTEXTS_TO_LOG)
            break;
    }
    account = 0;
    fprintf(gTraceFile,"########## Dynamic Dataobjecy Redundancy ##########\n");
    for(itIntra = dyIntraDataRed[threadId].begin(); itIntra != dyIntraDataRed[threadId].end(); ++itIntra){
        uint64_t keyhash = itIntra->first;
        uint32_t dataObj = keyhash >> 32;
        uint32_t contxt = keyhash & 0xffffffff;        
        fprintf(gTraceFile,"\ndynamic malloc:\n");
        PrintFullCallingContext(dataObj);
        fprintf(gTraceFile,"\n ~~ at ~~:\n");
        PrintFullCallingContext(contxt);
        list<IntraRedIndexPair>::iterator listit,listit2;
        
        for(listit = itIntra->second.begin(); listit != itIntra->second.end(); ++listit){
            for(listit2 = itIntra->second.begin();listit2 != listit;++listit2){
               if(IsSameSourceLine((*listit).curCtxt,(*listit2).curCtxt))
                  break;
            }
            if(listit2 == listit){
               fprintf(gTraceFile,"\nRed:%.2f, unique Indexes:",(*listit).redundancy);
               string indexlist = "[";
               list<IndexRange>::iterator IndIt=(*listit).indexes.begin();
               float frac = (float)((*IndIt).end-(*IndIt).start)*100/(*listit).len;
               indexlist += to_string((*IndIt).start) + "," + to_string((*IndIt).end) + "," +  to_string(frac)+ "%]";
               IndIt++;
               while(IndIt != (*listit).indexes.end()){
                  frac = (float)((*IndIt).end-(*IndIt).start)*100/(*listit).len;
                  indexlist += ",[" + to_string((*IndIt).start) + "," + to_string((*IndIt).end) + "," +  to_string(frac) + "%]";
                  IndIt++;
               }
               fprintf(gTraceFile,"%s\n",indexlist.c_str());
               indexlist = ConvertListToString((*listit).spatialRedInd);
               fprintf(gTraceFile,"redundant spatial indexes:%s\n",indexlist.c_str());               
               PrintFullCallingContext((*listit).curCtxt);
            }
        }
        fprintf(gTraceFile,"\n----------------------------");
        account++;
        if(account > MAX_REDUNDANT_CONTEXTS_TO_LOG)
            break;
    }
}

// On each Unload of a loaded image, the accummulated redundancy information is dumped
static VOID ImageUnload(IMG img, VOID* v) {
    fprintf(gTraceFile, "\n TODO .. Multi-threading is not well supported.");    
    THREADID  threadid =  PIN_ThreadId();
    fprintf(gTraceFile, "\nUnloading %s", IMG_Name(img).c_str());
    // Update gTotalInstCount first
   /* if(dyIntraDataRed[threadid].empty())
       return;*/
    PIN_LockClient();
    PrintRedundancyPairs(threadid);
    PIN_UnlockClient();
    // clear redmap now
    dyIntraDataRed[threadid].clear();
    stIntraDataRed[threadid].clear();
}

static VOID ThreadFiniFunc(THREADID threadId, const CONTEXT *ctxt, INT32 code, VOID *v) {
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    unordered_map<uint32_t,DataObjectStatus>::iterator it;
    for( it = tData->staticDataObjects.begin(); it != tData->staticDataObjects.end();++it){
        if(it->second.secondWrite == 0){
            DataHandle_t dataHandle = GetDataObjectHandle((void*)(it->second.startAddr),threadId); 
            CheckAndRecordIntraArrayRedundancy(it->first, it->second.lastWCtxt, it->second.lastWCtxt, it->second.accessLen, dataHandle.beg_addr, dataHandle.end_addr, threadId, 1);           
        }
    }
    for( it = tData->dynamicDataObjects.begin(); it != tData->dynamicDataObjects.end();++it){
        if(it->second.secondWrite == 0){
            DataHandle_t dataHandle = GetDataObjectHandle((void*)(it->second.startAddr),threadId); 
            if((dataHandle.end_addr - dataHandle.beg_addr)/it->second.accessLen <= 1)
               continue;
            CheckAndRecordIntraArrayRedundancy(it->first, it->second.lastWCtxt, it->second.lastWCtxt, it->second.accessLen, dataHandle.beg_addr, dataHandle.end_addr, threadId, 0);           
        }
    }
}

static VOID FiniFunc(INT32 code, VOID *v) {
    // do whatever you want to the full CCT with footpirnt
}


static void InitThreadData(RedSpyThreadData* tdata){
    tdata->NUM_INS = 0;
    tdata->Sample_flag = true;
}

static VOID ThreadStart(THREADID threadid, CONTEXT* ctxt, INT32 flags, VOID* v) {
    RedSpyThreadData* tdata = new RedSpyThreadData();
    InitThreadData(tdata);
    //    __sync_fetch_and_add(&gClientNumThreads, 1);
    PIN_SetThreadData(client_tls_key, tdata, threadid);
}


int main(int argc, char* argv[]) {
    // Initialize PIN
    if(PIN_Init(argc, argv))
        return Usage();
    
    // Initialize Symbols, we need them to report functions and lines
    PIN_InitSymbols();
    
    // Init Client
    ClientInit(argc, argv);
    // Intialize CCTLib
    PinCCTLibInit(INTERESTING_INS_MEMORY_ACCESS, gTraceFile, InstrumentInsCallback, 0, true);
    
    
    // Obtain  a key for TLS storage.
    client_tls_key = PIN_CreateThreadDataKey(0 /*TODO have a destructir*/);
    // Register ThreadStart to be called when a thread starts.
    PIN_AddThreadStartFunction(ThreadStart, 0);
    
    
    // fini function for post-mortem analysis
    PIN_AddThreadFiniFunction(ThreadFiniFunc, 0);
    PIN_AddFiniFunction(FiniFunc, 0);
    
#ifdef ENABLE_SAMPLING
    TRACE_AddInstrumentFunction(InstrumentTrace, 0);
#endif
    
    // Register ImageUnload to be called when an image is unloaded
    IMG_AddUnloadFunction(ImageUnload, 0);
    
    // Launch program now
    PIN_StartProgram();
    return 0;
}


