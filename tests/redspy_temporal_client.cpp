// * BeginRiceCopyright *****************************************************
//
// Copyright ((c)) 2002-2014, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sstream>
#include <functional>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "pin.H"
#include "cctlib.H"
using namespace std;
using namespace PinCCTLib;

/* infrastructure for shadow memory */
/* MACROs */
// 64KB shadow pages
#define PAGE_OFFSET_BITS (16LL)
#define PAGE_OFFSET(addr) ( addr & 0xFFFF)
#define PAGE_OFFSET_MASK ( 0xFFFF)

#define PAGE_SIZE (1 << PAGE_OFFSET_BITS)

// 2 level page table
#define PTR_SIZE (sizeof(struct Status *))
#define LEVEL_1_PAGE_TABLE_BITS  (20)
#define LEVEL_1_PAGE_TABLE_ENTRIES  (1 << LEVEL_1_PAGE_TABLE_BITS )
#define LEVEL_1_PAGE_TABLE_SIZE  (LEVEL_1_PAGE_TABLE_ENTRIES * PTR_SIZE )

#define LEVEL_2_PAGE_TABLE_BITS  (12)
#define LEVEL_2_PAGE_TABLE_ENTRIES  (1 << LEVEL_2_PAGE_TABLE_BITS )
#define LEVEL_2_PAGE_TABLE_SIZE  (LEVEL_2_PAGE_TABLE_ENTRIES * PTR_SIZE )

#define LEVEL_1_PAGE_TABLE_SLOT(addr) (((addr) >> (LEVEL_2_PAGE_TABLE_BITS + PAGE_OFFSET_BITS)) & 0xfffff)
#define LEVEL_2_PAGE_TABLE_SLOT(addr) (((addr) >> (PAGE_OFFSET_BITS)) & 0xFFF)


// have R, W representative macros
#define READ_ACTION (0)
#define WRITE_ACTION (0xff)

#define ONE_BYTE_READ_ACTION (0)
#define TWO_BYTE_READ_ACTION (0)
#define FOUR_BYTE_READ_ACTION (0)
#define EIGHT_BYTE_READ_ACTION (0)

#define ONE_BYTE_WRITE_ACTION (0xff)
#define TWO_BYTE_WRITE_ACTION (0xffff)
#define FOUR_BYTE_WRITE_ACTION (0xffffffff)
#define EIGHT_BYTE_WRITE_ACTION (0xffffffffffffffff)



#define IS_ACCESS_WITHIN_PAGE_BOUNDARY(accessAddr, accessLen)  (PAGE_OFFSET((accessAddr)) <= (PAGE_OFFSET_MASK - (accessLen)))

/* Other footprint_client settings */
#define MAX_REDUNDANT_CONTEXTS_TO_LOG (1000)
#define THREAD_MAX (1024)

#define ENCODE_ADDRESS_AND_ACCESS_LEN(addr, len) ( (addr) | (((uint64_t)(len)) << 48))
#define DECODE_ADDRESS(addrAndLen) ( (addrAndLen) & ((1L<<48) - 1))
#define DECODE_ACCESS_LEN(addrAndLen) ( (addrAndLen) >> 48)


#define MAX_WRITE_OP_LENGTH (512)
#define MAX_WRITE_OPS_IN_INS (8)

#define WINDOW_ENABLE 1000000
#define WINDOW_DISABLE 1000000000
#define WINDOW_CLEAN 10

#define DECODE_DEAD(data) static_cast<ContextHandle_t>(((data)  & 0xffffffffffffffff) >> 32 )
#define DECODE_KILL(data) (static_cast<ContextHandle_t>( (data)  & 0x00000000ffffffff))


#define MAKE_CONTEXT_PAIR(a, b) (((uint64_t)(a) << 32) | ((uint64_t)(b)))

__thread long long NUM_INS = 0;
__thread bool Sample_flag = true;
__thread long long NUM_winds = 0;


struct AddrValPair{
    void * address;
    uint8_t value[MAX_WRITE_OP_LENGTH];
};

