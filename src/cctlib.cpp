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

#include <set>
#include "cctlib.H"


#define __STDC_FORMAT_MACROS
#include <stdio.h>
#include <stdlib.h>
#include "pin.H"
#include <map>
#include <tr1/unordered_map>
#include <list>
#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <locale>
#include <unistd.h>
#include <sys/syscall.h>
#include <iostream>
#include <assert.h>
#include <sys/mman.h>
#include <exception>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <setjmp.h>
#include <sstream>
#include <limits.h>
#include <unwind.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#ifdef TARGET_MAC
#include <libelf/libelf.h>
#include <libelf/gelf.h>
#elif defined(TARGET_LINUX)
#include <libelf.h>
#include <gelf.h>
#else
"Unsupported platform"
#endif


#include "splay-macros.h"
// Need GOOGLE sparse hash tables
#include <google/sparse_hash_map>
#include <google/dense_hash_map>
// XED for printing instr
extern "C" {
#include "xed-interface.h"
}

using google::sparse_hash_map;      // namespace where class lives by default
using google::dense_hash_map;      // namespace where class lives by default
using namespace std;
using namespace std::tr1;
namespace boostFS = ::boost::filesystem;

namespace PinCCTLib {

// All globals
#define USE_SPLAY_TREE
#define MAX_PATH_NAME 1024
#define MALLOC_FN_NAME "malloc"
#define CALLOC_FN_NAME "calloc"
#define REALLOC_FN_NAME "realloc"
#define FREE_FN_NAME "free"


#define CCTLIB_SERIALIZATION_DEFAULT_DIR_NAME "cctlib-database-"
#ifndef MAX_IPNODES
#if defined(TARGET_IA32E)
// 2^32  IPNODES
#define MAX_IPNODES (1L << 32)
#elif defined(TARGET_IA32)
// 1M IPNODES
#define MAX_IPNODES (1L << 20)
#else
    "SHOULD NEVER REACH HERE"
#endif
#endif

#ifndef MAX_STRING_POOL_NODES
#if defined(TARGET_IA32E)
// 2^32  IPNODES
#define MAX_STRING_POOL_NODES (1L << 30)
#elif defined(TARGET_IA32)
// 1M charaters
#define MAX_STRING_POOL_NODES (1L << 20)
#else
    "SHOULD NEVER REACH HERE"
#endif
#endif

// Assuming 128 byte line size.
#define CACHE_LINE_SIZE (128)


#define GET_CONTEXT_HANDLE_FROM_IP_NODE(node) ((ContextHandle_t) ( (node) ? ((node) - GLOBAL_STATE.preAllocatedContextBuffer) : 0 ))
#define GET_IPNODE_FROM_CONTEXT_HANDLE(handle) ( (handle) ? (GLOBAL_STATE.preAllocatedContextBuffer + (handle)) : NULL )


#define BREAK_HERE raise(SIGINT)

//Serialization related macros
#define SERIALIZED_SHADOW_TRACE_IP_FILE_SUFFIX "/TraceMap.traceShadowMap"
#define SERIALIZED_CCT_FILE_PREFIX "/Thread-"
#define SERIALIZED_CCT_FILE_EXTN ".cct"
#define SERIALIZED_CCT_FILE_SUFFIX "-CCTMap.cct"

// Platform specific macros
#ifdef TARGET_MAC
#define MAP_ANONYMOUS MAP_ANON
    typedef uintptr_t _Unwind_Ptr ;
#endif


    /******** Fwd declarations **********/
    struct TraceNode;
    struct SerializedTraceNode;
    struct IPNode;
    struct TraceSplay;
    struct ModuleInfo;
    struct QNode;

#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC

#define LOCKED (1)
#define UNLOCKED (0)
#define UNLOCKED_AND_PREDECESSOR_WAS_WRITER (2)

    struct varType;
    struct PendingOps_t;
    struct ConcurrentReaderWriterTree_t;
    typedef set<varType> varSet;
#endif

    static inline ADDRINT GetIPFromInfo(IPNode*);
    static inline const string& GetModulePathFromInfo(IPNode* ipNode);
    static inline void GetLineFromInfo(const ADDRINT& ip, uint32_t& lineNo, string& filePath);

    static inline bool IsValidIP(ADDRINT ip);
    static void SerializeCCTNode(TraceNode* traceNode, FILE* const fp);

    enum CCTLibUsageMode {CCT_LIB_MODE_COLLECTION = 1, CCT_LIB_MODE_POSTMORTEM = 2};


#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
    enum AccessStatus {START_READING = 0, END_READING = 1, WAITING_WRITE = 3,  WRITE_STARTED = 4};
#endif


    /******** Data structures **********/
    struct TraceNode {
        IPNode* callerIPNode;
        IPNode*   childIPs;
        uint32_t traceKey; // max of 2^32 traces allowed
        uint32_t nSlots;
    };

    struct SerializedTraceNode {
        uint32_t traceKey;
        uint32_t nSlots;
        ContextHandle_t  childIPs;
    };


    struct TraceSplay {
        ADDRINT key;
        TraceNode* value;
        TraceSplay* left;
        TraceSplay* right;
    };

    struct IPNode {
        TraceNode* parentTraceNode;
#ifdef USE_SPLAY_TREE
        TraceSplay* calleeTraceNodes;
#else
        sparse_hash_map<ADDRINT, TraceNode*>* calleeTraceNodes;
#endif

#ifdef HAVE_METRIC_PER_IPNODE
        void *metric;
#endif
    };

    typedef struct QNode {
        struct QNode* volatile next;
        union {
            struct {
                volatile bool locked: 1;
                volatile bool predecessorWasWriter: 1;
            };
            volatile uint8_t status;
        };
    } QNode;

// should become TLS
    struct ThreadData {
#ifndef USE_SPLAY_TREE
        sparse_hash_map<ADDRINT, TraceNode*>::iterator gTraceIter;
#endif

        uint32_t tlsThreadId; // useful only during deserialization
        struct IPNode* tlsCurrentIPNode;
        struct IPNode* tlsCurrentChildIPs;
        struct TraceNode* tlsCurrentTraceNode;
        struct IPNode* tlsRootIPNode;
        struct TraceNode* tlsRootTraceNode;
        bool tlsInitiatedCall;

        struct TraceNode* tlsParentThreadTraceNode;
        struct IPNode* tlsParentThreadIPNode;


        sparse_hash_map<ADDRINT, IPNode*> tlsLongJmpMap;
        ADDRINT tlsLongJmpHoldBuf;

        uint32_t curSlotNo;

        // The caller that can handle the current exception
        struct TraceNode* tlsExceptionHandlerTraceNode;
        struct IPNode* tlsExceptionHandlerIPNode;
        void* tlsStackBase;
        void*   tlsStackEnd;



//DO_DATA_CENTRIC
        size_t tlsDynamicMemoryAllocationSize;
        ContextHandle_t tlsDynamicMemoryAllocationPathHandle;
#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        uint32_t rwLockStatus __attribute__((aligned(CACHE_LINE_SIZE)));
        struct ConcurrentReaderWriterTree_t* tlsLatestConcurrentTree;
        volatile uint8_t tlsMallocDSAccessStatus;
#endif
        // TODO .. identify why perf screws up w/o this buffer
        uint32_t DUMMY_HELPS_PERF  __attribute__((aligned(CACHE_LINE_SIZE)));
    } __attribute__((aligned));


//DO_DATA_CENTRIC

// Data centric support

#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC


    struct varType {
        void* start;
        void* end;
        union {
            uint32_t pathHandle;
            uint32_t symName;
        };
        uint8_t objectType;
        varType(void* s, void* e, uint32_t handle, uint8_t type): start(s), end(e), pathHandle(handle), objectType(type) {}
    };

    bool operator < (varType const& a, varType const& b) {
        return (ADDRINT)a.start < (ADDRINT)b.start;
    }

    struct PendingOps_t {
        uint8_t operation;
        varType var;
        PendingOps_t(const uint8_t o, varType const& v): operation(o), var(v) {}

    } __attribute__((aligned(CACHE_LINE_SIZE)));

    enum {INSERT = 0, DELETE = 1};

    struct ConcurrentReaderWriterTree_t {
        vector<PendingOps_t> pendingOps;
        varSet tree;
    };

#endif



// Information about loaded images.
    struct ModuleInfo {
        // name
        string moduleName;
        //Offset from the image's link-time address to its load-time address.
        ADDRINT imgLoadOffset;
    };

    /******** Globals variables **********/


    struct CCT_LIB_GLOBAL_STATE {
// Should data-centric attribution be perfomed?
        bool doDataCentric; // false  by default
        bool applicationStarted ; // false by default
        uint8_t cctLibUsageMode;
        FILE* CCTLibLogFile;
        CCTLibInstrumentInsCallback userInstrumentationCallback;
        VOID* userInstrumentationCallbackArg;
        char disassemblyBuff[200]; // string of 0 by default
        /// XED state
        xed_state_t  cct_xed_state;
        // prefix string for flushing all data for post processing.
        string CCTLibFilePathPrefix;
        IPNode* preAllocatedContextBuffer;
        char* preAllocatedStringPool;
        // SEGVHANDLEING FOR BAD .plt
        jmp_buf env;
        struct sigaction sigAct;
        //Load module info
        unordered_map<UINT32, ModuleInfo> ModuleInfoMap;
        // serialization directory path
        string serializationDirectory;
        // Deserialized CCTs
        vector<ThreadData> deserializedCCTs;
        //dense_hash_map<ADDRINT, void *> traceShadowMap;
        unordered_map<uint32_t, void*> traceShadowMap;
        PIN_LOCK lock;
        // key for accessing TLS storage in the threads. initialized once in main()
        TLS_KEY CCTLibTlsKey __attribute__((aligned(CACHE_LINE_SIZE))); // align to eliminate any false sharing with other  members
        uint32_t numThreads __attribute__((aligned(CACHE_LINE_SIZE))); // initial value = 0  // align to eliminate any false sharing with other  members
        uint32_t curPreAllocatedStringPoolIndex __attribute__((aligned(CACHE_LINE_SIZE))); // align to eliminate any false sharing with other  members
        uint64_t curPreAllocatedContextBufferIndex __attribute__((aligned(CACHE_LINE_SIZE))); // align to eliminate any false sharing with other  members
        // keys to associate parent child threads
        volatile uint64_t threadCreateCount __attribute__((aligned(CACHE_LINE_SIZE))) ; // initial value = 0  // align to eliminate any false sharing with other  members
        volatile uint64_t threadCaptureCount __attribute__((aligned(CACHE_LINE_SIZE))) ; // initial value = 0  // align to eliminate any false sharing with other  members
        volatile TraceNode* threadCreatorTraceNode __attribute__((aligned(CACHE_LINE_SIZE)));  // align to eliminate any false sharing with other  members
        volatile IPNode* threadCreatorIPNode __attribute__((aligned(CACHE_LINE_SIZE)));  // align to eliminate any false sharing with other  members
        volatile bool DSLock;
#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        //Data centric support
        unordered_map<UINT32, vector<PendingOps_t> > staticVariablesInModule;
        __attribute__((aligned(CACHE_LINE_SIZE)));  // align to eliminate any false sharing with other  members
        volatile ConcurrentReaderWriterTree_t* latestConcurrentTree;
        __attribute__((aligned(CACHE_LINE_SIZE)));  // align to eliminate any false sharing with other  members
        ConcurrentReaderWriterTree_t concurrentReaderWriterTree[2];
        __attribute__((aligned(CACHE_LINE_SIZE)));  // align to eliminate any false sharing with other  members
#endif
    } static GLOBAL_STATE;

    static void SegvHandler(int);



    /******** Function definitions **********/


    inline BOOL IsCallOrRetIns(INS ins) {
        if(INS_IsProcedureCall(ins))
            return true;

        if(INS_IsRet(ins))
            return true;

        return false;
    }


// function to get the next unique key for a trace
    ADDRINT GetNextTraceKey() {
        static uint32_t traceKey = 0;
        uint32_t key = __sync_fetch_and_add(&traceKey, 1);

        if(key == UINT_MAX) {
            fprintf(stderr, "\n UINT_MAX traces created! Exiting...\n");
            PIN_ExitProcess(-1);
        }

        return key;
    }

// function to access thread-specific data
    static inline ThreadData* CCTLibGetTLS(const THREADID threadId) {
        ThreadData* tdata =
            static_cast<ThreadData*>(PIN_GetThreadData(GLOBAL_STATE.CCTLibTlsKey, threadId));
        return tdata;
    }

    // This function is for dumping call path from debugger.
    void DumpCallStack(THREADID id, uint32_t slot) {
        ThreadData* tData = CCTLibGetTLS(id);
        fprintf(stderr, "\n slot =%u, max = %u\n", slot, tData->tlsCurrentTraceNode->nSlots);
        PIN_LockClient();
        ContextHandle_t h = &(tData->tlsCurrentTraceNode->childIPs[slot]) - GLOBAL_STATE.preAllocatedContextBuffer;
        fprintf(stderr, "\n");
        vector<Context> contextVec;
        GetFullCallingContext(h, contextVec);

        for(uint32_t i = 0 ; i < contextVec.size(); i++) {
            fprintf(stderr, "\n%u:%p:%s:%s:%s:%u", contextVec[i].ctxtHandle, (void*) contextVec[i].ip, contextVec[i].disassembly.c_str(), contextVec[i].functionName.c_str(), contextVec[i].filePath.c_str(), contextVec[i].lineNo);
        }

        PIN_UnlockClient();
    }

    // This function is for dumping call path from debugger.
    void DumpCallStackEasy() {
        DumpCallStack(PIN_ThreadId(), 0);
    }


#if 0
    static int SetJmpOverride(const CONTEXT* 	ctxt, 	THREADID 	tid, AFUNPTR gOriginalSetjmpRtn, jmp_buf env) {
        int ret = -1;
        PIN_CallApplicationFunction(ctxt,
                                    tid,
                                    CALLINGSTD_DEFAULT,
                                    gOriginalSetjmpRtn,
                                    PIN_PARG(int), &ret,
                                    PIN_PARG(void*), env,
                                    PIN_PARG_END());

        if(ret == 0) {
            // Remember the context.
            fprintf(stderr, "\n Here due to SetJmp\n");
        } else {
            //
            fprintf(stderr, "\n Here due to LongJmp\n");
        }

        return ret;
    }
#endif

    static inline void UpdateCurTraceAndIp(ThreadData* tData, TraceNode* const trace, IPNode* const ipNode) {
        tData->tlsCurrentTraceNode = trace;
        tData->tlsCurrentChildIPs = trace->childIPs;
        tData->tlsCurrentIPNode = ipNode;
    }

    static inline void UpdateCurTraceAndIp(ThreadData* tData, TraceNode* const trace) {
        UpdateCurTraceAndIp(tData, trace, trace->childIPs);
    }

    static inline void UpdateCurTraceOnly(ThreadData* tData, TraceNode* const trace) {
        tData->tlsCurrentTraceNode = trace;
        tData->tlsCurrentChildIPs = trace->childIPs;
    }


    static inline VOID CaptureSigSetJmpCtxt(ADDRINT buf, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        // Does not work when a trace has zero IPs!! tData->tlsLongJmpMap[buf] = tData->tlsCurrentIPNode->parentTraceNode->callerIPNode;
        tData->tlsLongJmpMap[buf] = tData->tlsCurrentTraceNode->callerIPNode;
        //fprintf(GLOBAL_STATE.CCTLibLogFile,"\n CaptureSetJmpCtxt buf = %lu, tData->tlsCurrentIPNode = %p", buf, tData->tlsCurrentIPNode);
    }

    static inline VOID HoldLongJmpBuf(ADDRINT buf, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->tlsLongJmpHoldBuf = buf;
        //fprintf(GLOBAL_STATE.CCTLibLogFile,"\n HoldLongJmpBuf tlsLongJmpHoldBuf = %lu, tData->tlsCurrentIPNode = %p", tData->tlsLongJmpHoldBuf, tData->tlsCurrentIPNode);
    }

