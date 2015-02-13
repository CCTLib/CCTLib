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
#include <sstream>
#include "pin.H"
// Enable data-centric
//
// default ... #define USE_SHADOW_FOR_DATA_CENTRIC
#include "cctlib.H"
using namespace std;
using namespace PinCCTLib;

INT32 Usage2() {
    PIN_ERROR("Pin tool to gather calling context on each instruction and associate each memory access to its data object (shadow memory technique).\n" + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

FILE* gTraceFile;


// Initialized the needed data structures before launching the target program
void ClientInit(int argc, char* argv[]) {
    // Create output file
    char name[MAX_FILE_PATH] = "client.out.";
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

VOID SimpleCCTQuery(THREADID id, const uint32_t slot) {
    GetContextHandle(id, slot);
}


VOID MemAnalysisRoutine(void* addr, THREADID threadId) {
    DataHandle_t handle = GetDataObjectHandle(addr, threadId);
}

VOID InstrumentInsCallback(INS ins, VOID* v, const uint32_t slot) {
    INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)SimpleCCTQuery, IARG_THREAD_ID, IARG_UINT32, slot, IARG_END);

    // Data centric for mem inst
    // Skip call, ret and JMP instructions
    if(INS_IsBranchOrCall(ins) || INS_IsRet(ins)) {
        return;
    }

    // skip stack ... actually our code handles it
    if(INS_IsStackRead(ins) || INS_IsStackWrite(ins))
        return;

    if(INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins)) {
        // How may memory operations?
        UINT32 memOperands = INS_MemoryOperandCount(ins);

        // Iterate over each memory operand of the instruction and add Analysis routine
        for(UINT32 memOp = 0; memOp < memOperands; memOp++) {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR) MemAnalysisRoutine, IARG_MEMORYOP_EA, memOp, IARG_THREAD_ID, IARG_END);
        }
    }
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
    PinCCTLibInit(INTERESTING_INS_ALL, gTraceFile, InstrumentInsCallback, 0 ,/*doDataCentric=*/ true);
    // Launch program now
    PIN_StartProgram();
    return 0;
}