struct RedSpyThreadData{
    AddrValPair buffer[MAX_WRITE_OPS_IN_INS];
    uint32_t regCtxt[REG_LAST];
    UINT8 regValue[REG_LAST][MAX_WRITE_OP_LENGTH];
    uint64_t bytesWritten;
};

struct RegInfo{
    UINT8 count;
    REG regs[MAX_WRITE_OPS_IN_INS];
};

// key for accessing TLS storage in the threads. initialized once in main()
static  TLS_KEY client_tls_key;

// function to access thread-specific data
inline RedSpyThreadData* ClientGetTLS(const THREADID threadId) {
    RedSpyThreadData* tdata =
    static_cast<RedSpyThreadData*>(PIN_GetThreadData(client_tls_key, threadId));
    return tdata;
}


template<int start, int end, int incr>
struct UnrolledLoop{
    static __attribute__((always_inline)) void Body(function<void (const int)> func){
        func(start); // Real loop body
        UnrolledLoop<start+incr, end, incr>:: Body(func);   // unroll next iteration
    }
};

template<int end,  int incr>
struct UnrolledLoop<end , end , incr>{
    static __attribute__((always_inline)) void Body(function<void (const int)> func){
        // empty body
    }
};

template<int start, int end, int incr>
struct UnrolledConjunction{
    static __attribute__((always_inline)) bool Body(function<bool (const int)> func){
        return func(start) && UnrolledConjunction<start+incr, end, incr>:: Body(func);   // unroll next iteration
    }
};

template<int end,  int incr>
struct UnrolledConjunction<end , end , incr>{
    static __attribute__((always_inline)) bool Body(function<void (const int)> func){
        return true;
    }
};