    static inline VOID RestoreSigLongJmpCtxt(THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        assert(tData->tlsLongJmpHoldBuf);
        tData->tlsCurrentIPNode = tData->tlsLongJmpMap[tData->tlsLongJmpHoldBuf];
        UpdateCurTraceOnly(tData, tData->tlsCurrentIPNode->parentTraceNode);
        tData->tlsLongJmpHoldBuf = 0; // reset so that next time we can check if it was set correctly.
        //fprintf(GLOBAL_STATE.CCTLibLogFile,"\n RestoreSigLongJmpCtxt2 tlsLongJmpHoldBuf = %lu",tData->tlsLongJmpHoldBuf);
    }


    static bool IsCallInstruction(ADDRINT ip) {
        // Get the instruction in a string
        xed_decoded_inst_t      xedd;
        /// XED state
        xed_state_t  xed_state;
        xed_decoded_inst_zero_set_mode(&xedd, &xed_state);

        if(XED_ERROR_NONE == xed_decode(&xedd, (const xed_uint8_t*)(ip), 15)) {
            if(XED_CATEGORY_CALL == xed_decoded_inst_get_category(&xedd))
                return true;
            else
                return false;
        } else {
            assert(0 && "failed to disassemble instruction");
            return false;
        }
    }

#define X86_DIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(callsite) (callsite - 5)
#define X86_INDIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(callsite) (callsite - 2)

    bool IsIpPresentInTrace(ADDRINT exceptionCallerReturnAddrIP, TraceNode* traceNode, uint32_t* ipSlot) {
        ADDRINT* tracesIPs = (ADDRINT*)GLOBAL_STATE.traceShadowMap[traceNode->traceKey];
        ADDRINT ipDirectCall = X86_DIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(exceptionCallerReturnAddrIP);
        ADDRINT ipIndirectCall = X86_INDIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(exceptionCallerReturnAddrIP);

        for(uint32_t i = 0; i < traceNode->nSlots; i++) {
            //printf("\n serching = %p", tracesIPs[i]);
            if((tracesIPs[i] == ipDirectCall) && IsCallInstruction(ipDirectCall)) {
                *ipSlot = i;
                return true;
            }

            if((tracesIPs[i] == ipIndirectCall) && IsCallInstruction(ipIndirectCall)) {
                *ipSlot = i;
                return true;
            }
        }

        return false;
    }

    static TraceNode* FindNearestCallerCoveringIP(ADDRINT exceptionCallerReturnAddrIP, uint32_t* ipSlot, ThreadData* tData) {
        TraceNode* curTrace = tData->tlsCurrentTraceNode;

        //int i = 0;
        while(curTrace) {
            if(IsIpPresentInTrace(exceptionCallerReturnAddrIP, curTrace, ipSlot)) {
                //printf("\n found at %d", i++);
                return curTrace;
            }

            // break if we have finished looking at the root
            if(curTrace == tData->tlsRootTraceNode)
                break;

            curTrace = curTrace->callerIPNode->parentTraceNode;
            //printf("\n did not find so far %d", i++);
        }

        printf("\n This is a terrible place to be in.. report to mc29@rice.edu\n");
        assert(0 && " Should never reach here");
        PIN_ExitProcess(-1);
        return NULL;
    }


    static VOID CaptureCallerThatCanHandleException(VOID* exceptionCallerContext, THREADID threadId) {
        //printf("\n Target ip is %p, exceptionCallerIP = %p", targeIp);
        //        extern ADDRINT _Unwind_GetIP(VOID *);
        //        ADDRINT exceptionCallerIP = (ADDRINT) _Unwind_GetIP(exceptionCallerContext);
        _Unwind_Ptr  exceptionCallerReturnAddrIP = _Unwind_GetIP((struct _Unwind_Context*)exceptionCallerContext);
        _Unwind_Ptr directExceptionCallerIP = X86_DIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(exceptionCallerReturnAddrIP);
        _Unwind_Ptr indirectExceptionCallerIP = X86_INDIRECT_CALL_SITE_ADDR_FROM_RETURN_ADDR(exceptionCallerReturnAddrIP);
        //printf("\n directExceptionCallerIP = %p indirectExceptionCallerIP = %p", (void*)directExceptionCallerIP, (void*)indirectExceptionCallerIP);
        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n directExceptionCallerIP = %p indirectExceptionCallerIP = %p", (void*)directExceptionCallerIP, (void*)indirectExceptionCallerIP);
        // Walk the CCT chain staring from tData->tlsCurrentTraceNode looking for the nearest one that has targeIp in the range.
        ThreadData* tData = CCTLibGetTLS(threadId);
        // Record the caller that can handle the exception.
        uint32_t ipSlot;
        tData->tlsExceptionHandlerTraceNode  = FindNearestCallerCoveringIP(exceptionCallerReturnAddrIP, &ipSlot, tData);
        tData->tlsExceptionHandlerIPNode = &(tData->tlsExceptionHandlerTraceNode->childIPs[ipSlot]);
    }


    static VOID SetCurTraceNodeAfterException(THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        // Record the caller that can handle the exception.
        UpdateCurTraceAndIp(tData, tData->tlsExceptionHandlerTraceNode, tData->tlsExceptionHandlerIPNode);
#if 1
        //printf("\n reset tData->tlsCurrentTraceNode to the handler");
        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n reset tData->tlsCurrentTraceNode to the handler");
#endif
    }


    static VOID SetCurTraceNodeAfterExceptionIfContextIsInstalled(ADDRINT retVal, THREADID threadId) {
        // if the return value is _URC_INSTALL_CONTEXT then we will reset the shadow stack, else NOP
        // Commented ... caller ensures it is inserted only at the end.
        // if(retVal != _URC_INSTALL_CONTEXT)
        //    return;
        ThreadData* tData = CCTLibGetTLS(threadId);
        // Record the caller that can handle the exception.
        UpdateCurTraceAndIp(tData, tData->tlsExceptionHandlerTraceNode, tData->tlsExceptionHandlerIPNode);
#if 1
        //printf("\n (SetCurTraceNodeAfterExceptionIfContextIsInstalled) reset tData->tlsCurrentTraceNode to the handler");
        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n (SetCurTraceNodeAfterExceptionIfContextIsInstalled) reset tData->tlsCurrentTraceNode to the handler");
#endif
    }



    inline VOID TakeLock() {
        do {
            while(GLOBAL_STATE.DSLock);
        } while(!__sync_bool_compare_and_swap(&GLOBAL_STATE.DSLock, 0, 1));
    }

    inline VOID ReleaseLock() {
        GLOBAL_STATE.DSLock = 0;
    }



// Pauses creator thread from thread creation until
// the previously created child thread has noted its parent.
    static inline void ThreadCreatePoint(THREADID threadId) {
        while(1) {
            TakeLock();

            if(GLOBAL_STATE.threadCreateCount > GLOBAL_STATE.threadCaptureCount)
                ReleaseLock();
            else
                break;
        }

        GLOBAL_STATE.threadCreatorTraceNode = CCTLibGetTLS(threadId)->tlsCurrentTraceNode;
        GLOBAL_STATE.threadCreatorIPNode = CCTLibGetTLS(threadId)->tlsCurrentIPNode;
        //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n ThreadCreatePoint, parent Trace = %p, parent ip = %p", GLOBAL_STATE.threadCreatorTraceNode, GLOBAL_STATE.threadCreatorIPNode);
        GLOBAL_STATE.threadCreateCount++;
        ReleaseLock();
    }


// Sets the child thread's CCT's parent to its creator thread's CCT node.
    static inline void ThreadCapturePoint(ThreadData* tdata) {
        TakeLock();

        if(GLOBAL_STATE.threadCreateCount == GLOBAL_STATE.threadCaptureCount) {
            // Base thread, no parent
            //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n ThreadCapturePoint, no parent ");
        } else {
            tdata->tlsParentThreadTraceNode = (TraceNode*) GLOBAL_STATE.threadCreatorTraceNode;
            tdata->tlsParentThreadIPNode = (IPNode*) GLOBAL_STATE.threadCreatorIPNode;
            //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n ThreadCapturePoint, parent Trace = %p, parent ip = %p", GLOBAL_STATE.threadCreatorTraceNode, GLOBAL_STATE.threadCreatorIPNode);
            GLOBAL_STATE.threadCaptureCount++;
        }

        ReleaseLock();
    }


    static inline IPNode* GetNextIPVecBuffer(uint32_t num) {
        uint64_t  oldBufIndex = __sync_fetch_and_add(&GLOBAL_STATE.curPreAllocatedContextBufferIndex, num);

        if(oldBufIndex + num  >= MAX_IPNODES) {
            //printf("\nPreallocated IPNodes exhausted. CCTLib couldn't fit your application in its memory. Try a smaller program.\n");
            fprintf(GLOBAL_STATE.CCTLibLogFile, "\nPreallocated IPNodes exhausted. CCTLib couldn't fit your application in its memory. Try a smaller program.\n");
            PIN_ExitProcess(-1);
        }

        return (GLOBAL_STATE.preAllocatedContextBuffer + oldBufIndex);
    }

    static inline uint32_t GetNextStringPoolIndex(char* name) {
        uint32_t len = strlen(name) + 1;
        uint64_t  oldStringPoolIndex = __sync_fetch_and_add(&GLOBAL_STATE.curPreAllocatedStringPoolIndex, len);

        if(oldStringPoolIndex + len  >= MAX_STRING_POOL_NODES) {
            fprintf(GLOBAL_STATE.CCTLibLogFile, "\nPreallocated String Pool exhausted. CCTLib couldn't fit your application in its memory. Try by changing MAX_STRING_POOL_NODES macro.\n");
            PIN_ExitProcess(-1);
        }

        // copy contents
        strncpy(GLOBAL_STATE.preAllocatedStringPool + oldStringPoolIndex, name, len);
        return oldStringPoolIndex;
    }

    static inline void CCTLibInitThreadData(ThreadData* const tdata, CONTEXT* ctxt, THREADID threadId) {
        TraceNode* t = new TraceNode();
        t->callerIPNode = 0;
        t->nSlots = 1;
        t->childIPs = GetNextIPVecBuffer(1);
        t->childIPs[0].parentTraceNode = t;
#ifdef USE_SPLAY_TREE
        t->childIPs[0].calleeTraceNodes = 0;
#else
        t->childIPs[0].calleeTraceNodes = new sparse_hash_map<ADDRINT, TraceNode*> ();
#endif
#ifdef HAVE_METRIC_PER_IPNODE
        t->childIPs[0].metric = NULL;
#endif
        tdata->tlsThreadId = threadId;
        tdata->tlsRootTraceNode = t;
        tdata->tlsRootIPNode = &(t->childIPs[0]);
        UpdateCurTraceAndIp(tdata, t);
        tdata->tlsParentThreadIPNode = 0;
        tdata->tlsParentThreadTraceNode = 0;
        tdata->tlsInitiatedCall = true;
        tdata->curSlotNo = 0;

        // Set stack sizes if data-centric is needed
        if(GLOBAL_STATE.doDataCentric) {
            ADDRINT s =  PIN_GetContextReg(ctxt, REG_STACK_PTR);
            tdata->tlsStackBase = (void*) s;
            struct rlimit rlim;

            if(getrlimit(RLIMIT_STACK, &rlim)) {
                cerr << "\n Failed to getrlimit()\n";
                PIN_ExitProcess(-1);
            }

            if(rlim.rlim_cur == RLIM_INFINITY) {
                cerr << "\n Need a finite stack size. Dont use unlimited.\n";
                PIN_ExitProcess(-1);
            }

            tdata->tlsStackEnd = (void*)(s - rlim.rlim_cur);
        }

#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        tdata->tlsMallocDSAccessStatus = END_READING;
#endif
    }

    static VOID CCTLibThreadStart(THREADID threadid, CONTEXT* ctxt, INT32 flags, VOID* v) {
        PIN_GetLock(&GLOBAL_STATE.lock, threadid + 1);
        GLOBAL_STATE.numThreads++;
        PIN_ReleaseLock(&GLOBAL_STATE.lock);
        ThreadData* tdata = new ThreadData();
        CCTLibInitThreadData(tdata, ctxt, threadid);
        PIN_SetThreadData(GLOBAL_STATE.CCTLibTlsKey, tdata, threadid);
        ThreadCapturePoint(tdata);
    }

// Analysis routine called on making a function call
    static inline VOID SetCallInitFlag(uint32_t slot, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->tlsInitiatedCall = true;
        tData->tlsCurrentIPNode = &(tData->tlsCurrentChildIPs[slot]);
#if 0
        ADDRINT* tracesIPs = (ADDRINT*)GLOBAL_STATE.traceShadowMap[tData->tlsCurrentTraceNode->traceKey];
        printf("\n Calling from IP = %p", tracesIPs[slot]);
#endif
    }

// Analysis routine called on function return.
// Point gCurrentContext to its parent, if we reach the root, set tlsInitiatedCall.
    static inline VOID GoUpCallChain(THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);

        // If we reach the root trace, then fake the call
        if(tData->tlsCurrentTraceNode->callerIPNode == tData->tlsRootIPNode) {
            tData->tlsInitiatedCall = true;
        }

        tData->tlsCurrentIPNode = tData->tlsCurrentTraceNode->callerIPNode;
        UpdateCurTraceOnly(tData, tData->tlsCurrentIPNode->parentTraceNode);
        // RET & CALL end a trace hence the target should trigger a new trace entry for us ... pray pray.
#if 0
        ADDRINT* tracesIPs = (ADDRINT*)GLOBAL_STATE.traceShadowMap[tData->tlsCurrentTraceNode->traceKey];
        int offset =  tData->tlsCurrentIPNode - tData->tlsCurrentTraceNode->childIPs;
        printf("\n Returning to the caller IP = %p", tracesIPs[offset]);
#endif
    }

// Analysis routine called interesting instructions to remember the slot no.
    static inline VOID RememberSlotNoInTLS(uint32_t slot, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->curSlotNo = slot;
    }


    static inline uint32_t GetNumInsInTrace(const TRACE& trace) {
        uint32_t count = 0;

        for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
            for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
                count++;
            }
        }

        return count;
    }

    static inline uint32_t GetNumInterestingInsInTrace(const TRACE& trace, IsInterestingInsFptr isInterestingIns) {
        uint32_t count = 0;

        for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
            for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
                // cal ret are always interesting for us :)
                if(IsCallOrRetIns(ins) || isInterestingIns(ins))
                    count++;
            }
        }

        return count;
    }

// Record the data about this image in a table
// Not written thread safe, but PIN guarantees that instrumentation functions are not called concurrently.
    static inline VOID CCTLibInstrumentImageLoad(IMG img, VOID* v) {
        UINT32 id = IMG_Id(img);
        ModuleInfo mi;
        mi.moduleName = IMG_Name(img);
        mi.imgLoadOffset = IMG_LoadOffset(img);
        GLOBAL_STATE.ModuleInfoMap[id] = mi;
    }