INT32 Usage2() {
    PIN_ERROR("Pin tool to gather calling context on each load and store.\n" + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

// Main for RedSpy, initialize the tool, register instrumentation functions and call the target program.
static FILE* gTraceFile;
static uint8_t ** gL1PageTable[LEVEL_1_PAGE_TABLE_SIZE];

// Initialized the needed data structures before launching the target program
static void ClientInit(int argc, char* argv[]) {
    // Create output file
    char name[MAX_FILE_PATH] = "redspy_temporal.out.";
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


/* helper functions for shadow memory */
static uint8_t* GetOrCreateShadowBaseAddress(uint64_t address) {
    uint8_t *shadowPage;
    uint8_t ***l1Ptr = &gL1PageTable[LEVEL_1_PAGE_TABLE_SLOT(address)];
    if(*l1Ptr == 0) {
        *l1Ptr = (uint8_t **) mmap(0, LEVEL_2_PAGE_TABLE_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * (sizeof(uint64_t)), PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    } else if((shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0 ){
        shadowPage = (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * (sizeof(uint64_t)), PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    }
    return shadowPage;
}



static const uint64_t READ_ACCESS_STATES [] = {/*0 byte */0, /*1 byte */ ONE_BYTE_READ_ACTION, /*2 byte */ TWO_BYTE_READ_ACTION, /*3 byte */ 0, /*4 byte */ FOUR_BYTE_READ_ACTION, /*5 byte */0, /*6 byte */0, /*7 byte */0, /*8 byte */ EIGHT_BYTE_READ_ACTION};
static const uint64_t WRITE_ACCESS_STATES [] = {/*0 byte */0, /*1 byte */ ONE_BYTE_WRITE_ACTION, /*2 byte */ TWO_BYTE_WRITE_ACTION, /*3 byte */ 0, /*4 byte */ FOUR_BYTE_WRITE_ACTION, /*5 byte */0, /*6 byte */0, /*7 byte */0, /*8 byte */ EIGHT_BYTE_WRITE_ACTION};
static const uint8_t OVERFLOW_CHECK [] = {/*0 byte */0, /*1 byte */ 0, /*2 byte */ 0, /*3 byte */ 1, /*4 byte */ 2, /*5 byte */3, /*6 byte */4, /*7 byte */5, /*8 byte */ 6};

static unordered_map<uint64_t, uint64_t> RedMap[THREAD_MAX];
static inline void AddToRedTable(uint64_t key,  uint16_t value, THREADID threadId) {
#ifdef MULTI_THREADED
    LOCK_RED_MAP();
#endif
    unordered_map<uint64_t, uint64_t>::iterator it = RedMap[threadId].find(key);
    if ( it  == RedMap[threadId].end()) {
        RedMap[threadId][key] = value;
    } else {
        it->second += value;
    }
#ifdef MULTI_THREADED
    UNLOCK_RED_MAP();
#endif
}

static inline VOID EmptyCtxt(THREADID threadId){
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    int i;
    for( i = 0; i< REG_LAST; ++i){
        tData->regCtxt[i] = 0;
    }
    NUM_winds++;
   /* 
    if(NUM_winds > WINDOW_CLEAN){
        long count = tData->bytesWritten;
        long delNum = 0;
        //printf("size of the map %lu, total reg written %lu\n",count,tData->numRegWritten);
        unordered_map<uint64_t,uint64_t>::iterator it,ittmp;
        for (it = RedMap[threadId].begin(); it != RedMap[threadId].end();) {
            //printf("%lu\n",(*it).second);
            if((*it).second * 100.0 < count){
                delNum += (*it).second;
                ittmp = it;
                it++;
                RedMap[threadId].erase(ittmp);
            }else
                it++;
        }
        NUM_winds=0;
        tData->bytesWritten -= delNum;
    }*/
}

static inline VOID CheckGenValueAfterWrite(uint32_t opaqueHandle, THREADID threadId, void * regs, uint32_t regBytes, uint32_t regCount, ...){
    if(Sample_flag){
        NUM_INS++;
        if(NUM_INS > WINDOW_ENABLE){
            Sample_flag = false;
            NUM_INS = 0;
            EmptyCtxt(threadId);
            return;
        }
    }else{
        NUM_INS++;
        if(NUM_INS > WINDOW_DISABLE){
            Sample_flag = true;
            NUM_INS = 0;
        }else
            return;
    }
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    struct RegInfo * wRegs = (struct RegInfo *)regs;
    
    va_list ap;
    UINT8 i;
    va_start(ap, regCount);
    
    for (i = 0; i < regCount; i++) {
        REG reg = wRegs->regs[i];
        ADDRINT regV = va_arg(ap, ADDRINT);
        ADDRINT regBefore = *(ADDRINT *)(&tData->regValue[reg][0]);
        
        bool isRedundantWrite = (regBefore == regV);
        
        if(isRedundantWrite && tData->regCtxt[reg] != 0) {
            AddToRedTable(MAKE_CONTEXT_PAIR(tData->regCtxt[reg],curCtxtHandle),REG_Size(reg),threadId);
        }
        tData->regCtxt[reg] = curCtxtHandle;
        *(ADDRINT *)(&tData->regValue[reg][0]) = regV;
    }
    tData->bytesWritten += regBytes;
}

static inline  VOID CheckLargeValueAfterWrite(PIN_REGISTER* regRef, REG reg, uint32_t regSize, uint32_t opaqueHandle, THREADID threadId){
    
    if(Sample_flag){
        NUM_INS++;
        if(NUM_INS > WINDOW_ENABLE){
            Sample_flag = false;
            NUM_INS = 0;
            EmptyCtxt(threadId);
            return;
        }
    }else{
        NUM_INS++;
        if(NUM_INS > WINDOW_DISABLE){
            Sample_flag = true;
            NUM_INS = 0;
        }else
            return;
    }
    
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    
    //struct RegInfo * wRegs = (struct RegInfo *)regs;
    int i;
    bool isRedundantWrite = true;
    for(i = 0; i < 16; ++i){
        if(tData->regValue[reg][i] != regRef->byte[i])
            isRedundantWrite = false;
        tData->regValue[reg][i] = regRef->byte[i];
    }
    
    if(isRedundantWrite && tData->regCtxt[reg]!=0) {
        AddToRedTable(MAKE_CONTEXT_PAIR(tData->regCtxt[reg],curCtxtHandle),regSize,threadId);
    }
    tData->regCtxt[reg] = curCtxtHandle;
    tData->bytesWritten += regSize;
    
}


static inline void InstrumentReadValueBeforeAndAfterWriting(INS ins, struct RegInfo * wRegs, uint32_t opaqueHandle){
    
    UINT8 i;
    uint32_t totBytes = 0;
    uint32_t regSize;
    bool flag = true;
    for(i = 0; i < wRegs->count; ++i){
        regSize = REG_Size(wRegs->regs[i]);
        if (regSize > 8) {
            flag = false;
        }
        totBytes += regSize;
    }
    
    if(flag){
        switch (wRegs->count) {
            case 1:
                INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite,IARG_UINT32,opaqueHandle,IARG_THREAD_ID,IARG_PTR,wRegs,IARG_UINT32, totBytes,IARG_UINT32,wRegs->count,IARG_REG_VALUE,wRegs->regs[0],IARG_END);
                break;
            case 2:
                INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_END);
                break;
            case 3:
                INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_END);
                break;
            case 4:
                INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckGenValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,  IARG_PTR, wRegs,IARG_UINT32, totBytes, IARG_UINT32,wRegs->count, IARG_REG_VALUE, wRegs->regs[0], IARG_REG_VALUE, wRegs->regs[1], IARG_REG_VALUE, wRegs->regs[2], IARG_REG_VALUE, wRegs->regs[3], IARG_END);
                break;
                
            default:
                assert(0 && "NYI");
                break;
        }
        
    }else{
        if(wRegs->count == 1){
            if(wRegs->regs[0] == REG_ST0)
                return;
            INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckLargeValueAfterWrite, IARG_REG_CONST_REFERENCE,wRegs->regs[0], IARG_UINT32, wRegs->regs[0], IARG_UINT32, totBytes, IARG_UINT32, opaqueHandle, IARG_THREAD_ID,IARG_END);
        }else
            printf("Writing multiple registers with large size\n");
    }
}


/*********************** memory temporal redundancy functions **************************/
template<uint16_t AccessLen, uint32_t bufferOffset>
struct RedSpyAnalysis{
    static __attribute__((always_inline)) bool IsWriteRedundant(void * &addr, THREADID threadId){
        RedSpyThreadData* const tData = ClientGetTLS(threadId);
        AddrValPair * avPair = & tData->buffer[bufferOffset];
        addr = avPair->address;
        switch(AccessLen){
            case 1: return *((uint8_t*)(&avPair->value)) == *(static_cast<uint8_t*>(avPair->address));
            case 2: return *((uint16_t*)(&avPair->value)) == *(static_cast<uint16_t*>(avPair->address));
            case 4: return *((uint32_t*)(&avPair->value)) == *(static_cast<uint32_t*>(avPair->address));
            case 8: return *((uint64_t*)(&avPair->value)) == *(static_cast<uint64_t*>(avPair->address));
            default: return memcmp(&avPair->value, avPair->address, AccessLen) == 0;
        }
    }
    
    static __attribute__((always_inline)) VOID RecordNByteValueBeforeWrite(void* addr, THREADID threadId){
        if(Sample_flag){
            NUM_INS++;
            if(NUM_INS > WINDOW_ENABLE){
                Sample_flag = false;
                NUM_INS = 0;
                //EmptyCtxt(threadId);
                return;
            }
        }else{
            NUM_INS++;
            if(NUM_INS > WINDOW_DISABLE){
                Sample_flag = true;
                NUM_INS = 0;
            }else
                return;
        }
        RedSpyThreadData* const tData = ClientGetTLS(threadId);
        tData->bytesWritten += AccessLen;
        AddrValPair * avPair = & tData->buffer[bufferOffset];
//printf("\n B: %lx %lu %d %d %lx", (uint64_t)addr, tData->bytesWritten, AccessLen, bufferOffset, (uint64_t)ip);
//fflush(stdout);
        avPair->address = addr;
        switch(AccessLen){
            case 1: *((uint8_t*)(&avPair->value)) = *(static_cast<uint8_t*>(addr)); break;
            case 2: *((uint16_t*)(&avPair->value)) = *(static_cast<uint16_t*>(addr)); break;
            case 4: *((uint32_t*)(&avPair->value)) = *(static_cast<uint32_t*>(addr)); break;
            case 8: *((uint64_t*)(&avPair->value)) = *(static_cast<uint64_t*>(addr)); break;
            default:memcpy(&avPair->value, addr, AccessLen);
        }
    }
    
    static __attribute__((always_inline)) VOID CheckNByteValueAfterWrite(uint32_t opaqueHandle, THREADID threadId){
        if(!Sample_flag)
           return;
        void * addr;
        bool isRedundantWrite = IsWriteRedundant(addr, threadId);
//printf("\t A: %lx %lu %d %d %lx", (uint64_t)addr, ClientGetTLS(threadId)->bytesWritten, AccessLen, bufferOffset, (uint64_t)ip);
//fflush(stdout);
        ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
        
        uint8_t* status = GetOrCreateShadowBaseAddress((uint64_t)addr);
        ContextHandle_t * __restrict__ prevIP = (ContextHandle_t*)(status + PAGE_OFFSET((uint64_t)addr) * sizeof(ContextHandle_t));
        const bool isAccessWithinPageBoundary = IS_ACCESS_WITHIN_PAGE_BOUNDARY( (uint64_t)addr, AccessLen);
        if(isRedundantWrite) {
            // detected redundancy
            if(isAccessWithinPageBoundary) {
                // All from same ctxt?
                if (UnrolledConjunction<0, AccessLen, 1>::Body( [&] (int index) -> bool { return (prevIP[index] == prevIP[0]); })) {
                    // report in RedTable
                    AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0], curCtxtHandle), AccessLen, threadId);
                    // Update context
                    UnrolledLoop<0, AccessLen, 1>::Body( [&] (int index) -> VOID {
                        // Update context
                        prevIP[index] = curCtxtHandle;
                    });
                } else {
                    // different contexts
                    UnrolledLoop<0, AccessLen, 1>::Body( [&] (int index) -> VOID {
                        // report in RedTable
                        AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[index], curCtxtHandle), 1,threadId);
                        // Update context
                        prevIP[index] = curCtxtHandle;
                    });
                }
            } else {
                // Write across a 64-K page boundary
                // First byte is on this page though
                AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0], curCtxtHandle), 1, threadId);
                // Update context
                prevIP[0] = curCtxtHandle;
                
                // Remaining bytes [1..AccessLen] somewhere will across a 64-K page boundary
                UnrolledLoop<1, AccessLen, 1>::Body( [&] (int index) -> VOID {
                    status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
                    prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
                    // report in RedTable
                    AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0 /* 0 is correct*/ ], curCtxtHandle), 1, threadId);
                    // Update context
                    prevIP[0] = curCtxtHandle;
                } );
            }
        } else {
            // No redundancy.
            // Just update contexts
            if(isAccessWithinPageBoundary) {
                UnrolledLoop<0, AccessLen, 1>::Body( [&] (int index) -> VOID {
                    // Update context
                    prevIP[index] = curCtxtHandle;
                });
            } else {
                // Write across a 64-K page boundary
                UnrolledLoop<0, AccessLen, 1>::Body( [&] (int index) -> VOID {
                    status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
                    prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
                    // Update context
                    prevIP[0] = curCtxtHandle;
                } );
            }
        }
    }
};


static inline VOID RecordValueBeforeLargeWrite(void* addr, UINT32 accessLen,  uint32_t bufferOffset, THREADID threadId){
    if(Sample_flag){
        NUM_INS++;
        if(NUM_INS > WINDOW_ENABLE){
            Sample_flag = false;
            NUM_INS = 0;
            //EmptyCtxt(threadId);
            return;
        }
    }else{
        NUM_INS++;
        if(NUM_INS > WINDOW_DISABLE){
            Sample_flag = true;
            NUM_INS = 0;
        }else
            return;
    }

    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    tData->bytesWritten += accessLen;
    memcpy(& (tData->buffer[bufferOffset].value), addr, accessLen);
    tData->buffer[bufferOffset].address = addr;
}

static inline VOID CheckAfterLargeWrite(UINT32 accessLen,  uint32_t bufferOffset, uint32_t opaqueHandle, THREADID threadId){

    if(!Sample_flag)
        return;
    RedSpyThreadData* const tData = ClientGetTLS(threadId);
    void * addr = tData->buffer[bufferOffset].address;
    ContextHandle_t curCtxtHandle = GetContextHandle(threadId, opaqueHandle);
    
    uint8_t* status = GetOrCreateShadowBaseAddress((uint64_t)addr);
    ContextHandle_t * __restrict__ prevIP = (ContextHandle_t*)(status + PAGE_OFFSET((uint64_t)addr) * sizeof(ContextHandle_t));
    if(memcmp( & (tData->buffer[bufferOffset].value), addr, accessLen) == 0){
        // redundant
        for(UINT32 index = 0 ; index < accessLen; index++){
            status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
            prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
            // report in RedTable
            AddToRedTable(MAKE_CONTEXT_PAIR(prevIP[0 /* 0 is correct*/ ], curCtxtHandle), 1, threadId);
            // Update context
            prevIP[0] = curCtxtHandle;
        }
    }else{
        // Not redundant
        for(UINT32 index = 0 ; index < accessLen; index++){
            status = GetOrCreateShadowBaseAddress((uint64_t)addr + index);
            prevIP = (ContextHandle_t*)(status + PAGE_OFFSET(((uint64_t)addr + index)) * sizeof(ContextHandle_t));
            // Update context
            prevIP[0] = curCtxtHandle;
        }
    }
}