// Called each time a new trace is JITed.
// Given a trace this function adds instruction to each instruction in the trace.
// It also adds the trace to a hash table "GLOBAL_STATE.traceShadowMap" to maintain the reverse mapping from an (interesting) instruction's position in CCT back to its IP.
    static inline VOID PopulateIPReverseMapAndAccountTraceInstructions(TRACE trace, uint32_t traceKey, uint32_t numInterestingInstInTrace, IsInterestingInsFptr isInterestingIns) {
        // if there were 0 numInterestingInstInTrace, then let us simply return since it makes no sense to record anything about it.
        if(numInterestingInstInTrace == 0)
            return;

        ADDRINT* ipShadow = (ADDRINT*)malloc((2 + numInterestingInstInTrace) * sizeof(ADDRINT));     // +1 to hold the number of slots as a metadata and ++1 to hold module id
        // Record the number of instructions in the trace as the first entry
        ipShadow[0] = numInterestingInstInTrace;
        // Record the module id as 2nd entry
        ipShadow[1] = IMG_Id(IMG_FindByAddress(TRACE_Address(trace)));
        uint32_t slot = 0;
        GLOBAL_STATE.traceShadowMap[traceKey] = &ipShadow[2] ; // 0th entry is 2 behind

        for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
            for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
                // If it is a call/ret instruction, we need to adjust the CCT.
                // manage context
                if(INS_IsProcedureCall(ins)) {
                    // INS_InsertPredicatedCall if the call is not made, we should not set the flag
                    INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) SetCallInitFlag, IARG_UINT32, slot, IARG_THREAD_ID, IARG_END);

                    if(GLOBAL_STATE.userInstrumentationCallback) {
                        // Call user instrumentation passing the flag
                        if(isInterestingIns(ins))
                            GLOBAL_STATE.userInstrumentationCallback(ins, GLOBAL_STATE.userInstrumentationCallbackArg, slot);
                    } else {
                        // TLS will remember your slot no.
                        // TODO: should this be INS_InsertPredicatedCall? not sure. One can argue either ways.
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) RememberSlotNoInTLS, IARG_UINT32, slot, IARG_THREAD_ID, IARG_END);
                    }

                    // put next slot in corresponding ins start location;
                    ipShadow[slot + 2] = INS_Address(ins); // +2 because the first 2 entries hold metadata
                    slot++;
                } else if(INS_IsRet(ins)) {
                    // INS_InsertPredicatedCall if the RET is not made, we should not change CCT node
                    // CALL_ORDER_LAST because we want update context after all analysis routines for RET have executed.
                    INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) GoUpCallChain, IARG_CALL_ORDER, CALL_ORDER_LAST,  IARG_THREAD_ID, IARG_END);

                    if(GLOBAL_STATE.userInstrumentationCallback) {
                        // Call user instrumentation passing the flag
                        if(isInterestingIns(ins))
                            GLOBAL_STATE.userInstrumentationCallback(ins, GLOBAL_STATE.userInstrumentationCallbackArg, slot);
                    } else {
                        // TLS will remember your slot no.
                        // TODO: should this be INS_InsertPredicatedCall? not sure. One can argue either ways.
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) RememberSlotNoInTLS, IARG_UINT32, slot, IARG_THREAD_ID, IARG_END);
                    }

                    // put next slot in corresponding ins start location;
                    ipShadow[slot + 2] = INS_Address(ins); // +2 because the first 2 entries hold metadata
                    slot++;
                } else if(isInterestingIns(ins)) {
                    if(GLOBAL_STATE.userInstrumentationCallback) {
                        // Call user instrumentation passing the flag
                        GLOBAL_STATE.userInstrumentationCallback(ins, GLOBAL_STATE.userInstrumentationCallbackArg, slot);
                    } else {
                        // If, it is an interesting Ins, then we need to hold on to the slot number.
                        // TLS will remember your slot no.
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) RememberSlotNoInTLS, IARG_UINT32, slot, IARG_THREAD_ID, IARG_END);
                    }

                    // put next slot in corresponding ins start location;
                    ipShadow[slot + 2] = INS_Address(ins); // +2 because the first 2 entries hold metadata
                    slot++;
                } else {
                    // NOP
                }
            }
        }
    }

    static struct TraceSplay* splay(struct TraceSplay* root, ADDRINT ip) {
        REGULAR_SPLAY_TREE(TraceSplay, root, ip, key, left, right);
        return root;
    }


// Does necessary work on a trace entry (called during runtime)
// 1. If landed here due to function call, then go down in CCT.
// 2. Look up the current trace under the CCT node creating new if if needed.
// 3. Update iterators and curXXXX pointers.

    static inline void InstrumentTraceEntry(uint32_t traceKey, uint32_t numInterestingInstInTrace, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);

        // if landed here w/o a call instruction, then let's make this trace a sibling.
        // The trick to do it is to go to the parent TraceNode and make this trace a child of it
        if(!tData->tlsInitiatedCall) {
            tData->tlsCurrentIPNode = tData->tlsCurrentTraceNode->callerIPNode;
        } else {
            // tlsCurrentIPNode must be pointing to the call IP in the parent trace
            tData->tlsInitiatedCall = false;
        }

        // if the current trace is a child of currentIPNode, then let's set ourselves to that
#ifdef USE_SPLAY_TREE
        TraceSplay* found    = splay(tData->tlsCurrentIPNode->calleeTraceNodes, traceKey);

        // Check if a trace node with traceKey already exists under this context node
        if(found && (traceKey == found->key)) {
            tData->tlsCurrentIPNode->calleeTraceNodes = found;
            // already present, so set current trace to it
            UpdateCurTraceAndIp(tData, found->value);
        } else {
            // Create new trace node and insert under the IPNode.
            TraceNode* newChild = new TraceNode();
#if 0
            static uint64_t traceNodeCnt = 0;
            traceNodeCnt++;

            if((traceNodeCnt % 100000) == 0)
                printf("\n Trace traceNodeCnt=%lu", traceNodeCnt);

#endif
            newChild->callerIPNode = tData->tlsCurrentIPNode;
            newChild->traceKey = traceKey;

            if(numInterestingInstInTrace) {
                // if CONTINUOUS_DEADINFO is set, then all ip vecs come from a fixed 4GB buffer
                // might need a lock in MT case
                newChild->childIPs  = (IPNode*)GetNextIPVecBuffer(numInterestingInstInTrace);
                newChild->nSlots = numInterestingInstInTrace;

                //cerr<<"\n***:"<<numInterestingInstInTrace;
                for(uint32_t i = 0 ; i < numInterestingInstInTrace ; i++) {
                    newChild->childIPs[i].parentTraceNode = newChild;
                    newChild->childIPs[i].calleeTraceNodes = 0;
                }
            } else {
                // This can happen since we may hot a trace with 0 interesting instructions.
                //assert(0 && "I never expect traces to have 0 instructions");
                newChild->nSlots = 0;
                newChild->childIPs = 0;
            }

            TraceSplay* newNode = new TraceSplay();
            newNode->key = traceKey;
            newNode->value = newChild;
            tData->tlsCurrentIPNode->calleeTraceNodes = newNode;

            if(!found) {
                newNode->left = NULL;
                newNode->right = NULL;
            } else if(traceKey < found->key) {
                newNode->left = found->left;
                newNode->right = found;
                found->left = NULL;
            } else { // addr > addr of found
                newNode->left = found;
                newNode->right = found->right;
                found->right = NULL;
            }

            UpdateCurTraceAndIp(tData, newChild);
        }

#else

        // TODO: Delete me .. not maintianed

        // Check if a trace node with currentIp already exists under this context node
        if((tData->gTraceIter = (tData->tlsCurrentIPNode->calleeTraceNodes->find(currentIp))) != tData->tlsCurrentIPNode->calleeTraceNodes->end()) {
            // already present, so set current trace to it
            tData->tlsCurrentTraceNode = tData->gTraceIter->second;
            tData->tlsCurrentIPNode = tData->tlsCurrentTraceNode->childIPs;
        } else {
            // Create new trace node and insert under the IPNode.
            TraceNode* newChild = new TraceNode();
            newChild->callerIPNode = tData->tlsCurrentIPNode;
            newChild->address = currentIp;

            if(numInterestingInstInTrace) {
                // if CONTINUOUS_DEADINFO is set, then all ip vecs come from a fixed 4GB buffer
                // might need a lock in MT case
                newChild->childIPs  = (IPNode*)GetNextIPVecBuffer(numInterestingInstInTrace);
                newChild->nSlots = numInterestingInstInTrace;

                //cerr<<"\n***:"<<numInterestingInstInTrace;
                for(uint32_t i = 0 ; i < numInterestingInstInTrace ; i++) {
                    newChild->childIPs[i].parentTraceNode = newChild;
                    newChild->childIPs[i].calleeTraceNodes = new sparse_hash_map<ADDRINT, TraceNode*>();
                }
            } else {
                // This can happen since we may hot a trace with 0 interesting instructions.
                //assert(0 && "I never expect traces to have 0 instructions");
                newChild->nSlots = 0;
                newChild->childIPs = 0;
            }

            (*(tData->tlsCurrentIPNode->calleeTraceNodes))[currentIp] = newChild;
            tData->tlsCurrentTraceNode = newChild;
            tData->tlsCurrentIPNode = newChild->childIPs;
        }

#endif
    }


// Instrument a trace, take the first instruction in the first BBL and insert the analysis function before that
    static void CCTLibInstrumentTrace(TRACE trace, void*   isInterestingIns) {
        BBL bbl = TRACE_BblHead(trace);
        INS ins = BBL_InsHead(bbl);
        uint32_t numInterestingInstInTrace = GetNumInterestingInsInTrace(trace, (IsInterestingInsFptr)isInterestingIns);
        uint32_t traceKey = GetNextTraceKey();
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)InstrumentTraceEntry, IARG_UINT32, traceKey, IARG_UINT32, numInterestingInstInTrace, IARG_THREAD_ID, IARG_END);
        PopulateIPReverseMapAndAccountTraceInstructions(trace, traceKey, numInterestingInstInTrace, (IsInterestingInsFptr)isInterestingIns);
    }


    static void OnSig(THREADID threadId, CONTEXT_CHANGE_REASON reason, const CONTEXT* ctxtFrom,
                      CONTEXT* ctxtTo, INT32 sig, VOID* v) {
        ThreadData* tData = CCTLibGetTLS(threadId);

        switch(reason) {
        case CONTEXT_CHANGE_REASON_FATALSIGNAL:
            cerr << "\n FATAL SIGNAL";

        case CONTEXT_CHANGE_REASON_SIGNAL:
            //cerr<<"\n SIGNAL";
            // rest will be set as we enter the signal callee
            tData->tlsInitiatedCall = true;
            break;

        case CONTEXT_CHANGE_REASON_SIGRETURN: {
            // nothig  needs to be done! works like magic!!
            //cerr<<"\n SIG RET";
            //assert(0 && "NYI");
            break;
        }

        default:
            assert(0 && "\n BAD CONTEXT SWITCH");
        }
    }


    IPNode* GetPINCCTCurrentContext(THREADID id) {
        ThreadData* tData = CCTLibGetTLS(id);
        uint32_t slot = tData->curSlotNo;
        assert(slot < tData->tlsCurrentTraceNode->nSlots);
        return &(tData->tlsCurrentTraceNode->childIPs[slot]);
    }


    IPNode* GetPINCCTCurrentContextWithSlot(THREADID id, uint32_t slot) {
        ThreadData* tData = CCTLibGetTLS(id);
        assert(slot < tData->tlsCurrentTraceNode->nSlots);
        return &(tData->tlsCurrentTraceNode->childIPs[slot]);
    }

    ContextHandle_t GetContextHandle(const THREADID id, const uint32_t slot) {
        ThreadData* tData = CCTLibGetTLS(id);
        assert(slot < tData->tlsCurrentTraceNode->nSlots);
        return &(tData->tlsCurrentTraceNode->childIPs[slot]) - GLOBAL_STATE.preAllocatedContextBuffer;
    }

#ifdef HAVE_METRIC_PER_IPNODE
    void** GetIPNodeMetric(const THREADID id, const uint32_t slot) {
        ThreadData* tData = CCTLibGetTLS(id);
        assert(slot < tData->tlsCurrentTraceNode->nSlots);
        return &(tData->tlsCurrentTraceNode->childIPs[slot].metric);
    }
#endif

    uint32_t GetPINCCT32BitContextIndex(IPNode* node) {
        return node - GLOBAL_STATE.preAllocatedContextBuffer;
    }

    IPNode* GetPINCCTContextFrom32BitIndex(uint32_t index) {
        return  &GLOBAL_STATE.preAllocatedContextBuffer[index];
    }

    static void SegvHandler(int sig) {
        longjmp(GLOBAL_STATE.env, 1);
    }


// On program termination output all gathered data and statistics
    static VOID CCTLibFini(INT32 code, VOID* v) {
        // byte count
        //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n#eof");
        //fclose(GLOBAL_STATE.CCTLibLogFile);
    }

#if 0
// Visit all nodes of the splay tree of child traces.
    static void VisitAllNodesOfSplayTree(TraceSplay* node, FILE* const fp) {
        if(node == NULL)
            return;

        // visit left
        VisitAllNodesOfSplayTree(node->left, fp);
        // process self
        SerializeCCTNode(node->value, fp);
        // visit right
        VisitAllNodesOfSplayTree(node->right, fp);
    }

    static void SerializeCCTNode(TraceNode* traceNode, FILE* const fp) {
        // if traceNode had 0 interesting childIPs, then we are at a leaf trace so, we can simply return.
        if(traceNode->nSlots == 0)
            return;

        IPNode* parentIPNode = traceNode->callerIPNode ? traceNode->callerIPNode : 0;
        ADDRINT* traceIPs = (ADDRINT*)(GLOBAL_STATE.traceShadowMap[traceNode->traceKey]);
        ADDRINT moduleId = traceIPs[-1];
        ADDRINT loadOffset =   GLOBAL_STATE.ModuleInfoMap[moduleId].imgLoadOffset;

        // Iterate over all IPNodes in this trace
        for(uint32_t i = 0 ; i < traceNode->nSlots; i++) {
            fprintf(fp, "\n%p:%p:%p:%lu", &traceNode->childIPs[i], (void*)(traceIPs[i] - loadOffset), parentIPNode, moduleId);
        }

        // Iterate over all IPNodes
        for(uint32_t i = 0 ; i < traceNode->nSlots; i++) {
            // Iterate over all decendent TraceNode of traceNode->childIPs[i]
            VisitAllNodesOfSplayTree((traceNode->childIPs[i]).calleeTraceNodes, fp);
        }
    }

    static void SerializeAllCCTs() {
        for(uint32_t id = 0 ; id < GLOBAL_STATE.numThreads; id++) {
            ThreadData* tData = CCTLibGetTLS(id);
            std::stringstream cctMapFilePath;
            cctMapFilePath << GLOBAL_STATE.CCTLibFilePathPrefix << "-Thread" << id << "-CCTMap.txt";
            FILE* fp = fopen(cctMapFilePath.str().c_str(), "w");
            fprintf(fp, "NodeId:IP:ParentId:ModuleId");
            SerializeCCTNode(tData->tlsRootTraceNode, fp);
            fclose(fp);
        }
    }

#endif