#define HANDLE_CASE(NUM, BUFFER_INDEX) \
case (NUM):{INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::RecordNByteValueBeforeWrite, IARG_MEMORYOP_EA, memOp, IARG_THREAD_ID, IARG_END);\
INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) RedSpyAnalysis<(NUM), (BUFFER_INDEX)>::CheckNByteValueAfterWrite, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_INST_PTR,IARG_END);}break


static int GetNumWriteOperandsInIns(INS ins, UINT32 & whichOp){
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

template<uint32_t readBufferSlotIndex>
struct RedSpyInstrument{
    static __attribute__((always_inline)) void InstrumentReadValueBeforeAndAfterWriting(INS ins, UINT32 memOp, uint32_t opaqueHandle){
        UINT32 refSize = INS_MemoryOperandSize(ins, memOp);
        switch(refSize) {
                HANDLE_CASE(1, readBufferSlotIndex);
                HANDLE_CASE(2, readBufferSlotIndex);
                HANDLE_CASE(4, readBufferSlotIndex);
                HANDLE_CASE(8, readBufferSlotIndex);
                HANDLE_CASE(10, readBufferSlotIndex);
                HANDLE_CASE(16, readBufferSlotIndex);
                
            default: {
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) RecordValueBeforeLargeWrite, IARG_MEMORYOP_EA, memOp, IARG_MEMORYWRITE_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_THREAD_ID, IARG_END);
                INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR) CheckAfterLargeWrite, IARG_MEMORYREAD_SIZE, IARG_UINT32, readBufferSlotIndex, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_END);
            }
        }
    }
};
/*
static inline bool INS_IsIgnorable(INS ins){
    
    if(INS_IsFarJump(ins))
        return true;
    else if(INS_IsRet(ins))
        return true;
    return false;
}*/

static inline bool INS_IsIgnorable(INS ins){
    if( INS_IsFarJump(ins) || INS_IsDirectFarJump(ins) || INS_IsMaskedJump(ins))
        return true;
    else if(INS_IsRet(ins) || INS_IsIRet(ins))
        return true;
    else if(INS_IsCall(ins) || INS_IsSyscall(ins))
        return true;
    else if(INS_IsBranch(ins) || INS_IsRDTSC(ins) || INS_IsNop(ins))
        return true;
    return false;
}

static inline bool REG_IsIgnorable(REG reg){
    if (REG_is_seg(reg))
        return true;
    else if(REG_is_pin_gr(reg))
        return true;
    else if(reg == REG_MXCSR)
        return true;
    return false;
}

static VOID InstrumentInsCallback(INS ins, VOID* v, const uint32_t opaqueHandle) {
    if (!INS_HasFallThrough(ins)) return;
    if (INS_IsIgnorable(ins))return;
    if (INS_IsBranchOrCall(ins) || INS_IsRet(ins)) return;
    
    if (INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins)){

      //Instrument memory writes to find redundancy
      // Special case, if we have only one write operand
      UINT32 whichOp = 0;
      if(GetNumWriteOperandsInIns(ins, whichOp) == 1){
        // Read the value at location before and after the instruction
        RedSpyInstrument<0>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
        return;
      }
    
      UINT32 memOperands = INS_MemoryOperandCount(ins);
      int readBufferSlotIndex=0;
      for(UINT32 memOp = 0; memOp < memOperands; memOp++) {
        
        if(!INS_MemoryOperandIsWritten(ins, memOp))
            continue;
        
        switch (readBufferSlotIndex) {
            case 0:
                // Read the value at location before and after the instruction
                RedSpyInstrument<0>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                break;
            case 1:
                // Read the value at location before and after the instruction
                RedSpyInstrument<1>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                break;
            case 2:
                // Read the value at location before and after the instruction
                RedSpyInstrument<2>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                break;
            case 3:
                // Read the value at location before and after the instruction
                RedSpyInstrument<3>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                break;
            case 4:
                // Read the value at location before and after the instruction
                RedSpyInstrument<4>::InstrumentReadValueBeforeAndAfterWriting(ins, whichOp, opaqueHandle);
                break;
            default:
                assert(0 && "NYI");
                break;
        }
        
        // use next slot for the next write operand
        readBufferSlotIndex++;
      }

    }else{  

      //Instrument register writes to find redundancy
      struct RegInfo * wRegs = new struct RegInfo;
    
      UINT32 numOperands = INS_OperandCount(ins);
    
      int regCount = 0;
    
      for(UINT32 Oper = 0; Oper < numOperands; Oper++) {
        
        if(!INS_OperandWritten(ins, Oper) || !INS_OperandIsReg(ins,Oper))
            continue;
        
        REG curReg = INS_OperandReg(ins,Oper);
        
        if(REG_IsIgnorable(curReg))
            continue;
        
        if (regCount >= MAX_WRITE_OPS_IN_INS) {
            assert(0 && "NYI");
            break;
        }else{
            wRegs->regs[regCount] = curReg;
            regCount++;
            wRegs->count = regCount;
        }
      }
      if(regCount > 0)
        InstrumentReadValueBeforeAndAfterWriting(ins, wRegs, opaqueHandle);
    }
}