// Visit all nodes of the splay tree of child traces.
    static void VisitAllNodesOfSplayTree(TraceSplay* node, FILE* const fp) {
        // process self
        SerializeCCTNode(node->value, fp);

        // visit left
        if(node->left)
            VisitAllNodesOfSplayTree(node->left, fp);

        // visit right
        if(node->right)
            VisitAllNodesOfSplayTree(node->right, fp);
    }

    static uint32_t NO_MORE_TRACE_NODES_IN_SPLAY_TREE = UINT_MAX;

    static void SerializeCCTNode(TraceNode* traceNode, FILE* const fp) {
        SerializedTraceNode serializedTraceNode = {traceNode->traceKey, traceNode->nSlots, GET_CONTEXT_HANDLE_FROM_IP_NODE(traceNode->childIPs) };
        fwrite(&serializedTraceNode, sizeof(SerializedTraceNode), 1, fp);

        // Iterate over all IPNodes
        for(uint32_t i = 0 ; i < traceNode->nSlots; i++) {
            if((traceNode->childIPs[i]).calleeTraceNodes == NULL) {
                fwrite(&NO_MORE_TRACE_NODES_IN_SPLAY_TREE, sizeof(NO_MORE_TRACE_NODES_IN_SPLAY_TREE), 1, fp);
            } else {
                // Iterate over all decendent TraceNode of traceNode->childIPs[i]
                VisitAllNodesOfSplayTree((traceNode->childIPs[i]).calleeTraceNodes, fp);
                fwrite(&NO_MORE_TRACE_NODES_IN_SPLAY_TREE, sizeof(NO_MORE_TRACE_NODES_IN_SPLAY_TREE), 1, fp);
            }
        }
    }

    static TraceNode* DeserializeCCTNode(IPNode* parentIPNode, FILE* const fp) {
        uint32_t noMoreTrace;

        if(fread(&noMoreTrace, sizeof(noMoreTrace), 1, fp) != 1) {
            fprintf(stderr, "\n Failed to read at line %d\n", __LINE__);
            PIN_ExitProcess(-1);
        }

        if(noMoreTrace == NO_MORE_TRACE_NODES_IN_SPLAY_TREE) {
            return NULL;
        }

        // go back 4 bytes;
        fseek(fp, -sizeof(noMoreTrace), SEEK_CUR);
        SerializedTraceNode serializedTraceNode;

        if(fread(&serializedTraceNode, sizeof(SerializedTraceNode), 1, fp) != 1) {
            fprintf(stderr, "\n Failed to read at line %d\n", __LINE__);
            PIN_ExitProcess(-1);
        }

        TraceNode* traceNode = new TraceNode();
        traceNode->traceKey = serializedTraceNode.traceKey;
        traceNode->nSlots = serializedTraceNode.nSlots;
        traceNode->childIPs = GET_IPNODE_FROM_CONTEXT_HANDLE(serializedTraceNode.childIPs);
        traceNode->callerIPNode = parentIPNode;

        // Iterate over all IPNodes
        for(uint32_t i = 0 ; i < traceNode->nSlots; i++) {
            traceNode->childIPs[i].parentTraceNode = traceNode;

            while(1) {
                TraceNode* childTrace =  DeserializeCCTNode(&traceNode->childIPs[i], fp);

                if(childTrace == NULL)
                    break;

                // add childTrace to the splay tree at traceNode->childIPs[i]
                TraceSplay* newNode = new TraceSplay();
                newNode->key = childTrace->traceKey;
                newNode->value = childTrace;

                // if no children
                if(traceNode->childIPs[i].calleeTraceNodes == NULL) {
                    traceNode->childIPs[i].calleeTraceNodes = newNode;
                    newNode->left = NULL;
                    newNode->right = NULL;
                } else {
                    TraceSplay* found    = splay(traceNode->childIPs[i].calleeTraceNodes, childTrace->traceKey);

                    if(childTrace->traceKey < found->key) {
                        newNode->left = found->left;
                        newNode->right = found;
                        found->left = NULL;
                    } else { // addr > addr of found
                        newNode->left = found;
                        newNode->right = found->right;
                        found->right = NULL;
                    }
                }
            }
        }

        return traceNode;
    }


    static void SerializeAllCCTs() {
        for(uint32_t id = 0 ; id < GLOBAL_STATE.numThreads; id++) {
            ThreadData* tData = CCTLibGetTLS(id);
            std::stringstream cctMapFilePath;
            cctMapFilePath << GLOBAL_STATE.serializationDirectory << SERIALIZED_CCT_FILE_PREFIX << id << SERIALIZED_CCT_FILE_SUFFIX;
            FILE* fp = fopen(cctMapFilePath.str().c_str(), "wb");

            if(fp == NULL) {
                fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", cctMapFilePath.str().c_str(), __LINE__);
                PIN_ExitProcess(-1);
            }

            //record thread id
            uint32_t threadId = tData->tlsThreadId;
            fwrite(&threadId, sizeof(tData->tlsThreadId), 1, fp);
            // record path of the parent
            ContextHandle_t parentIpHandle = GET_CONTEXT_HANDLE_FROM_IP_NODE(tData->tlsParentThreadIPNode);
            fwrite(&parentIpHandle, sizeof(ContextHandle_t), 1, fp);
            SerializeCCTNode(tData->tlsRootTraceNode, fp);
            fclose(fp);
        }
    }


// return the filenames of all files that have the specified extension
// in the specified directory and all subdirectories
    static void GetAllFilesInDirWithExtn(const boostFS::path& root, const string& ext, vector<boostFS::path>& ret) {
        if(!boostFS::exists(root)) return;

        if(boostFS::is_directory(root)) {
            boostFS::directory_iterator it(root);
            boostFS::directory_iterator endit;

            while(it != endit) {
                if(boostFS::is_regular_file(*it) && it->path().extension() == ext) {
                    ret.push_back(boostFS::system_complete(it->path()));
                }

                ++it;
            }
        }
    }

    static void DeserializeAllCCTs() {
        // Get all files with
        vector<boostFS::path> serializedCCTFiles;
        GetAllFilesInDirWithExtn(GLOBAL_STATE.serializationDirectory, SERIALIZED_CCT_FILE_EXTN, serializedCCTFiles);

        for(uint32_t id = 0 ; id < serializedCCTFiles.size(); id++) {
            std::stringstream cctMapFilePath;
            cctMapFilePath << serializedCCTFiles[id].native();
            //fprintf(stderr, "\nexists = %d\n",boostFS::exists(serializedCCTFiles[id]));
            FILE* fp = fopen(cctMapFilePath.str().c_str(), "rb");

            if(fp == NULL) {
                perror("fopen:");
                fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", cctMapFilePath.str().c_str(), __LINE__);
                PIN_ExitProcess(-1);
            }

            // Get thread id
            uint32_t threadId;

            if(fread(&threadId, sizeof(threadId), 1, fp) != 1) {
                fprintf(stderr, "\n Failed to read at line %d\n", __LINE__);
                PIN_ExitProcess(-1);
            }

            // record path of the parent
            ContextHandle_t parentIpHandle;

            if(fread(&parentIpHandle, sizeof(ContextHandle_t), 1, fp) != 1) {
                fprintf(stderr, "\n Failed to read at line %d\n", __LINE__);
                PIN_ExitProcess(-1);
            }

            TraceNode*  rootTrace = DeserializeCCTNode(GET_IPNODE_FROM_CONTEXT_HANDLE(parentIpHandle), fp);
#ifndef NDEBUG
            // we should be at the end of file now
            uint8_t dummy;
            assert(fread(&dummy, sizeof(uint8_t), 1, fp) == 0);
#endif
            fclose(fp);
            // Add a ThreadData record to GLOBAL_STATE.deserializedCCTs
            ThreadData tdata;
            //bzero(&tdata, sizeof(tdata));
            tdata.tlsThreadId = threadId;
            tdata.tlsParentThreadIPNode = GET_IPNODE_FROM_CONTEXT_HANDLE(parentIpHandle);
            tdata.tlsRootTraceNode = rootTrace;
            tdata.tlsRootIPNode = rootTrace->childIPs;
            GLOBAL_STATE.deserializedCCTs.push_back(tdata);
            // Update the number of threads
            GLOBAL_STATE.numThreads++;
        }
    }




    static void DottifyCCTNode(TraceNode* traceNode,  uint64_t curDotId, FILE* const fp);

    static uint64_t gDotId;
// Visit all nodes of the splay tree of child traces.
    static void DottifyAllNodesOfSplayTree(TraceSplay* node, uint64_t curDotId, FILE* const fp) {
        if(node == NULL)
            return;

        // visit left
        DottifyAllNodesOfSplayTree(node->left, curDotId, fp);
        // process self
        DottifyCCTNode(node->value, curDotId, fp);
        // visit right
        DottifyAllNodesOfSplayTree(node->right, curDotId, fp);
    }

// Visit all nodes of the splay tree of child traces.
    static void ListAllNodesOfSplayTree(TraceSplay* node, vector<TraceNode*>& childTraces) {
        if(node == NULL)
            return;

        // visit left
        ListAllNodesOfSplayTree(node->left, childTraces);
        childTraces.push_back(node->value);
        // visit right
        ListAllNodesOfSplayTree(node->right, childTraces);
    }


    static void DottifyCCTNode(TraceNode* traceNode,  uint64_t parentDotId, FILE* const fp) {
        // if traceNode had 0 interesting childIPs, then we are at a leaf trace so, we can simply return.
        if(traceNode->nSlots == 0) {
            return;
        }

        uint64_t myDotId = ++gDotId;
        fprintf(fp, "\"%" PRIx64 "\" -> \"%" PRIx64 "\";\n", parentDotId, myDotId);
        vector<TraceNode*> childTraces;

        // Iterate over all IPNodes
        for(uint32_t i = 0 ; i < traceNode->nSlots; i++) {
            // Iterate over all decendent TraceNode of traceNode->childIPs[i]
            //DottifyAllNodesOfSplayTree((traceNode->childIPs[i]).calleeTraceNodes, childTraceDotId, fp);
            ListAllNodesOfSplayTree((traceNode->childIPs[i]).calleeTraceNodes, childTraces);
        }

        for(vector<TraceNode*>::iterator it = childTraces.begin(); it != childTraces.end(); it++) {
            DottifyCCTNode(*it, myDotId, fp);
        }
    }


    void DottifyAllCCTs() {
        std::stringstream cctMapFilePath;
        cctMapFilePath << GLOBAL_STATE.serializationDirectory << "./CCTMap.dot";
        FILE* fp = fopen(cctMapFilePath.str().c_str(), "w");

        if(fp == NULL) {
            fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", cctMapFilePath.str().c_str(), __LINE__);
            PIN_ExitProcess(-1);
        }

        fprintf(fp, "digraph CCTLibGraph {\n");
        uint64_t dotId;

        for(uint32_t id = 0 ; id < GLOBAL_STATE.numThreads; id++) {
            ThreadData* tData = CCTLibGetTLS(id);
            DottifyCCTNode(tData->tlsRootTraceNode, gDotId, fp);
            dotId++;
        }

        fprintf(fp, "\n}");
        fclose(fp);
    }


#define SERIALIZED_MODULE_MAP_SUFFIX "/ModuleMap.txt"

    static void SerializeMouleInfo() {
        string moduleFilePath = GLOBAL_STATE.serializationDirectory + SERIALIZED_MODULE_MAP_SUFFIX;
        FILE* fp = fopen(moduleFilePath.c_str(), "w");

        if(fp == NULL) {
            perror("Error:");
            fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", moduleFilePath.c_str(), __LINE__);
            PIN_ExitProcess(-1);
        }

        unordered_map<UINT32, ModuleInfo>::iterator it;
        fprintf(fp, "ModuleId\tModuleFile\tLoadOffset");

        for(it = GLOBAL_STATE.ModuleInfoMap.begin(); it != GLOBAL_STATE.ModuleInfoMap.end(); ++it) {
            fprintf(fp, "\n%u\t%s\t%p", it->first, (it->second).moduleName.c_str(), (void*)((it->second).imgLoadOffset));
        }

        fclose(fp);
    }

    static void DeserializeMouleInfo() {
        string moduleFilePath = GLOBAL_STATE.serializationDirectory + SERIALIZED_MODULE_MAP_SUFFIX;
        FILE* fp = fopen(moduleFilePath.c_str(), "r");

        if(fp == NULL) {
            perror("Error");
            fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", moduleFilePath.c_str(), __LINE__);
            PIN_ExitProcess(-1);
        }

        // read header and thow it away
        uint32_t moduleId;
        ADDRINT offset;
        char path[MAX_FILE_PATH];
        //fprintf(fp, "ModuleId\tModuleFile\tLoadOffset");
        fscanf(fp, "%s%s%s", path, path, path);

        while(EOF != fscanf(fp, "%u%s%p", &moduleId, path, (void**)&offset)) {
            ModuleInfo minfo;
            minfo.moduleName = path;
            minfo.imgLoadOffset = offset;
            GLOBAL_STATE.ModuleInfoMap[moduleId] = minfo;
        }

        fclose(fp);
    }


    static void SerializeTraceIps() {
        string traceMapFilePath = GLOBAL_STATE.serializationDirectory + SERIALIZED_SHADOW_TRACE_IP_FILE_SUFFIX;
        FILE* fp = fopen(traceMapFilePath.c_str(), "wb");

        if(fp == NULL) {
            fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", traceMapFilePath.c_str(), __LINE__);
            PIN_ExitProcess(-1);
        }

        unordered_map<uint32_t, void*>::iterator it;
        //fprintf(fp, "TraceKey:NumSlots:ModuleId:[ip1][ip2]..[ipNumSlots]");

        for(it = GLOBAL_STATE.traceShadowMap.begin(); it != GLOBAL_STATE.traceShadowMap.end(); ++it) {
            // traceId
            fwrite(&(it->first), sizeof(it->first), 1, fp);
            ADDRINT* ptr = (ADDRINT*)(it->second);
            uint32_t moduleId = (uint32_t) ptr[-1];
            ADDRINT offset = GLOBAL_STATE.ModuleInfoMap[moduleId].imgLoadOffset;
            ADDRINT nSlots =  ptr[-2];

            // Normalize all IPs
            // NOTE --- once this update has happened, the traceShadowMapp[traceId] is rendered unusables without adding the offset in this run.
            // It must be used in post mortem analysis from this point onwards.
            for(ADDRINT i = 0; i < nSlots; i++) {
                ptr[i] = ptr[i] - offset;
            }

            // Write all slots
            fwrite(ptr - 2, sizeof(ADDRINT), ptr[-2] + 2 , fp);
        }

        fclose(fp);
    }


    static void DeserializeTraceIps() {
        string traceMapFilePath = GLOBAL_STATE.serializationDirectory + SERIALIZED_SHADOW_TRACE_IP_FILE_SUFFIX;
        FILE* fp = fopen(traceMapFilePath.c_str(), "rb");

        if(fp == NULL) {
            fprintf(stderr, "\n Failed to open %s in line %d. Exiting\n", traceMapFilePath.c_str(), __LINE__);
            PIN_ExitProcess(-1);
        }

        unordered_map<uint32_t, void*>::iterator it;
        //fprintf(fp, "TraceKey:NumSlots:ModuleId:[ip1][ip2]..[ipNumSlots]");
        uint32_t traceKey;

        while(fread(&traceKey, sizeof(traceKey), 1, fp) == 1) {
            // read num entries
            ADDRINT numSlots;

            if(fread(&numSlots, sizeof(ADDRINT), 1, fp) != 1) {
                fprintf(stderr, "\n Failed to read in line %d. Exiting\n", __LINE__);
                PIN_ExitProcess(-1);
            }

            // allocate the shadow ips
            ADDRINT* array = (ADDRINT*) malloc((numSlots + 2) * sizeof(ADDRINT));
            array[0] = numSlots;

            // read remaining entires
            if(fread(&array[1], sizeof(ADDRINT), numSlots + 1, fp) != (numSlots + 1)) {
                fprintf(stderr, "\n Failed to read in line %d. Exiting\n", __LINE__);
                PIN_ExitProcess(-1);
            }

            // Insert into the shadow map
            GLOBAL_STATE.traceShadowMap[traceKey] = (void*)(&array[2]);  // 2 because first 2 entries are behind as in runtime.
        }

        fclose(fp);
    }



    void SerializeMetadata(string directoryForSerializationFiles) {
        if(directoryForSerializationFiles != "") {
            GLOBAL_STATE.serializationDirectory = directoryForSerializationFiles;
        } else {
            // construct one
            std::stringstream ss;
            char hostname[MAX_FILE_PATH];
            gethostname(hostname, MAX_FILE_PATH);
            pid_t pid = getpid();
            ss << CCTLIB_SERIALIZATION_DEFAULT_DIR_NAME << hostname << "-" << pid;
            GLOBAL_STATE.serializationDirectory = ss.str();
        }

        // create directory
        string cmd = "mkdir -p " + GLOBAL_STATE.serializationDirectory;
        int result = system(cmd.c_str());

        if(result != 0) {
            fprintf(stderr, "\n failed to call system()");
        }

        SerializeAllCCTs();
        SerializeMouleInfo();
        SerializeTraceIps();
    }

    void DeserializeMetadata(string directoryForSerializationFiles) {
        GLOBAL_STATE.serializationDirectory = directoryForSerializationFiles;
        DeserializeAllCCTs();
        DeserializeTraceIps();
        DeserializeMouleInfo();
    }

    /**
     * Returns the peak (maximum so far) resident set size (physical
     * memory use) measured in KB, or zero if the value cannot be
     * determined on this OS.
     */
    size_t getPeakRSS() {
        struct rusage rusage;
        getrusage(RUSAGE_SELF, &rusage);
        return (size_t)(rusage.ru_maxrss);
    }


    static void PrintStats() {
        fprintf(GLOBAL_STATE.CCTLibLogFile, "\nTotal call paths=%" PRIu64, GLOBAL_STATE.curPreAllocatedContextBufferIndex);
        // Peak resource usage
        fprintf(GLOBAL_STATE.CCTLibLogFile, "\nPeak RSS=%zu", getPeakRSS());
    }


// This function is called when the application exits
    VOID Fini(INT32 code, VOID* v) {
        //SerializeMetadata();
        //DottifyAllCCTs();
        PrintStats();
    }

// Given a pointer (i.e. slot) within a trace node, returns the IP corresponding to that slot
    static inline ADDRINT GetIPFromInfo(IPNode* ipNode) {
        TraceNode* traceNode = ipNode->parentTraceNode;
        // what is my slot id ?
        uint32_t slotNo = 0;

        for(; slotNo < traceNode->nSlots; slotNo++) {
            if(&traceNode->childIPs[slotNo] == ipNode)
                break;
        }

        ADDRINT* ip = (ADDRINT*) GLOBAL_STATE.traceShadowMap[traceNode->traceKey] ;
        return ip[slotNo];
    }