struct RedundacyData {
    ContextHandle_t dead;
    ContextHandle_t kill;
    uint64_t frequency;
};




static inline bool RedundacyCompare(const struct RedundacyData &first, const struct RedundacyData &second) {
    return first.frequency > second.frequency ? true : false;
}

static void PrintRedundancyPairs(THREADID threadId) {
    vector<RedundacyData> tmpList;
    vector<RedundacyData>::iterator tmpIt;

    uint64_t grandTotalRedundantBytes = 0;
    fprintf(gTraceFile, "*************** Dump Data from Thread %d ****************\n", threadId);
    for (unordered_map<uint64_t, uint64_t>::iterator it = RedMap[threadId].begin(); it != RedMap[threadId].end(); ++it) {
        ContextHandle_t dead = DECODE_DEAD((*it).first);
        ContextHandle_t kill = DECODE_KILL((*it).first);

        for(tmpIt = tmpList.begin();tmpIt != tmpList.end(); ++tmpIt){
             bool ct1 = false;
             if(dead == 0 || ((*tmpIt).dead) == 0){
                  if(dead == 0 && ((*tmpIt).dead) == 0)
                       ct1 = true;
             }else{
                  ct1 = IsSameSourceLine(dead,(*tmpIt).dead);
             }
             bool ct2 = IsSameSourceLine(kill,(*tmpIt).kill);
             if(ct1 && ct2){
                  (*tmpIt).frequency += (*it).second;
                  grandTotalRedundantBytes += (*it).second;
                  break;
             }
        }
        if(tmpIt == tmpList.end()){
             RedundacyData tmp = { dead, kill, (*it).second};
             tmpList.push_back(tmp);
             grandTotalRedundantBytes += tmp.frequency;
        }
    }  
        
    fprintf(gTraceFile, "\n Total redundant bytes = %f %%\n", grandTotalRedundantBytes * 100.0 / ClientGetTLS(threadId)->bytesWritten);
    
    sort(tmpList.begin(), tmpList.end(), RedundacyCompare);
    vector<struct AnalyzedMetric_t>::iterator listIt;
    int cntxtNum = 0;
    for (vector<RedundacyData>::iterator listIt = tmpList.begin(); listIt != tmpList.end(); ++listIt) {
        if (cntxtNum < MAX_REDUNDANT_CONTEXTS_TO_LOG) {
            fprintf(gTraceFile, "\n======= (%f) %% ======\n", (*listIt).frequency * 100.0 / grandTotalRedundantBytes);
            if ((*listIt).dead == 0) {
                fprintf(gTraceFile, "\n Prepopulated with  by OS\n");
            } else {
                PrintFullCallingContext((*listIt).dead);
            }
            fprintf(gTraceFile, "\n---------------------Redundantly written by---------------------------\n");
            PrintFullCallingContext((*listIt).kill);
        }
        else {
            break;
        }
        cntxtNum++;
    }
}

// On each Unload of a loaded image, the accummulated redundancy information is dumped
static VOID ImageUnload(IMG img, VOID* v) {
    fprintf(gTraceFile, "\n TODO .. Multi-threading is not well supported.");    
    THREADID  threadid =  PIN_ThreadId();
    fprintf(gTraceFile, "\nUnloading %s", IMG_Name(img).c_str());
    // Update gTotalInstCount first
    PIN_LockClient();
    PrintRedundancyPairs(threadid);
    PIN_UnlockClient();
    // clear redmap now
    RedMap[threadid].clear();
}

static VOID ThreadFiniFunc(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v) {
}

static VOID FiniFunc(INT32 code, VOID *v) {
    // do whatever you want to the full CCT with footpirnt
}


static void InitThreadData(RedSpyThreadData* tdata){
    tdata->bytesWritten = 0;
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
        return Usage2();
    
    // Initialize Symbols, we need them to report functions and lines
    PIN_InitSymbols();
    
    // Init Client
    ClientInit(argc, argv);
    // Intialize CCTLib
    PinCCTLibInit(INTERESTING_INS_ALL, gTraceFile, InstrumentInsCallback, 0);
    
    
    // Obtain  a key for TLS storage.
    client_tls_key = PIN_CreateThreadDataKey(0 /*TODO have a destructir*/);
    // Register ThreadStart to be called when a thread starts.
    PIN_AddThreadStartFunction(ThreadStart, 0);
    
    
    // fini function for post-mortem analysis
    PIN_AddThreadFiniFunction(ThreadFiniFunc, 0);
    PIN_AddFiniFunction(FiniFunc, 0);
    //TRACE_AddInstrumentFunction(InstrumentTrace, 0);
    
    
    // Register ImageUnload to be called when an image is unloaded
    IMG_AddUnloadFunction(ImageUnload, 0);
    
    // Launch program now
    PIN_StartProgram();
    return 0;
}