// Given a pointer (i.e. slot) within a trace node, returns the module name corresponding to that slot
    static inline const string& GetModulePathFromInfo(IPNode* ipNode) {
        TraceNode* traceNode = ipNode->parentTraceNode;
        ADDRINT* ptr = (ADDRINT*) GLOBAL_STATE.traceShadowMap[traceNode->traceKey] ;
        UINT32 moduleId = ptr[-1]; // module id is stored one behind.
        return GLOBAL_STATE.ModuleInfoMap[moduleId].moduleName;
    }


// Given a pointer (i.e. slot) within a trace node, returns the Line number corresponding to that slot
    static inline void GetLineFromInfo(const ADDRINT& ip, uint32_t& lineNo, string& filePath) {
        PIN_GetSourceLocation(ip, NULL, (INT32*) &lineNo, &filePath);
    }

    static void GetDecodedInstFromIP(ADDRINT ip) {
        // Get the instruction in a string
        xed_decoded_inst_t      xedd;
        GLOBAL_STATE.disassemblyBuff[0] = 0;
        xed_decoded_inst_zero_set_mode(&xedd, &GLOBAL_STATE.cct_xed_state);

        if(XED_ERROR_NONE == xed_decode(&xedd, (const xed_uint8_t*)(ip), 15)) {
            if(0 == xed_format_context(XED_SYNTAX_ATT, &xedd, GLOBAL_STATE.disassemblyBuff, 200,  ip, 0, 0))
                strcpy(GLOBAL_STATE.disassemblyBuff , "xed_decoded_inst_dump_att_format failed");
        } else {
            strcpy(GLOBAL_STATE.disassemblyBuff , "xed_decode failed");
        }
    }

// Returns true if the given address belongs to one of the loaded binaries
    static inline bool IsValidIP(ADDRINT ip) {
        for(IMG img = APP_ImgHead(); IMG_Valid(img); img = IMG_Next(img)) {
            if(ip >= IMG_LowAddress(img) && ip <= IMG_HighAddress(img)) {
                return true;
            }
        }

        return false;
    }
#if 0
// Returns true if the given deadinfo belongs to one of the loaded binaries
    static inline bool IsValidIP(DeadInfo  di) {
        bool res = false;

        for(IMG img = APP_ImgHead(); IMG_Valid(img); img = IMG_Next(img)) {
            if((ADDRINT)di.firstIP >= IMG_LowAddress(img) && (ADDRINT)di.firstIP <= IMG_HighAddress(img)) {
                res = true;
                break;
            }
        }

        if(!res)
            return false;

        for(IMG img = APP_ImgHead(); IMG_Valid(img); img = IMG_Next(img)) {
            if((ADDRINT)di.secondIP >= IMG_LowAddress(img) && (ADDRINT)di.secondIP <= IMG_HighAddress(img)) {
                return true;
            }
        }

        return false;
    }
#endif

// Returns true if the address in the given context node corresponds to a sinature (assembly code: ) that corresponds to a .PLT section
// Sample PLt signatire : ff 25 c2 24 21 00       jmpq   *2172098(%rip)        # 614340 <quoting_style_args+0x2a0>
    static inline bool IsValidPLTSignature(const ADDRINT& ip) {
        if((*((unsigned char*)ip) == 0xff) && (*((unsigned char*)ip + 1) == 0x25))
            return true;

        return false;
    }


#define NOT_ROOT_CTX (-1)
// Return true if the given ContextNode is one of the root context nodes
    static int IsARootIPNode(IPNode* curIPNode) {
        // if it is runing monitoring we will use CCTLibGetTLS
        if(GLOBAL_STATE.cctLibUsageMode == CCT_LIB_MODE_COLLECTION) {
            for(uint32_t id = 0 ; id < GLOBAL_STATE.numThreads; id++) {
                ThreadData* tData = CCTLibGetTLS(id);

                if(tData->tlsRootIPNode == curIPNode)
                    return id;
            }
        } else {
            //CCT_LIB_MODE_POSTMORTEM
            for(uint32_t id = 0 ; id < GLOBAL_STATE.numThreads; id++) {
                if(GLOBAL_STATE.deserializedCCTs[id].tlsRootIPNode == curIPNode)
                    return GLOBAL_STATE.deserializedCCTs[id].tlsThreadId;
            }
        }

        return NOT_ROOT_CTX;
    }




#if 0

    static VOID PrintFullCallingContext(IPNode* curIPNode);
// Given a context node (curContext), traverses up in the chain till the root and prints the entire calling context

    static VOID PrintFullCallingContext(IPNode* curIPNode) {
        int depth = 0;
#ifdef MULTI_THREADED
        int root;
#endif         //end MULTI_THREADED
        // set sig handler
        struct sigaction old;
        sigaction(SIGSEGV, &GLOBAL_STATE.sigAct, &old);

        // Dont print if the depth is more than MAX_CCT_PRINT_DEPTH since files become too large
        while(curIPNode && (depth ++ < MAX_CCT_PRINT_DEPTH)) {
            int threadCtx = 0;

            if((threadCtx = IsARootIPNode(curIPNode)) != NOT_ROOT_CTX) {
                fprintf(GLOBAL_STATE.CCTLibLogFile, "\nTHREAD[%d]_ROOT_CTXT", threadCtx);
                // if the thread has a parent, recurse over it.
                IPNode* parentThreadIPNode = CCTLibGetTLS(threadCtx)->tlsParentThreadIPNode;

                if(parentThreadIPNode)
                    PrintFullCallingContext(parentThreadIPNode);

                break;
            } else {
                ADDRINT ip = GetIPFromInfo(curIPNode);

                if(IsValidIP(ip)) {
                    if(PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE) == ".plt") {
                        if(setjmp(GLOBAL_STATE.env) == 0) {
                            if(IsValidPLTSignature(ip)) {
                                uint64_t nextByte = (uint64_t) ip + 2;
                                int* offset = (int*) nextByte;
                                uint64_t nextInst = (uint64_t) ip + 6;
                                ADDRINT loc = *((uint64_t*)(nextInst + *offset));

                                if(IsValidIP(loc)) {
                                    string filePath;
                                    uint32_t lineNo;
                                    GetLineFromInfo(ip, lineNo, filePath);
                                    GetDecodedInstFromIP(ip);
                                    fprintf(GLOBAL_STATE.CCTLibLogFile, "\n%u:!%p:%s:%s:%s:%u", GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode), (void*)ip, GLOBAL_STATE.disassemblyBuff, PIN_UndecorateSymbolName(RTN_FindNameByAddress(loc), UNDECORATION_COMPLETE).c_str(), filePath.c_str(), lineNo);
                                } else {
                                    fprintf(GLOBAL_STATE.CCTLibLogFile, "\nIN PLT BUT NOT VALID GOT");
                                }
                            } else {
                                fprintf(GLOBAL_STATE.CCTLibLogFile, "\nUNRECOGNIZED PLT SIGNATURE");
                                //fprintf(GLOBAL_STATE.CCTLibLogFile,"\n plt plt plt %x", * ((UINT32*)curContext->address));
                                //for(int i = 1; i < 4 ; i++)
                                //	fprintf(GLOBAL_STATE.CCTLibLogFile," %x",  ((UINT32 *)curContext->address)[i]);
                            }
                        } else {
                            fprintf(GLOBAL_STATE.CCTLibLogFile, "\nCRASHED !!");
                        }
                    } else {
                        string filePath;
                        uint32_t lineNo;
                        GetLineFromInfo(ip, lineNo, filePath);
                        GetDecodedInstFromIP(ip);
#if 0
                        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n%p:%s:%s:%s", (void*)ip, GLOBAL_STATE.disassemblyBuff, PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE).c_str(), line.c_str());
#else
                        // also print the IPNode handle so that I can debug deserialization
                        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n%u:%p:%s:%s:%s:%u", GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode), (void*)ip, GLOBAL_STATE.disassemblyBuff, PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE).c_str(), filePath.c_str(), lineNo);
#endif
                    }
                } else {
                    fprintf(GLOBAL_STATE.CCTLibLogFile, "\nBAD IP ");
                }

                curIPNode = curIPNode->parentTraceNode->callerIPNode;
            }
        }

        //reset sig handler
        sigaction(SIGSEGV, &old, 0);
    }
#endif

    static VOID GetFullCallingContext(IPNode* curIPNode, vector<Context>& contextVec);

    VOID GetFullCallingContext(const ContextHandle_t ctxtHandle, vector<Context>& contextVec) {
        GetFullCallingContext(GLOBAL_STATE.preAllocatedContextBuffer + ctxtHandle, contextVec);
    }

    static VOID GetFullCallingContextInSitu(IPNode* curIPNode, vector<Context>& contextVec) {
        int depth = 0;
#ifdef MULTI_THREADED
        int root;
#endif         //end MULTI_THREADED
        // set sig handler
        struct sigaction old;
        sigaction(SIGSEGV, &GLOBAL_STATE.sigAct, &old);

        // Dont print if the depth is more than MAX_CCT_PRINT_DEPTH since files become too large
        while(curIPNode && (depth ++ < MAX_CCT_PRINT_DEPTH)) {
            int threadCtx = 0;

            if((threadCtx = IsARootIPNode(curIPNode)) != NOT_ROOT_CTX) {
                // if the thread has a parent, recur over it.
                IPNode* parentThreadIPNode = CCTLibGetTLS(threadCtx)->tlsParentThreadIPNode;
                Context ctxt = {"THREAD[" +  boost::lexical_cast<std::string>(threadCtx) + "]_ROOT_CTXT" /*functionName*/, "" /*filePath */, "" /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, 0 /*ip*/};
                contextVec.push_back(ctxt);

                if(parentThreadIPNode)
                    GetFullCallingContextInSitu(parentThreadIPNode, contextVec);

                break;
            } else {
                ADDRINT ip = GetIPFromInfo(curIPNode);

                if(IsValidIP(ip)) {
                    if(PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE) == ".plt") {
                        if(setjmp(GLOBAL_STATE.env) == 0) {
                            if(IsValidPLTSignature(ip)) {
                                uint64_t nextByte = (uint64_t) ip + 2;
                                int* offset = (int*) nextByte;
                                uint64_t nextInst = (uint64_t) ip + 6;
                                ADDRINT loc = *((uint64_t*)(nextInst + *offset));

                                if(IsValidIP(loc)) {
                                    string filePath;
                                    uint32_t lineNo;
                                    GetLineFromInfo(ip, lineNo, filePath);
                                    GetDecodedInstFromIP(ip);
                                    Context ctxt = {PIN_UndecorateSymbolName(RTN_FindNameByAddress(loc), UNDECORATION_COMPLETE)  /*functionName*/, filePath/*filePath */, string(GLOBAL_STATE.disassemblyBuff) /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, lineNo /*lineNo*/, ip /*ip*/};
                                    contextVec.push_back(ctxt);
                                } else {
                                    GetDecodedInstFromIP(ip);
                                    Context ctxt = {"IN PLT BUT NOT VALID GOT"  /*functionName*/, ""/*filePath */, string(GLOBAL_STATE.disassemblyBuff) /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, ip /*ip*/};
                                    contextVec.push_back(ctxt);
                                }
                            } else {
                                GetDecodedInstFromIP(ip);
                                Context ctxt = {"UNRECOGNIZED PLT SIGNATURE"  /*functionName*/, ""/*filePath */, string(GLOBAL_STATE.disassemblyBuff) /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, ip /*ip*/};
                                contextVec.push_back(ctxt);
                                //fprintf(GLOBAL_STATE.CCTLibLogFile,"\n plt plt plt %x", * ((UINT32*)curContext->address));
                                //for(int i = 1; i < 4 ; i++)
                                //      fprintf(GLOBAL_STATE.CCTLibLogFile," %x",  ((UINT32 *)curContext->address)[i]);
                            }
                        } else {
                            Context ctxt = {"CRASHED !!"  /*functionName*/, ""/*filePath */, "" /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, ip /*ip*/};
                            contextVec.push_back(ctxt);
                        }
                    } else {
                        string filePath;
                        uint32_t lineNo;
                        GetLineFromInfo(ip, lineNo, filePath);
                        GetDecodedInstFromIP(ip);
#if 0
                        fprintf(GLOBAL_STATE.CCTLibLogFile, "\n%p:%s:%s:%s", (void*)ip, GLOBAL_STATE.disassemblyBuff, PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE).c_str(), li
                                ne.c_str());
#else
                        // also print the IPNode handle so that I can debug deserialization
                        Context ctxt = {PIN_UndecorateSymbolName(RTN_FindNameByAddress(ip), UNDECORATION_COMPLETE)  /*functionName*/, filePath/*filePath */, string(GLOBAL_STATE.disassemblyBuff) /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, lineNo /*lineNo*/, ip /*ip*/};
                        contextVec.push_back(ctxt);
#endif
                    }
                } else {
                    Context ctxt = {"BAD IP !!"  /*functionName*/, ""/*filePath */, "" /*disassembly*/, GET_CONTEXT_HANDLE_FROM_IP_NODE(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, ip /*ip*/};
                    contextVec.push_back(ctxt);
                }

                curIPNode = curIPNode->parentTraceNode->callerIPNode;
            }
        }

        //reset sig handler
        sigaction(SIGSEGV, &old, 0);
    }



    static VOID GetFullCallingContextPostmortem(IPNode* curIPNode, vector<Context>& contextVec) {
        int depth = 0;
#ifdef MULTI_THREADED
        int root;
#endif         //end MULTI_THREADED
        // set sig handler
        struct sigaction old;
        sigaction(SIGSEGV, &GLOBAL_STATE.sigAct, &old);

        // Dont print if the depth is more than MAX_CCT_PRINT_DEPTH since files become too large
        while(curIPNode && (depth ++ < MAX_CCT_PATH_DEPTH)) {
            int threadCtx = 0;

            if((threadCtx = IsARootIPNode(curIPNode)) != NOT_ROOT_CTX) {
                Context ctxt = {"THREAD[" +  boost::lexical_cast<std::string>(threadCtx) + "]_ROOT_CTXT" /*functionName*/, "" /*filePath */, "" /*disassembly*/, GetPINCCT32BitContextIndex(curIPNode) /*ctxtHandle*/, 0 /*lineNo*/, 0 /*ip*/};
                contextVec.push_back(ctxt);
                // if the thread has a parent, recurse over it.
                IPNode* parentThreadIPNode = GLOBAL_STATE.deserializedCCTs[threadCtx].tlsParentThreadIPNode;

                if(parentThreadIPNode)
                    GetFullCallingContextPostmortem(parentThreadIPNode, contextVec);

                break;
            } else {
                ADDRINT ip = GetIPFromInfo(curIPNode);
                const string& modulePath = GetModulePathFromInfo(curIPNode);
                std::stringstream command;
                command << "addr2line -C -f -e " << modulePath << " " << std::hex << ip;
                FILE* fp = popen(command.str().c_str(), "r");
                char functionName[MAX_FILE_PATH];
                char fileName[MAX_FILE_PATH];
                uint32_t lineNo;

                if(setjmp(GLOBAL_STATE.env) == 0) {
                    if(fgets(functionName, MAX_FILE_PATH, fp) == NULL) {
                        strcpy(functionName, "FAILED_TO_READ");
                        strcpy(fileName, "FAILED_TO_READ");
                        lineNo = 0;
                    } else {
                        if(fgets(fileName, MAX_FILE_PATH, fp) == NULL) {
                            strcpy(fileName, "FAILED_TO_READ");
                            lineNo = 0;
                        } else {
                            // Look for last ":"
                            int len = strlen(fileName);
                            int linePos;

                            for(linePos = len - 1; linePos >= 0 ; linePos --) {
                                if(fileName[linePos] == ':')
                                    break;
                            }

                            lineNo = atoi(&fileName[linePos + 1]);
                            fileName[linePos] = '\0';
                        }
                    }
                } else {
                    strcpy(functionName, "CRASHED!");
                    strcpy(fileName, "CRASHED!");
                    lineNo = 0;
                }

                pclose(fp);
                string fnName(functionName);
                boost::algorithm::trim(fnName);
                string flName(fileName);
                boost::algorithm::trim(flName);
                Context ctxt = {fnName/*functionName*/, flName /*filePath */, "TODO-Disassmebly" /*disassembly*/, GetPINCCT32BitContextIndex(curIPNode) /*ctx
tHandle*/, lineNo /*lineNo*/, ip /*ip*/
                               };
                contextVec.push_back(ctxt);
                curIPNode = curIPNode->parentTraceNode->callerIPNode;
            }
        }

        //reset sig handler
        sigaction(SIGSEGV, &old, 0);
    }


    static VOID GetFullCallingContext(IPNode* curIPNode, vector<Context>& contextVec) {
        if(GLOBAL_STATE.cctLibUsageMode == CCT_LIB_MODE_POSTMORTEM)
            GetFullCallingContextPostmortem(curIPNode, contextVec);
        else
            GetFullCallingContextInSitu(curIPNode, contextVec);
    }

    VOID PrintFullCallingContext(const ContextHandle_t ctxtHandle) {
        vector<Context> contextVec;

        if(GLOBAL_STATE.cctLibUsageMode == CCT_LIB_MODE_POSTMORTEM)
            GetFullCallingContextPostmortem(GLOBAL_STATE.preAllocatedContextBuffer + ctxtHandle, contextVec);
        else
            GetFullCallingContextInSitu(GLOBAL_STATE.preAllocatedContextBuffer + ctxtHandle, contextVec);

        for(uint32_t i = 0 ; i < contextVec.size(); i++) {
            fprintf(GLOBAL_STATE.CCTLibLogFile, "\n%u:%p:%s:%s:%s:%u", contextVec[i].ctxtHandle, (void*) contextVec[i].ip, contextVec[i].disassembly.c_str(), contextVec[i].functionName.c_str(), contextVec[i].filePath.c_str(), contextVec[i].lineNo);
        }
    }


// Initialize the needed data structures before launching the target program
    static void InitBuffers() {
        // prealloc IPNodeVec so that they all come from a continuous memory region.
        // IMPROVEME ... actually this can be as high as 24 GB since lower 3 bits are always zero for pointers
        GLOBAL_STATE.preAllocatedContextBuffer = (IPNode*) mmap(0, MAX_IPNODES * sizeof(IPNode), PROT_WRITE
                | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // start from index 1 so that we can use 0 as empty key for the google hash table
        GLOBAL_STATE.curPreAllocatedContextBufferIndex = 1;
        // Init the string pool
        GLOBAL_STATE.preAllocatedStringPool = (char*) mmap(0, MAX_STRING_POOL_NODES * sizeof(char), PROT_WRITE
                                              | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // start from index 1 so that we can use 0 as a special value
        GLOBAL_STATE.curPreAllocatedStringPoolIndex = 1;
    }


#if 0
// Initialize RW locks
    static void InitLocks() {
//        PIN_RWMutexInit(&gStaticVarRWLock);
//        PIN_RWMutexInit(&gMallocVarRWLock);
    }
#endif

    static void InitLogFile(FILE* logFile) {
        GLOBAL_STATE.CCTLibLogFile = logFile;
    }

    static void InitMapFilePrefix() {
        char* envPath = getenv("OUTPUT_FILE");

        if(envPath) {
            // assumes max of MAX_FILE_PATH
            GLOBAL_STATE.CCTLibFilePathPrefix = string(envPath) + "-";
        }

        std::stringstream ss;
        char hostname[MAX_FILE_PATH];
        gethostname(hostname, MAX_FILE_PATH);
        pid_t pid = getpid();
        ss << hostname << "-" << pid;
        GLOBAL_STATE.CCTLibFilePathPrefix += ss.str();
    }


    static void InitSegHandler() {
        // Init the  segv handler that may happen (due to PIN bug) when unwinding the stack during the printing
        memset(&GLOBAL_STATE.sigAct, 0, sizeof(struct sigaction));
        GLOBAL_STATE.sigAct.sa_handler = SegvHandler;
        GLOBAL_STATE.sigAct.sa_flags = SA_NODEFER;
    }

    static void InitXED() {
        // Init XED for decoding instructions
        xed_state_init(&GLOBAL_STATE.cct_xed_state, XED_MACHINE_MODE_LONG_64, (xed_address_width_enum_t) 0, XED_ADDRESS_WIDTH_64b);
        // removed from  Xed v.67254 xed_decode_init();
    }


//DO_DATA_CENTRIC

#ifdef USE_SHADOW_FOR_DATA_CENTRIC

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

#define LEVEL_1_PAGE_TABLE_SLOT(addr) ((((uint64_t)addr) >> (LEVEL_2_PAGE_TABLE_BITS + PAGE_OFFSET_BITS)) & 0xfffff)
#define LEVEL_2_PAGE_TABLE_SLOT(addr) ((((uint64_t)addr) >> (PAGE_OFFSET_BITS)) & 0xFFF)

#define SHADOW_STRUCT_SIZE (sizeof (T))

//uint8_t ** gL1PageTable[LEVEL_1_PAGE_TABLE_SIZE];
    uint8_t*** gL1PageTable;

    volatile bool gShadowPageLock;
    inline VOID TakeLock(volatile bool* myLock) {
        do {
            while(*myLock);
        } while(!__sync_bool_compare_and_swap(myLock, 0, 1));
    }

    inline VOID ReleaseLock(volatile bool* myLock) {
        *myLock = 0;
    }



// Given a address generated by the program, returns the corresponding shadow address FLOORED to  PAGE_SIZE
// If the shadow page does not exist a new one is MMAPed

    template <class T>
    inline T* GetOrCreateShadowBaseAddress(void const* const address) {
        T* shadowPage;
        uint8_t**  * l1Ptr = &gL1PageTable[LEVEL_1_PAGE_TABLE_SLOT(address)];

        if(*l1Ptr == 0) {
            TakeLock(&gShadowPageLock);

            // If some other thread created L2 page table in the meantime, then let's not do the same.
            if(*l1Ptr == 0) {
                *l1Ptr = (uint8_t**) calloc(1, LEVEL_2_PAGE_TABLE_SIZE);
            }

            // If some other thread created the same shadow page in the meantime, then let's not do the same.
            if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0) {
                (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] = (uint8_t*) mmap(0, PAGE_SIZE * SHADOW_STRUCT_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            }

            ReleaseLock(&gShadowPageLock);
        } else if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0) {
            TakeLock(&gShadowPageLock);

            // If some other thread created the same shadow page in the meantime, then let's not do the same.
            if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0) {
                (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] = (uint8_t*) mmap(0, PAGE_SIZE * SHADOW_STRUCT_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            }

            ReleaseLock(&gShadowPageLock);
        }

        shadowPage = (T*)((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]);
        return shadowPage;
    }

    template <class T>
    inline T* GetOrCreateShadowAddress(void* address) {
        T* shadowPage = GetOrCreateShadowBaseAddress<T>(address);
        return shadowPage + PAGE_OFFSET((uint64_t)address);
    }


    static void InitShadowSpaceForDataCentric(VOID* addr, uint32_t accessLen, DataHandle_t* initializer) {
        uint64_t endAddr = (uint64_t)addr + accessLen;
        uint32_t numInited = 0;

        for(uint64_t curAddr = (uint64_t)addr; curAddr < endAddr; curAddr += PAGE_SIZE) {
            DataHandle_t* status = GetOrCreateShadowAddress<DataHandle_t>((void*)curAddr);
            int maxBytesInThisPage  = PAGE_SIZE - PAGE_OFFSET((uint64_t)addr);

            for(int i = 0 ; (i < maxBytesInThisPage) && numInited < accessLen; numInited++, i++) {
                status[i] = *initializer;
            }
        }
    }

    DataHandle_t GetDataObjectHandle(VOID* addr, THREADID threadId) {
        DataHandle_t dataHandle;
        ThreadData* tData = CCTLibGetTLS(threadId);

        // if it is a stack location, set so and return
        if(addr > tData->tlsStackEnd && addr <  tData->tlsStackBase) {
            dataHandle.objectType = STACK_OBJECT;
            return dataHandle;
        }

        dataHandle = *GetOrCreateShadowAddress<DataHandle_t>(addr);
        return dataHandle;
    }

#elif defined(USE_TREE_BASED_FOR_DATA_CENTRIC)
    DataHandle_t GetDataObjectHandle(VOID* addr, THREADID threadId) {
        DataHandle_t record;
        ThreadData* tData = CCTLibGetTLS(threadId);

        // if it is a stack location, set so and return
        if(addr > tData->tlsStackEnd && addr <  tData->tlsStackBase) {
            record.objectType = STACK_OBJECT;
            return record;
        }

        // publish the most recent MallocVarSet that this thread sees. This allows the concurrent writer to make progress.
        // This is placed here so that we dont update this on each stack access. We favor reader progress.
        tData->tlsLatestConcurrentTree = (ConcurrentReaderWriterTree_t*) GLOBAL_STATE.latestConcurrentTree;
        varSet* curTree = & (tData->tlsLatestConcurrentTree->tree);
        tData->tlsMallocDSAccessStatus = START_READING;
        // first check dymanically allocated variables
        //ReadLock mallocRWlock(gMallocVarRWLock);
        // gMallocVarRWLock.lock_read();
        // ReadLock(&(tData->rwLockStatus));
        varSet::iterator node = curTree->lower_bound(varType(addr, addr, 0 /*handle*/, UNKNOWN_OBJECT));

        if(node != curTree->begin() && node->start != addr) node --;

        if(node != curTree->end() && (addr < node->start || addr >= node->end))
            node = curTree->end();

        if(node != curTree->end()) {
            record.objectType = node->objectType;
            record.pathHandle = node->pathHandle;
// Milind - Commented this since thsi consumes too much space.
// Need to discuss with Xu on how to maintain this efficiently.
//            record.beg_addr = (uint64_t)node->start;
// 	    record.end_addr = (uint64_t)node->end;
        } else {
            record.objectType = UNKNOWN_OBJECT;
        }

        tData->tlsMallocDSAccessStatus = END_READING;
        return record;
    }


#else
    DataHandle_t GetDataObjectHandle(VOID* addr, THREADID threadId) {
        assert(0 && "should never reach here");
    }

#endif

    static VOID CaptureMallocSize(size_t arg0, THREADID threadId) {
        // Remember the CCT node and the allocation size
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->tlsDynamicMemoryAllocationSize = arg0;
        tData->tlsDynamicMemoryAllocationPathHandle = GetContextHandle(threadId, 0);
    }

    static VOID CaptureCallocSize(size_t arg0, size_t arg1, THREADID threadId) {
        // Remember the CCT node and the allocation size
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->tlsDynamicMemoryAllocationSize = arg0 * arg1;
        tData->tlsDynamicMemoryAllocationPathHandle = GetContextHandle(threadId, 0);
    }

//Fwd declaration;
    static void CaptureFree(void* ptr, THREADID threadId);

    static VOID CaptureReallocSize(void* ptr, size_t arg1, THREADID threadId) {
        // Remember the CCT node and the allocation size
        ThreadData* tData = CCTLibGetTLS(threadId);
        tData->tlsDynamicMemoryAllocationSize = arg1;
        tData->tlsDynamicMemoryAllocationPathHandle = GetContextHandle(threadId, 0);
#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        // Simulate free(ptr);
        CaptureFree(ptr, threadId);
#endif
    }


#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC

#if 0
    static void DUMP_STATUS(THREADID threadId, int stuckon, int calledFrom) {
        printf("\n STUCK! threadId = %d, stack waiting for %d, called from %s", threadId, stuckon, calledFrom == 0 ? "malloc" : "free");

        for(uint32_t i = 0; i < GLOBAL_STATE.numThreads; i++) {
            ThreadData* tData = CCTLibGetTLS(i);
            printf("\n Thread %d,  tlsLatestMallocVarSet = %p, tlsMallocDSAccessStatus = %d", i, tData->tlsLatestMallocVarSet , tData->tlsMallocDSAccessStatus);
        }

        fflush(stdout);
    }

    static void WaitTillAllThreadsProgressIntoNewSet(varSet* latestVarSet, THREADID threadId, int calledFrom) {
        // TODO: if the thread exists in the mean time, it might not have seen the update. We need to ignore such threads.
        for(uint32_t i = 0; i < GLOBAL_STATE.numThreads; i++) {
            ThreadData* tData = CCTLibGetTLS(i);
            uint64_t j = 0;

            while((tData->tlsLatestMallocVarSet != latestVarSet) && (tData->tlsMallocDSAccessStatus == START_READING)) {
                // spin
                if(j++ == 0xffffff) {
                    DUMP_STATUS(threadId, i, calledFrom);
                }
            }
        }
    }
#else

    QNode* volatile MCSLock = NULL;

    static void MCSAcquire(QNode* volatile* L, QNode* I) {
        I->next = NULL;
        I->status = LOCKED;
        QNode*   pred = (QNode*) __sync_lock_test_and_set((uint64_t*)L, I);

        if(pred) {
            pred->next = I;

            while(I->status == LOCKED) ; // spin
        }
    }

    static void MCSRelease(QNode* volatile* L, QNode* I, uint8_t releaseVal) {
        if(I->next == NULL) {
            if(__sync_bool_compare_and_swap((uint64_t*) L, I, NULL))
                return;

            while(I->next == NULL) ; // spin

            // wait till some successor
        }

        I->next->status = releaseVal;
    }


    static void WaitTillAllThreadsProgressIntoNewSet(ConcurrentReaderWriterTree_t* latestTree) {
        // TODO: if the thread exists in the mean time, it might not have seen the update. We need to ignore such threads.
        for(uint32_t i = 0; i < GLOBAL_STATE.numThreads; i++) {
            ThreadData* tData = CCTLibGetTLS(i);

            while((tData->tlsLatestConcurrentTree != latestTree) && (tData->tlsMallocDSAccessStatus == START_READING)) {
                // spin
            }
        }
    }

    static void ApplyPendingOperationsToTree(ConcurrentReaderWriterTree_t* threadFreeTree) {
        for(uint32_t i = 0 ; i < threadFreeTree->pendingOps.size(); i++) {
            switch(threadFreeTree->pendingOps[i].operation) {
            case INSERT: {
                //fprintf(stderr,"\n Inserting %1d %p - %p",threadFreeTree->pendingOps[i].var.objectType, threadFreeTree->pendingOps[i].var.start, threadFreeTree->pendingOps[i].var.end);
                threadFreeTree->tree.insert(threadFreeTree->pendingOps[i].var);
                break;
            }

            case DELETE: {
                //fprintf(stderr,"\n Deleting %1d %p\n", threadFreeTree->pendingOps[i].var.objectType, threadFreeTree->pendingOps[i].var.start);
                varSet::const_iterator iterThreadFreeTree = threadFreeTree->tree.lower_bound(threadFreeTree->pendingOps[i].var);

                //assert(iterThreadFreeTree != threadFreeTree->tree.end());
                //assert(threadFreeTree->pendingOps[i].var.start >= iterThreadFreeTree->start);
                //assert(threadFreeTree->pendingOps[i].var.start < iterThreadFreeTree->end);
                if(threadFreeTree->pendingOps[i].var.start == iterThreadFreeTree->start) {
                    threadFreeTree->tree.erase(*iterThreadFreeTree);
                } else {
                    fprintf(stderr, "\n Can't delete %1d %p\n", threadFreeTree->pendingOps[i].var.objectType, threadFreeTree->pendingOps[i].var.start);
                }

                break;
            }

            default:
                assert(0 && "Should not reach here");
                break;
            }
        }

        //clear list
        threadFreeTree->pendingOps.clear();
    }
#endif


    static void UpdateLockLessTree(THREADID threadId, const vector<PendingOps_t>& ops) {
        static ThreadData dummyThreadData; // we use it when GLOBAL_STATE.applicationStarted is false
        ThreadData* tData;

        if(GLOBAL_STATE.applicationStarted == false) {
            tData = &dummyThreadData;
        } else {
            tData = CCTLibGetTLS(threadId);
        }

        // tell that this thread is waiting to write
        tData->tlsMallocDSAccessStatus = WAITING_WRITE;
        QNode mcsNode;
        MCSAcquire(&MCSLock, &mcsNode);
        // Not waiting anymore
        tData->tlsMallocDSAccessStatus = WRITE_STARTED;
        ConcurrentReaderWriterTree_t*   curTree = (ConcurrentReaderWriterTree_t*) GLOBAL_STATE.latestConcurrentTree;
        ConcurrentReaderWriterTree_t* newTree = (curTree == (&GLOBAL_STATE.concurrentReaderWriterTree[0])) ? (&GLOBAL_STATE.concurrentReaderWriterTree[1])  : (&GLOBAL_STATE.concurrentReaderWriterTree[0]);
        // Queue up "ops" to both trees
        curTree->pendingOps.insert(curTree->pendingOps.end(), ops.begin(), ops.end());
        newTree->pendingOps.insert(newTree->pendingOps.end(), ops.begin(), ops.end());
#if 0

        // Write batching optimization is disabled since it can cause writer to not find its inserted item immediately if it becomes a reader.
        if(mcsNode.status != UNLOCKED_AND_PREDECESSOR_WAS_WRITER /* first writer*/) {
            // Wait for all threads to make progress into curTree
            WaitTillAllThreadsProgressIntoNewSet(curTree);
        }

#else
        // Wait for all threads to make progress into curTree
        WaitTillAllThreadsProgressIntoNewSet(curTree);
#endif
        // All threads will be in curTree, so we can modify newTree
        // Apply pending operations to newTree
        ApplyPendingOperationsToTree(newTree);
#if 0

        // Write batching optimization is disabled since it can cause writer to not find its inserted item immediately if it becomes a reader.
        if(mcsNode.next == NULL  /* last writer */) {
            // Publish newTree.
            GLOBAL_STATE.latestConcurrentTree = newTree;
            // set self tlsLatestMallocVarSet to be newTree
            tData->tlsLatestConcurrentTree = newTree;
            MCSRelease(&MCSLock, &mcsNode, UNLOCKED);
        } else {
            MCSRelease(&MCSLock, &mcsNode, UNLOCKED_AND_PREDECESSOR_WAS_WRITER);
        }

#else
        // Publish newTree.
        GLOBAL_STATE.latestConcurrentTree = newTree;
        // set self tlsLatestMallocVarSet to be newTree
        tData->tlsLatestConcurrentTree = newTree;
        MCSRelease(&MCSLock, &mcsNode, UNLOCKED);
#endif
    }


#endif


    static VOID CaptureMallocPointer(void* ptr, THREADID threadId) {
        ThreadData* tData = CCTLibGetTLS(threadId);
#ifdef USE_SHADOW_FOR_DATA_CENTRIC
        DataHandle_t dataHandle;
        dataHandle.objectType = DYNAMIC_OBJECT;
        dataHandle.pathHandle = tData->tlsDynamicMemoryAllocationPathHandle;
        InitShadowSpaceForDataCentric(ptr, tData->tlsDynamicMemoryAllocationSize, &dataHandle);
#elif defined(USE_TREE_BASED_FOR_DATA_CENTRIC)
        varType v(ptr, (void*)((char*)ptr + (tData->tlsDynamicMemoryAllocationSize)), tData->tlsDynamicMemoryAllocationPathHandle, DYNAMIC_OBJECT);
        vector<PendingOps_t> ops(1, PendingOps_t(INSERT, v));
        UpdateLockLessTree(threadId, ops);
#else
        assert(0 && "Should not reach here");
#endif
    }

    static void CaptureFree(void* ptr, THREADID threadId) {
#ifdef USE_SHADOW_FOR_DATA_CENTRIC
        //NOP
#elif defined(USE_TREE_BASED_FOR_DATA_CENTRIC)

        if(ptr) {
            varType v(ptr, ptr, 0 /* handle */, DYNAMIC_OBJECT);
            vector<PendingOps_t> ops(1, PendingOps_t(DELETE, v));
            UpdateLockLessTree(threadId, ops);
        } // else NOP .. free() does nothing for NULL ptr

#else
        assert(0 && "Should not reach here");
#endif
    }


// compute static variables
// each image has a splay tree to include all static variables
// that reside in the image. All images are linked as a link list

    static void compute_static_var(char* filename, IMG img) {
#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        vector<PendingOps_t> ops;
        UINT32 imgId = IMG_Id(img);
#endif
        //Elf32_Ehdr* elf_header;         /* ELF header */
        Elf* elf;                       /* Our Elf pointer for libelf */
        Elf_Scn* scn = NULL;                   /* Section Descriptor */
        Elf_Data* edata = NULL;                /* Data Descriptor */
        GElf_Sym sym;                   /* Symbol */
        GElf_Shdr shdr;                 /* Section Header */
        char* base_ptr;         // ptr to our object in memory
        struct stat elf_stats;  // fstat struct
        int i, symbol_count;
        int fd = open(filename, O_RDONLY);

        if((fstat(fd, &elf_stats))) {
            printf("bss: could not fstat, so not monitor static variables\n");
            close(fd);
            return;
        }

        if((base_ptr = (char*) malloc(elf_stats.st_size)) == NULL) {
            printf("could not malloc\n");
            close(fd);
            PIN_ExitProcess(-1);
        }

        if((read(fd, base_ptr, elf_stats.st_size)) < elf_stats.st_size) {
            printf("could not read\n");
            free(base_ptr);
            close(fd);
            PIN_ExitProcess(-1);
        }

        if(elf_version(EV_CURRENT) == EV_NONE) {
            printf("WARNING Elf Library is out of date!\n");
        }

        //elf_header = (Elf32_Ehdr*) base_ptr;    // point elf_header at our object in memory
        elf = elf_begin(fd, ELF_C_READ, NULL);  // Initialize 'elf' pointer to our file descriptor

        // Iterate each section until symtab section for object symbols
        while((scn = elf_nextscn(elf, scn)) != NULL) {
            gelf_getshdr(scn, &shdr);

            if(shdr.sh_type == SHT_SYMTAB) {
                edata = elf_getdata(scn, edata);
                symbol_count = shdr.sh_size / shdr.sh_entsize;

                for(i = 0; i < symbol_count; i++) {
                    if(gelf_getsym(edata, i, &sym) == NULL) {
                        printf("gelf_getsym return NULL\n");
                        printf("%s\n", elf_errmsg(elf_errno()));
                        PIN_ExitProcess(-1);
                    }

                    if((sym.st_size == 0) || (ELF32_ST_TYPE(sym.st_info) != STT_OBJECT)) { //not a variable
                        continue;
                    }

#ifdef USE_SHADOW_FOR_DATA_CENTRIC
                    DataHandle_t dataHandle;
                    dataHandle.objectType = STATIC_OBJECT;
                    char* symname = elf_strptr(elf, shdr.sh_link, sym.st_name);
                    dataHandle.symName = symname ? GetNextStringPoolIndex(symname) : 0;
                    InitShadowSpaceForDataCentric((void*)((IMG_LoadOffset(img)) + sym.st_value), (uint32_t)sym.st_size, &dataHandle);
#elif defined(USE_TREE_BASED_FOR_DATA_CENTRIC)
                    char* symname = elf_strptr(elf, shdr.sh_link, sym.st_name);
                    uint32_t handle = symname ? GetNextStringPoolIndex(symname) : 0;
                    varType vInsert((void*)((IMG_LoadOffset(img)) + sym.st_value), (void*)((IMG_LoadOffset(img)) + sym.st_value + sym.st_size), handle, STATIC_OBJECT);
                    varType vDelete((void*)((IMG_LoadOffset(img)) + sym.st_value), (void*)((IMG_LoadOffset(img)) + sym.st_value), handle, STATIC_OBJECT);
                    ops.push_back(PendingOps_t(INSERT, vInsert));
                    // record for later deletion
                    GLOBAL_STATE.staticVariablesInModule[imgId].push_back(PendingOps_t(INSERT, vDelete));
#else
                    assert(0 && "Should not reach here");
#endif
                }
            }
        }

#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        UpdateLockLessTree(PIN_ThreadId(), ops);
#endif
    }

    static VOID
    DeleteStaticVar(IMG img, VOID* v) {
#ifdef USE_SHADOW_FOR_DATA_CENTRIC
        //NOP
#else
        UpdateLockLessTree(PIN_ThreadId(), GLOBAL_STATE.staticVariablesInModule[IMG_Id(img)]);
#endif
    }

    static VOID ComputeVarBounds(IMG img, VOID* v) {
        char filename[PATH_MAX];
        char* result = realpath(IMG_Name(img).c_str(), filename);

        if(result == NULL) {
            fprintf(stderr, "\n failed to resolve path");
        }

        compute_static_var(filename, img);
    }

// end DO_DATA_CENTRIC #endif

    VOID CCTLibImage(IMG img, VOID* v) {
        //  Find the pthread_create() function.
#define PTHREAD_CREATE_RTN "pthread_create"
#define ARCH_LONGJMP_RTN "__longjmp"
#define SETJMP_RTN "_setjmp"
//#define LONGJMP_RTN "longjmp"
#define LONGJMP_RTN ARCH_LONGJMP_RTN
#define SIGSETJMP_RTN "sigsetjmp"
//#define SIGLONGJMP_RTN "siglongjmp"
#define SIGLONGJMP_RTN ARCH_LONGJMP_RTN
#define UNWIND_SETIP "_Unwind_SetIP"
#define UNWIND_RAISEEXCEPTION "_Unwind_RaiseException"
#define UNWIND_RESUME "_Unwind_Resume"
#define UNWIND_FORCEUNWIND "_Unwind_ForcedUnwind"
#define UNWIND_RESUME_OR_RETHROW "_Unwind_Resume_or_Rethrow"
        RTN pthread_createRtn = RTN_FindByName(img, PTHREAD_CREATE_RTN);
        RTN setjmpRtn = RTN_FindByName(img, SETJMP_RTN);
        RTN longjmpRtn = RTN_FindByName(img, LONGJMP_RTN);
        RTN sigsetjmpRtn = RTN_FindByName(img, SIGSETJMP_RTN);
        RTN siglongjmpRtn = RTN_FindByName(img, SIGLONGJMP_RTN);
        RTN archlongjmpRtn = RTN_FindByName(img, ARCH_LONGJMP_RTN);
        RTN unwindSetIpRtn = RTN_FindByName(img, UNWIND_SETIP);
        RTN unwindRaiseExceptionRtn = RTN_FindByName(img, UNWIND_RAISEEXCEPTION);
        RTN unwindResumeRtn = RTN_FindByName(img, UNWIND_RESUME);
        RTN unwindForceUnwindRtn = RTN_FindByName(img, UNWIND_FORCEUNWIND);
        //RTN unwindResumeOrRethrowRtn = RTN_FindByName(img, UNWIND_RESUME_OR_RETHROW);
#if 0
        cout << "\n Image name" << IMG_Name(img);
#endif

        if(RTN_Valid(pthread_createRtn)) {
            //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",PTHREAD_CREATE_RTN);
            RTN_Open(pthread_createRtn);
            // Instrument malloc() to print the input argument value and the return value.
            RTN_InsertCall(pthread_createRtn, IPOINT_AFTER, (AFUNPTR)ThreadCreatePoint, IARG_THREAD_ID, IARG_END);
            RTN_Close(pthread_createRtn);
        }

#if 0

        if(RTN_Valid(setjmpRtn)) {
            //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",SETJMP_RTN);
            RTN_ReplaceSignature(setjmpRtn, AFUNPTR(SetJmpOverride),  IARG_CONST_CONTEXT, IARG_THREAD_ID, IARG_ORIG_FUNCPTR, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
        }

#endif

        // Look for setjmp and longjmp routines present in libc.so.x file only
        if(strstr(IMG_Name(img).c_str(), "libc.so")) {
            if(RTN_Valid(setjmpRtn)) {
                //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",SETJMP_RTN);
                RTN_Open(setjmpRtn);
                RTN_InsertCall(setjmpRtn, IPOINT_BEFORE, (AFUNPTR)CaptureSigSetJmpCtxt, IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(setjmpRtn);
            }

            if(RTN_Valid(longjmpRtn)) {
                //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",LONGJMP_RTN);
                RTN_Open(longjmpRtn);
                RTN_InsertCall(longjmpRtn, IPOINT_BEFORE, (AFUNPTR)HoldLongJmpBuf, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(longjmpRtn);
            }

            if(RTN_Valid(sigsetjmpRtn)) {
                //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",SIGSETJMP_RTN);
                RTN_Open(sigsetjmpRtn);
                //CALL_ORDER_LAST so that cctlib's trace level instrumentation has updated the tlsCurrentIPNode
                RTN_InsertCall(sigsetjmpRtn, IPOINT_BEFORE, (AFUNPTR)CaptureSigSetJmpCtxt, IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(sigsetjmpRtn);
            }

            if(RTN_Valid(siglongjmpRtn)) {
                //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",SIGLONGJMP_RTN);
                RTN_Open(siglongjmpRtn);
                RTN_InsertCall(siglongjmpRtn, IPOINT_BEFORE, (AFUNPTR)HoldLongJmpBuf, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(siglongjmpRtn);
            }

            if(RTN_Valid(archlongjmpRtn)) {
                //fprintf(GLOBAL_STATE.CCTLibLogFile, "\n Found RTN %s",ARCH_LONGJMP_RTN);
                RTN_Open(archlongjmpRtn);
                // Insert after the last JMP Inst.
                INS lastIns = RTN_InsTail(archlongjmpRtn);
                assert(INS_Valid(lastIns));
                assert(INS_IsBranch(lastIns));
                assert(!INS_IsDirectBranch(lastIns));
                INS_InsertCall(lastIns, IPOINT_TAKEN_BRANCH, (AFUNPTR) RestoreSigLongJmpCtxt,  IARG_THREAD_ID, IARG_END);
                //RTN_InsertCall(siglongjmpRtn, IPOINT_BEFORE, (AFUNPTR)RestoreSigLongJmpCtxt, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(archlongjmpRtn);
            }
        }

//#if DISABLE_EXCEPTION_HANDLING
#if 1

        // Look for unwinding related routines present in libc.so.x file only
        if(strstr(IMG_Name(img).c_str(), "libgcc_s.so")) {
            if(RTN_Valid(unwindSetIpRtn)) {
#ifdef DEBUG_CCTLIB
                fprintf(GLOBAL_STATE.CCTLibLogFile, "\n %s found in %s", UNWIND_SETIP, IMG_Name(img).c_str());
#endif
                RTN_Open(unwindSetIpRtn);
                // Get the intended target IP and prepare the call stack to be ready to unwind to that level
                RTN_InsertCall(unwindSetIpRtn, IPOINT_BEFORE, (AFUNPTR)CaptureCallerThatCanHandleException, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
#if 0

                // We should conditionally enable this only for SjLj style exceptions which overwrite RA in _Unwind_SetIP
                // After every return instruction in this function, call SetCurTraceNodeAfterException
                for(INS i = RTN_InsHead(unwindSetIpRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;

                    INS_InsertCall(i, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterException,  IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_THREAD_ID, IARG_END);
                }

#endif
                // I don;t think there is a need to do this as the last instruction unlike RestoreSigLongJmpCtxt.
                // Since _Unwind_SetIP implementations employ a technique of overwriting the return address to jump to the
                // exception handler, calls made by _Unwind_SetIP if any will not cause any problem even if we rewire the call path before executing the return.
                RTN_Close(unwindSetIpRtn);
            }

            if(RTN_Valid(unwindResumeRtn)) {
#ifdef DEBUG_CCTLIB
                fprintf(GLOBAL_STATE.CCTLibLogFile, "\n %s found in %s", UNWIND_RESUME, IMG_Name(img).c_str());
#endif
                RTN_Open(unwindResumeRtn);

                // *** THIS ROUTINE NEVER RETURNS ****
                // After every return instruction in this function, call SetCurTraceNodeAfterException
                for(INS i = RTN_InsHead(unwindResumeRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;

                    // CALL_ORDER_LAST+10 because CALL_ORDER_LAST is reserved for  GoUpCallChain that is executed on each RET instruction. We need to adjust the context after GoUpCallChain has executed.
                    INS_InsertCall(i, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterException,  IARG_CALL_ORDER, CALL_ORDER_LAST + 10, IARG_THREAD_ID, IARG_END);
                    //INS_InsertCall(i, IPOINT_TAKEN_BRANCH, (AFUNPTR) SetCurTraceNodeAfterException, IARG_THREAD_ID, IARG_END);
                }

                RTN_Close(unwindResumeRtn);
            }

#if 1

            if(RTN_Valid(unwindRaiseExceptionRtn)) {
#ifdef DEBUG_CCTLIB
                fprintf(GLOBAL_STATE.CCTLibLogFile, "\n %s found in %s", UNWIND_RAISEEXCEPTION, IMG_Name(img).c_str());
#endif
                RTN_Open(unwindRaiseExceptionRtn);
                // After the last return instruction in this function, call SetCurTraceNodeAfterExceptionIfContextIsInstalled
                INS  lastIns = INS_Invalid();

                for(INS i = RTN_InsHead(unwindRaiseExceptionRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;
                    else
                        lastIns = i;
                }

                if(lastIns != INS_Invalid()) {
                    // CALL_ORDER_LAST+10 because CALL_ORDER_LAST is reserved for  GoUpCallChain that is executed on each RET instruction. We need to adjust the context after GoUpCallChain has executed.
                    INS_InsertCall(lastIns, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled,  IARG_CALL_ORDER, CALL_ORDER_LAST + 10, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                    //INS_InsertCall(lastIns, IPOINT_TAKEN_BRANCH, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                } else {
                    //assert(0 && "did not find the last return in unwindRaiseExceptionRtn");
                    //printf("\n did not find the last return in unwindRaiseExceptionRtn");
                    fprintf(GLOBAL_STATE.CCTLibLogFile, "\n did not find the last return in unwindRaiseExceptionRtn");
                }

                RTN_Close(unwindRaiseExceptionRtn);
            }

            if(RTN_Valid(unwindForceUnwindRtn)) {
#ifdef DEBUG_CCTLIB
                fprintf(GLOBAL_STATE.CCTLibLogFile, "\n %s found in %s", UNWIND_FORCEUNWIND, IMG_Name(img).c_str());
#endif
                RTN_Open(unwindForceUnwindRtn);
                // After the last return instruction in this function, call SetCurTraceNodeAfterExceptionIfContextIsInstalled
                INS  lastIns = INS_Invalid();

                for(INS i = RTN_InsHead(unwindForceUnwindRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;
                    else
                        lastIns = i;
                }

                if(lastIns != INS_Invalid()) {
                    // CALL_ORDER_LAST+10 because CALL_ORDER_LAST is reserved for  GoUpCallChain that is executed on each RET instruction. We need to adjust the context after GoUpCallChain has executed.
                    INS_InsertCall(lastIns, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled,  IARG_CALL_ORDER, CALL_ORDER_LAST + 10, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                    //INS_InsertCall(lastIns, IPOINT_TAKEN_BRANCH, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                } else {
                    // TODO : This function _Unwind_ForcedUnwind also appears in /lib64/libpthread.so.0. in which case, we should ignore it.
                    //assert(0 && "did not find the last return in unwindForceUnwindRtn");
                    //printf("\n did not find the last return in unwindForceUnwindRtn");
                    fprintf(GLOBAL_STATE.CCTLibLogFile, "\n did not find the last return in unwindForceUnwindRtn");
                }

                RTN_Close(unwindForceUnwindRtn);
            }

#else

            if(RTN_Valid(unwindRaiseExceptionRtn)) {
                RTN_Open(unwindRaiseExceptionRtn);

                // After the last return instruction in this function, call SetCurTraceNodeAfterExceptionIfContextIsInstalled
                for(INS i = RTN_InsHead(unwindRaiseExceptionRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;
                    else {
                        // CALL_ORDER_LAST+10 because CALL_ORDER_LAST is reserved for  GoUpCallChain that is executed on each RET instruction. We need to adjust the context after GoUpCallChain has executed.
                        INS_InsertCall(i, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled,  IARG_CALL_ORDER, CALL_ORDER_LAST + 10, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                        //INS_InsertCall(i, IPOINT_TAKEN_BRANCH, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                    }
                }

                RTN_Close(unwindRaiseExceptionRtn);
            }

            if(RTN_Valid(unwindForceUnwindRtn)) {
                RTN_Open(unwindForceUnwindRtn);

                // After the last return instruction in this function, call SetCurTraceNodeAfterExceptionIfContextIsInstalled
                for(INS i = RTN_InsHead(unwindForceUnwindRtn); INS_Valid(i); i = INS_Next(i)) {
                    if(!INS_IsRet(i))
                        continue;
                    else {
                        // CALL_ORDER_LAST+10 because CALL_ORDER_LAST is reserved for  GoUpCallChain that is executed on each RET instruction. We need to adjust the context after GoUpCallChain has executed.
                        INS_InsertCall(i, IPOINT_BEFORE, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled,  IARG_CALL_ORDER, CALL_ORDER_LAST + 10, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                        //INS_InsertCall(i, IPOINT_TAKEN_BRANCH, (AFUNPTR) SetCurTraceNodeAfterExceptionIfContextIsInstalled, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                    }
                }

                RTN_Close(unwindForceUnwindRtn);
            }

#endif
        } // end strstr

#endif
        //end DISABLE_EXCEPTION_HANDLING

        // For new DW2 exception handling, we need to reset the shadow stack to the current handler in the following functions:
        // 1. _Unwind_Reason_Code _Unwind_RaiseException ( struct _Unwind_Exception *exception_object );
        // 2. _Unwind_Reason_Code _Unwind_ForcedUnwind ( struct _Unwind_Exception *exception_object, _Unwind_Stop_Fn stop, void *stop_parameter );
        // 3. void _Unwind_Resume (struct _Unwind_Exception *exception_object); *** INSTALL UNCONDITIONALLY, SINCE THIS NEVER RETURNS ***
        // 4. _Unwind_Reason_Code LIBGCC2_UNWIND_ATTRIBUTE _Unwind_Resume_or_Rethrow (struct _Unwind_Exception *exc) *** I AM NOT IMPLEMENTING THIS UNTILL I HIT A CODE THAT NEEDS IT ***

        // These functions call "uw_install_context" at the end of the routine just before returning, which overwrite the return address.
        // uw_install_context itself is a static function inlined or macroed. So we would rely on the more externally visible functions.
        // There are multiple returns in these (_Unwind_RaiseException, _Unwind_ForcedUnwind, _Unwind_Resume_or_Rethrow) functions. Only if the return value is "_URC_INSTALL_CONTEXT" shall we reset the shadow stack.

        // if data centric is enabled, capture allocation routines
        if(GLOBAL_STATE.doDataCentric) {
            RTN mallocRtn = RTN_FindByName(img, MALLOC_FN_NAME);

            if(RTN_Valid(mallocRtn)) {
                RTN_Open(mallocRtn);
                // Capture the allocation size and CCT node
                RTN_InsertCall(mallocRtn, IPOINT_BEFORE, (AFUNPTR) CaptureMallocSize, IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                // capture the allocated pointer and initialize the memory with CCT node.
                RTN_InsertCall(mallocRtn, IPOINT_AFTER, (AFUNPTR) CaptureMallocPointer, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                RTN_Close(mallocRtn);
            }

            RTN callocRtn = RTN_FindByName(img, CALLOC_FN_NAME);

            if(RTN_Valid(callocRtn)) {
                RTN_Open(callocRtn);
                // Capture the allocation size and CCT node
                RTN_InsertCall(callocRtn, IPOINT_BEFORE, (AFUNPTR) CaptureCallocSize, IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_THREAD_ID, IARG_END);
                // capture the allocated pointer and initialize the memory with CCT node.
                RTN_InsertCall(callocRtn, IPOINT_AFTER, (AFUNPTR) CaptureMallocPointer, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                RTN_Close(callocRtn);
            }

            RTN reallocRtn = RTN_FindByName(img, REALLOC_FN_NAME);

            if(RTN_Valid(reallocRtn)) {
                RTN_Open(reallocRtn);
                // Capture the allocation size and CCT node
                RTN_InsertCall(reallocRtn, IPOINT_BEFORE, (AFUNPTR) CaptureReallocSize, IARG_CALL_ORDER, CALL_ORDER_LAST, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_THREAD_ID, IARG_END);
                // capture the allocated pointer and initialize the memory with CCT node.
                RTN_InsertCall(reallocRtn, IPOINT_AFTER, (AFUNPTR) CaptureMallocPointer, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
                RTN_Close(reallocRtn);
            }

            RTN freeRtn = RTN_FindByName(img, FREE_FN_NAME);

            if(RTN_Valid(freeRtn)) {
                RTN_Open(freeRtn);
                RTN_InsertCall(freeRtn, IPOINT_BEFORE, (AFUNPTR) CaptureFree, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_THREAD_ID, IARG_END);
                RTN_Close(freeRtn);
            }
        }
    }


//DO_DATA_CENTRIC
    static void InitDataCentric() {
        // For shadow memory based approach initialize the L1 page table LEVEL_1_PAGE_TABLE_SIZE
#ifdef USE_SHADOW_FOR_DATA_CENTRIC
        gL1PageTable = (uint8_t***) mmap(0, LEVEL_1_PAGE_TABLE_SIZE * sizeof(uint8_t***), PROT_WRITE
                                         | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
#endif // end USE_SHADOW_FOR_DATA_CENTRIC
        // This will perform hpc_var_bounds functionality on each image load
        IMG_AddInstrumentFunction(ComputeVarBounds, 0);
        // delete image from the list at the unloading callback
        IMG_AddUnloadFunction(DeleteStaticVar, 0);
#ifdef USE_TREE_BASED_FOR_DATA_CENTRIC
        // make gLatestMallocVarSet point to one of mallocVarSets.
        GLOBAL_STATE.latestConcurrentTree  = & (GLOBAL_STATE.concurrentReaderWriterTree[0]);
#endif
    }

    VOID CCTLibAppStartNotification(void* v) {
        GLOBAL_STATE.applicationStarted = true;
    }

// Main for DeadSpy, initialize the tool, register instrumentation functions and call the target program.

    int PinCCTLibInit(IsInterestingInsFptr isInterestingIns, FILE* logFile, CCTLibInstrumentInsCallback userCallback, VOID* userCallbackArg, BOOL doDataCentric) {
        if(GLOBAL_STATE.cctLibUsageMode == CCT_LIB_MODE_POSTMORTEM) {
            fprintf(stderr, "\n CCTLib was initialized for postmortem analysis using PinCCTLibInitForPostmortemAnalysis! Exiting...\n");
            PIN_ExitApplication(-1);
        }

        GLOBAL_STATE.cctLibUsageMode = CCT_LIB_MODE_COLLECTION;
        // Initialize Symbols, we need them to report functions and lines
        PIN_InitSymbols();
        // Intialize
        InitBuffers();
        InitLogFile(logFile);
        InitMapFilePrefix();
        InitSegHandler();
        InitXED();
        //InitLocks();
        // Obtain  a key for TLS storage.
        GLOBAL_STATE.CCTLibTlsKey = PIN_CreateThreadDataKey(0 /*TODO have a destructor*/);
        // remember user instrumentation callback
        GLOBAL_STATE.userInstrumentationCallback = userCallback;
        GLOBAL_STATE.userInstrumentationCallbackArg = userCallbackArg;
        // Register ThreadStart to be called when a thread starts.
        PIN_AddThreadStartFunction(CCTLibThreadStart, 0);
        // Register for context change in case of signals .. Actually this is never used. // Todo: - fix me
        PIN_AddContextChangeFunction(OnSig, 0);
        // Initialize ModuleInfoMap
        //ModuleInfoMap.set_empty_key(UINT_MAX);
        // Record Module information on each Image load.
        IMG_AddInstrumentFunction(CCTLibInstrumentImageLoad, 0);

        if(doDataCentric) {
            InitDataCentric();
        }

        // Since some functions may not be known, instrument every "trace"
        TRACE_AddInstrumentFunction(CCTLibInstrumentTrace, (void*) isInterestingIns);
        // Register Image to be called to instrument functions.
        IMG_AddInstrumentFunction(CCTLibImage, 0);
        // Add a function to report entire stats at the termination.
        PIN_AddFiniFunction(CCTLibFini, 0);
        // Register Fini to be called when the application exits
        PIN_AddFiniFunction(Fini, 0);
        // We need to know if the applicated has started
        PIN_AddApplicationStartFunction(CCTLibAppStartNotification, 0);
        return 0;
    }


    int PinCCTLibInitForPostmortemAnalysis(FILE* logFile, string serializedFilesDirectory) {
        if(GLOBAL_STATE.cctLibUsageMode == CCT_LIB_MODE_COLLECTION) {
            fprintf(stderr, "\n CCTLib was initialized for online collection using PinCCTLibInit! Exiting...\n");
            PIN_ExitApplication(-1);
        }

        GLOBAL_STATE.cctLibUsageMode = CCT_LIB_MODE_POSTMORTEM;
        // Initialize Symbols, we need them to report functions and lines
        PIN_InitSymbols();
        // Intialize
        InitBuffers();
        InitLogFile(logFile);
        InitMapFilePrefix();
        InitSegHandler();
        InitXED();
        //InitLocks();
        // Obtain  a key for TLS storage.
        GLOBAL_STATE.CCTLibTlsKey = PIN_CreateThreadDataKey(0 /*TODO have a destructor*/);
        DeserializeMetadata(serializedFilesDirectory);
        return 0;
    }

#ifdef HAVE_METRIC_PER_IPNODE
    static void BottomUpTraverse(TraceNode *node, void (*opFunc) (const THREADID threadid, ContextHandle_t myHandle, ContextHandle_t parentHandle, void **myMetric, void **parentMetric), const THREADID threadid);
    
    static void BottomUpTraverseHelper(TraceSplay *node, void (*opFunc) (const THREADID threadid, ContextHandle_t myHandle, ContextHandle_t parentHandle, void **myMetric, void **parentMetric), const THREADID threadid) {
        if (!node) return;

	BottomUpTraverseHelper(node->left, opFunc, threadid);
	BottomUpTraverse(node->value, opFunc, threadid);
	BottomUpTraverseHelper(node->right, opFunc, threadid);
    }
    static void BottomUpTraverse(TraceNode *node, void (*opFunc) (const THREADID threadid, ContextHandle_t myHandle, ContextHandle_t parentHandle, void **myMetric, void **parentMetric), const THREADID threadid) {
        if (!node) {
          return;
        }
        for(uint32_t i = 0 ; i < node->nSlots; i++) {
            if((node->childIPs[i]).calleeTraceNodes) {
                // Iterate over all decendent TraceNode of traceNode->childIPs[i]
                BottomUpTraverseHelper((node->childIPs[i]).calleeTraceNodes, opFunc, threadid);
            }
            // do anything here
           assert(node->callerIPNode);
           if( node->callerIPNode) {
               ContextHandle_t myHandle = GetPINCCT32BitContextIndex(&(node->childIPs[i]));
               ContextHandle_t parentHandle = GetPINCCT32BitContextIndex(node->callerIPNode);
               opFunc(threadid, myHandle, parentHandle, &(node->childIPs[i].metric), &(node->callerIPNode->metric));
           }
        }
    }

    void TraverseCCTBottomUp(const THREADID threadid, void (*opFunc) (const THREADID threadid, ContextHandle_t myHandle, ContextHandle_t parentHandle, void **myMetric, void **parentMetric)) {
        ThreadData* tData = CCTLibGetTLS(threadid);
        BottomUpTraverse(tData->tlsRootTraceNode, opFunc, threadid);
    }
#endif

    bool IsSameSourceLine(ContextHandle_t ctxt1, ContextHandle_t ctxt2) {
         if (ctxt1 == ctxt2)
             return true;
          
         ADDRINT ip1 = GetIPFromInfo(GetPINCCTContextFrom32BitIndex(ctxt1));
         ADDRINT ip2 = GetIPFromInfo(GetPINCCTContextFrom32BitIndex(ctxt2));

         if (ip1 == ip2)
             return true;

         uint32_t lineNo1, lineNo2; 
         string filePath1, filePath2;        

         PIN_GetSourceLocation(ip1, NULL, (INT32*) &lineNo1, &filePath1);
         PIN_GetSourceLocation(ip2, NULL, (INT32*) &lineNo2, &filePath2);

         if (filePath1 == filePath2 && lineNo1 == lineNo2)
             return true;
         return false;
    }
}

