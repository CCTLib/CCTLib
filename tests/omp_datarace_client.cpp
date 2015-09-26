// * BeginRiceCopyright *****************************************************
//
// Copyright ((c)) 2002-2011, Rice University
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
#include <map>
#include <list>
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
#include <pthread.h>
#include "pin.H"
#include "cctlib.H"
using namespace std;
using namespace PinCCTLib;

// All globals
#define MAX_FILE_PATH   (200)

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

// Enter rank of a phase is twice the number of ordered sections it has entered
// This is same as the phase number rounded up to the next multiple of 2.
#define ENTER_RANK(phase) (((phase) + 1)& 0xfffffffffffffffe)

// Exit rank of a phase is twice the number of ordered sections it has exited.
// This is same as the phase number rounded down to the next multiple of 2.
#define EXIT_RANK(phase) ((phase) & 0xfffffffffffffffe)

#define MAX_REGIONS (1<<20)


//#define DEBUG_LOOP

// Data structures

enum LabelCreationType
{
    CREATE_FIRST,
    CREATE_AFTER_FORK,
    CREATE_AFTER_JOIN,
    CREATE_AFTER_BARRIER,
    CREATE_AFTER_ENTERING_ORDERED_SECTION,
    CREATE_AFTER_EXITING_ORDERED_SECTION,
};

enum AccessType{
    READ_ACCESS = 0,
    WRITE_ACCESS = 1
};

struct LabelComponent{
    uint64_t offset;
    uint64_t span;
    uint64_t phase;
};

static const LabelComponent defaultExtension = {};
FILE *gTraceFile;

class Label {
private:
    LabelComponent * m_labelComponent;
    uint8_t m_labelLength;
    uint64_t m_refCount;
    
public:
    inline uint8_t GetLength() const { return m_labelLength;}
    inline void SetLength(uint64_t len) { m_labelLength = len;}
    inline void AddRef() {__sync_fetch_and_add(&m_refCount,1);}
    inline void RemRef(){
        if (__sync_fetch_and_sub(&m_refCount,1) == 1){
            /*TODO free if 0 assert(0 && "Free label NYI"); */
        }
    }
    
    // Initial label
    Label() : m_labelLength(1), m_refCount(0){
        m_labelComponent = new LabelComponent[GetLength()];
        m_labelComponent[0].offset = 0;
        m_labelComponent[0].span = 1;
        m_labelComponent[0].phase = 0;
    }
    
    
    LabelComponent * GetComponent(int index) const{
        assert(index < GetLength());
        return &m_labelComponent[index];
    }
    
    void CopyLabelComponents(const Label & label){
        SetLength(label.GetLength());
        m_labelComponent = new LabelComponent[GetLength()];
        for(uint32_t i = 0; i < GetLength() ; i++){
            m_labelComponent[i] = label.m_labelComponent[i];
        }
    }
    
    
    void LabelCreateAfterFork(const Label & label, const LabelComponent & extension){
        SetLength(label.GetLength() + 1);
        m_labelComponent = new LabelComponent[GetLength()];
        uint8_t i = 0;
        for(; i < GetLength()-1; i++){
            m_labelComponent[i] = label.m_labelComponent[i];
        }
        m_labelComponent[i] = extension;
    }
    
    void LabelCreateAfterJoin(const Label & label){
        assert(label.GetLength() > 1);
        SetLength(label.GetLength() - 1);
        m_labelComponent = new LabelComponent[GetLength()];
        uint8_t i = 0;
        for(; i < GetLength(); i++){
            m_labelComponent[i] = label.m_labelComponent[i];
        }
        // increase the offset of last component by span
        m_labelComponent[i-1].offset += m_labelComponent[i-1].span;
    }
    
    void LabelCreateAfterBarrier(const Label & label) {
        // Create a label with my parent's offset incremanted by span
        assert(label.GetLength() > 1);
        CopyLabelComponents(label);
        m_labelComponent[GetLength()-2].offset += m_labelComponent[GetLength()-2].span;
        // What to do about m_labelComponent[GetLength()-1] ????
    }
    
    void LabelCreateAfterEneringOrderedSection(const Label & label) {
        // Increment phase
        assert(label.GetLength() > 1);
        CopyLabelComponents(label);
        m_labelComponent[GetLength()-1].phase ++;
    }
    
    void LabelCreateAfterExitingOrderedSection(const Label & label) {
        // Increment phase
        assert(label.GetLength() > 1);
        CopyLabelComponents(label);
        m_labelComponent[GetLength()-1].phase ++;
    }
    
    explicit Label(LabelCreationType type, const Label & label, const LabelComponent & extension = defaultExtension) {
        switch(type){
            case CREATE_AFTER_FORK: LabelCreateAfterFork(label, extension); break;
            case CREATE_AFTER_JOIN: LabelCreateAfterJoin(label); break;
            case CREATE_AFTER_BARRIER: LabelCreateAfterBarrier(label); break;
            case CREATE_AFTER_ENTERING_ORDERED_SECTION: LabelCreateAfterEneringOrderedSection(label); break;
            case CREATE_AFTER_EXITING_ORDERED_SECTION: LabelCreateAfterExitingOrderedSection(label); break;
            default: assert(false);
        }
    }
    
    void PrintLabel() const{
        fprintf(gTraceFile,"\n");
        for(uint8_t i = 0; i < GetLength() ; i++){
            fprintf(gTraceFile,"[%lu,%lu,%lu]", m_labelComponent[i].offset, m_labelComponent[i].span, m_labelComponent[i].phase);
        }
    }
    
};


class LabelIterator {
private:
    const Label & m_label;
    uint8_t m_curLoc;
    uint8_t m_len;
public:
    LabelIterator(const Label & label) : m_label(label) {
        m_curLoc = 0;
        m_len = label.GetLength();
    }
    
    LabelComponent * NextComponent() {
        if(m_len == m_curLoc) {
            return NULL;
        }
        return m_label.GetComponent(m_curLoc++);
    }
    
};

// All fwd declarations

uint8_t ** gL1PageTable[LEVEL_1_PAGE_TABLE_SIZE];

const static char * HW_LOCK = "HW_LOCK";

Label ** gRegionIdToMasterLabelMap;

typedef struct VersionInfo_t{
    union{
        volatile uint64_t readStart;
        volatile uint64_t writeEnd;
    };
    union{
        volatile uint64_t writeStart;
        volatile uint64_t readEnd;
    };
}VersionInfo_t;


typedef struct DataraceInfo_t{
    VersionInfo_t versionInfo;
    Label * read1;
    /* TODO Context pointers can be 32-bit indices */
    ContextHandle_t read1Context;
    Label * read2;
    /* TODO Context pointers can be 32-bit indices */
    ContextHandle_t read2Context;
    Label * write1;
    /* TODO Context pointers can be 32-bit indices */
    ContextHandle_t  write1Context;
}DataraceInfo_t;

typedef struct ExtendedDataraceInfo_t{
    DataraceInfo_t * shadowAddress;
    DataraceInfo_t data;
}ExtendedDataraceInfo_t;


class ThreadData_t {
    Label * m_curLable;
    ADDRINT m_stackBaseAddress;
    ADDRINT m_stackEndAddress;
    ADDRINT m_stackCurrentFrameBaseAddress;
public:
    ThreadData_t(ADDRINT stackBaseAddress, ADDRINT stackEndAddress, ADDRINT stackCurrentFrameBaseAddress) :
    m_curLable(NULL),
    m_stackBaseAddress(stackBaseAddress),
    m_stackEndAddress(stackEndAddress),
    m_stackCurrentFrameBaseAddress(stackCurrentFrameBaseAddress)
    {
    }
    inline Label * GetLabel() const {return m_curLable;}
    inline void SetLabel(Label * label) {
        // TODO: If the current label had a zero ref count, we can possibly delete it
        m_curLable = label;
    }
};


// key for accessing TLS storage in the threads. initialized once in main()
static  TLS_KEY tls_key;

// Range of address where images to skip are loaded e.g., OMP runtime, linux loader.
#define OMP_RUMTIMR_LIB_NAME "/home/xl10/support/gcc-4.7-install/lib64/libgomp.so.1"
#define LINUX_LD_NAME    "/lib64/ld-linux-x86-64.so.2"
#define MAX_SKIP_IMAGES (2)
static ADDRINT gSkipImageAddressRanges[MAX_SKIP_IMAGES][2];
static int gNumCurSkipImages;
static string skipImages[] = {OMP_RUMTIMR_LIB_NAME, LINUX_LD_NAME};



// function to access thread-specific data
ThreadData_t* GetTLS(THREADID threadid) {
    ThreadData_t* tdata =
    static_cast<ThreadData_t*>(PIN_GetThreadData(tls_key, threadid));
    return tdata;
}



volatile bool gShadowPageLock;
inline VOID TakeLock(volatile bool * myLock) {
    do{
        while(*myLock);
    }while(!__sync_bool_compare_and_swap(myLock,0,1));
}

inline VOID ReleaseLock(volatile bool * myLock){
    *myLock = 0;
}



// Given a address generated by the program, returns the corresponding shadow address FLOORED to  PAGE_SIZE
// If the shadow page does not exist a new one is MMAPed

template <class T>
inline T * GetOrCreateShadowBaseAddress(void const * const address) {
    T * shadowPage;
    uint8_t  *** l1Ptr = &gL1PageTable[LEVEL_1_PAGE_TABLE_SLOT(address)];
    if ( *l1Ptr == 0) {
        TakeLock(&gShadowPageLock);
        // If some other thread created L2 page table in the meantime, then let's not do the same.
        if (*l1Ptr == 0) {
            *l1Ptr =  (uint8_t **) calloc(1,LEVEL_2_PAGE_TABLE_SIZE);
        }
        // If some other thread created the same shadow page in the meantime, then let's not do the same.
        if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0 ) {
            (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * SHADOW_STRUCT_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        }
        ReleaseLock(&gShadowPageLock);
    } else if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0 ){
        TakeLock(&gShadowPageLock);
        // If some other thread created the same shadow page in the meantime, then let's not do the same.
        if(((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]) == 0 ) {
            (*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)] =  (uint8_t *) mmap(0, PAGE_SIZE * SHADOW_STRUCT_SIZE, PROT_WRITE | PROT_READ, MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        }
        ReleaseLock(&gShadowPageLock);
    }
    shadowPage = (T *)((*l1Ptr)[LEVEL_2_PAGE_TABLE_SLOT(address)]);
    return shadowPage;
}

template <class T>
inline T * GetOrCreateShadowAddress(void * address) {
    T * shadowPage = GetOrCreateShadowBaseAddress<T>(address);
    return shadowPage + PAGE_OFFSET((uint64_t)address);
}

inline uint64_t GetReadStartForLoc(const DataraceInfo_t * const shadowAddress){
    return shadowAddress->versionInfo.readStart;
}

inline uint64_t GetReadEndForLoc(const DataraceInfo_t * const shadowAddress){
    return shadowAddress->versionInfo.readEnd;
}

inline uint64_t GetWriteStartForLoc(const DataraceInfo_t * const shadowAddress){
    return shadowAddress->versionInfo.writeStart;
}

inline uint64_t GetWriteEndForLoc(const DataraceInfo_t * const shadowAddress){
    return shadowAddress->versionInfo.writeEnd;
}

inline volatile uint64_t * GetWriteStartAddressForLoc( DataraceInfo_t * const shadowAddress){
    return &(shadowAddress->versionInfo.writeEnd);
}


inline bool TryIncrementWriteEndForLoc(const DataraceInfo_t * const shadowAddress){
    return shadowAddress->versionInfo.writeEnd;
}


void UpdateShadowDataAtShadowAddress(DataraceInfo_t * shadowAddress, const DataraceInfo_t &  info){
    shadowAddress->read1 = info.read1;
    shadowAddress->read1Context = info.read1Context;
    shadowAddress->read2 = info.read2;
    shadowAddress->read2Context = info.read2Context;
    shadowAddress->write1 = info.write1;
    shadowAddress->write1Context = info.write1Context;
    shadowAddress->versionInfo.writeEnd = shadowAddress->versionInfo.writeStart; //writeStart will be most upto date
}

void ReadShadowData(DataraceInfo_t * info, DataraceInfo_t * shadowAddress){
    info->read1 = shadowAddress->read1;
    info->read1Context = shadowAddress->read1Context;
    info->read2 = shadowAddress->read2;
    info->read2Context = shadowAddress->read2Context;
    info->write1 = shadowAddress->write1;
    info->write1Context = shadowAddress->write1Context;
}

inline bool IsConsistentSpapshot(const DataraceInfo_t * const info){
    return info->versionInfo.readStart == info->versionInfo.readEnd;
}
// Read a consistent snapshot of the shadow address:
// Uses Leslie Lamport algorithm listed for readers and writers with two integers.
inline void ReadShadowMemory(DataraceInfo_t * shadowAddress, DataraceInfo_t * info) {
#ifdef DEBUG_LOOP
    int trip = 0;
#endif
    
    do{
        
        info->versionInfo.readStart = GetReadStartForLoc(shadowAddress);
        // Need fense to prevent load reordering
        __sync_synchronize();
        ReadShadowData(info, shadowAddress);
        info->versionInfo.readEnd = GetReadEndForLoc(shadowAddress);
        
#ifdef DEBUG_LOOP
        if(trip++ > 100000){
            fprintf(stderr,"\n Loop trip > %d in line %d ... Ver1 = %lu .. ver2 = %lu ... %d", trip, __LINE__, info->versionInfo.readStart, info->versionInfo.readEnd, PIN_ThreadId());
        }
#endif
    }while(!IsConsistentSpapshot(info));
#ifdef DEBUG_LOOP
    
    if(trip > 100000){
        fprintf(stderr,"\n Done ... %d", PIN_ThreadId());
    }
#endif
    
}


// Write a consistent snapshot of the shadow address:
// Uses Leslie Lamport algorithm listed for readers and writers with two integers.
inline bool TryWriteShadowMemory(DataraceInfo_t *  shadowAddress, const DataraceInfo_t &  info) {
    // Get the first integer for this shadow location:
    volatile uint64_t * firstVersionLoc = GetWriteStartAddressForLoc(shadowAddress);
    uint64_t version = info.versionInfo.writeStart;
    if(!__sync_bool_compare_and_swap(firstVersionLoc, version, version+1))
        return false; // fail retry
    
    UpdateShadowDataAtShadowAddress(shadowAddress, info);
    return true;
}


// Fetch the current threads's logical label
inline Label * GetMyLabel(THREADID threadId) {
    return GetTLS(threadId)->GetLabel();
}

// Fetch the current threads's logical label
inline void SetMyLabel(THREADID threadId, Label * label) {
    GetTLS(threadId)->SetLabel(label);
}


void UpdateLabel(Label ** oldLabel, Label * newLabel){
    (*oldLabel) = newLabel;
}

void UpdateContext(ContextHandle_t * oldCtxt, ContextHandle_t  ctxt){
    (*oldCtxt) = ctxt;
}

void CommitChangesToShadowMemory(Label * oldLabel, Label * newLabel){
    if(oldLabel) {
        oldLabel->RemRef();
    }
    newLabel->AddRef();
}

bool HappensBefore(const Label * const oldLabel, const Label * const newLabel){
    // newLabel ought to be non null
    assert(newLabel && "newLabel can't be NULL");
    
    
    /*
     [0,1,0][2,200,0]
     [0,1,0][1,200,0]
     
     if ((oldLabel && newLabel) &&  (oldLabel->GetLength () == 2) && (newLabel->GetLength() == 2) && (oldLabel != newLabel))  {
     bar();
     }
     */
    
    // If oldLabel is null, then this is the first access, hence return true
    if (oldLabel == NULL)
        return true;
    
    // Case 1: oldLabel is a prefix of newLabel
    LabelIterator oldLabelIter = LabelIterator(*oldLabel);
    LabelIterator newLabelIter = LabelIterator(*newLabel);
    
    // Special case TODO // if oldLabel == newLabel , return true;
    LabelComponent * oldLabelComponent = NULL;
    LabelComponent * newLabelComponent = NULL;
    
    while (1){
        oldLabelComponent = oldLabelIter.NextComponent();
        newLabelComponent = newLabelIter.NextComponent();
        if (oldLabelComponent == NULL)
            return true; // Found a prefix
        
        if(newLabelComponent == NULL) {
            assert(0 && "I don't expect this to happen");
            return false; // oldLabel is longer than newLabel
        }
        
        if(oldLabelComponent->offset != newLabelComponent->offset)
            break;
    }
    
    //Case 2: The place where they diverge have are of the form P[O(x),SPAN]S_x
    // and P[O(y),SPAN]S_y and O(x)  < O(y) and ( O(x) mod SPAN == O(y) mod SPAN )
    assert(oldLabelComponent->span == newLabelComponent->span);
    
    if ((oldLabelComponent->offset < newLabelComponent->offset ) &&
        (oldLabelComponent->offset % newLabelComponent->span == newLabelComponent->offset % newLabelComponent->span) ) {
        return true;
    }
    
    // Now check the ordered secton case:
    if ((oldLabelComponent->offset < newLabelComponent->offset) &&
        (EXIT_RANK(oldLabelComponent->phase) < ENTER_RANK(newLabelComponent->phase)) ) {
        return true;
    }
    return false;
}

bool IsLeftOf(const Label * const newLabel, const Label * const oldLabel){
    // newLabel ought to be non null
    assert(newLabel && "newLabel can't be NULL");
    
    // If oldLabel is null, then this is the first access, hence return false
    if (oldLabel == NULL)
        return false;
    
    LabelIterator oldLabelIter = LabelIterator(*oldLabel);
    LabelIterator newLabelIter = LabelIterator(*newLabel);
    
    
    while (1){
        LabelComponent * oldLabelComponent = oldLabelIter.NextComponent();
        LabelComponent * newLabelComponent = newLabelIter.NextComponent();
        if (oldLabelComponent == NULL || newLabelComponent == NULL)
            return false; // Found a prefix
        
        if( (oldLabelComponent->offset % newLabelComponent->span < newLabelComponent->offset % newLabelComponent->span)) {
            return true;
        }
    }
    
    return false;
}

bool MaximizesExitRank(const Label * const newLabel, const Label * const oldLabel){
    // newLabel ought to be non null
    assert(newLabel && "newLabel can't be NULL");
    
    // If oldLabel is null, then this is the first access, hence return false
    if (oldLabel == NULL)
        return false;
    
    LabelIterator oldLabelIter = LabelIterator(*oldLabel);
    LabelIterator newLabelIter = LabelIterator(*newLabel);
    
    while (1){
        LabelComponent * oldLabelComponent = oldLabelIter.NextComponent();
        LabelComponent * newLabelComponent = newLabelIter.NextComponent();
        if (oldLabelComponent == NULL || newLabelComponent == NULL)
            return false;
        
        if( (oldLabelComponent->offset % newLabelComponent->span != newLabelComponent->offset % newLabelComponent->span)) {
            // At the level where offsets diverge
            if( (EXIT_RANK(newLabelComponent->phase) > EXIT_RANK(oldLabelComponent->phase))) {
                return true;
            }
            return false;
        }
        oldLabelComponent = oldLabelIter.NextComponent();
        newLabelComponent = newLabelIter.NextComponent();
    }
    return false;
}


static void DumpRaceInfo(ContextHandle_t oldCtxt,Label * oldLbl, ContextHandle_t newCtxt, Label * newLbl){
    PIN_LockClient();
    fprintf(gTraceFile, "\n ----------");
    oldLbl->PrintLabel();
    PrintFullCallingContext(oldCtxt);
    fprintf(gTraceFile, "\n *****RACES WITH*****");
    newLbl->PrintLabel();
    PrintFullCallingContext(newCtxt);
    fprintf(gTraceFile, "\n ----------");
    PIN_UnlockClient();
}

void CheckRead(DataraceInfo_t * shadowAddress, Label * myLabel, uint32_t opaqueHandle, THREADID threadId) {
    bool reported = false;
    // TODO .. Is this do-while excessive?
    do{
        DataraceInfo_t shadowData;
        ReadShadowMemory(shadowAddress, &shadowData);
        bool updated1 = false;
        Label * oldR1Label = NULL;
        Label * oldR2Label = NULL;
        bool updated2 = false;
        
        // If we have reported a data race originating from this read
        // then let's not inundate with more data races at the same location.
        if (!reported && !HappensBefore(shadowData.write1, myLabel)) {
            // Report W->R Data race
            fprintf(stderr, "\n W->R race");
            DumpRaceInfo(shadowData.write1Context, shadowData.write1, GetContextHandle(threadId, opaqueHandle), myLabel);
            reported = true;
        }
        
        // Update labels
        /* TODO replace HappensBefore with SAME THREAD */
        if(MaximizesExitRank(myLabel, shadowData.read1) || IsLeftOf(myLabel, shadowData.read1) || HappensBefore(shadowData.read1, myLabel)) {
            oldR1Label = shadowData.read1;
            UpdateLabel(&shadowData.read1, myLabel);
            UpdateContext(&shadowData.read1Context, GetContextHandle(threadId, opaqueHandle));
            updated1 = true;
        }
        
        /* TODO replace HappensBefore with SAME THREAD */
        if( (shadowData.read2 && IsLeftOf(shadowData.read2, myLabel))  || HappensBefore(shadowData.read2, myLabel)) {
            oldR2Label = shadowData.read2;
            UpdateLabel(&shadowData.read2, myLabel);
            UpdateContext(&shadowData.read2Context, GetContextHandle(threadId, opaqueHandle));
            updated2 = true;
        }
        
        if (updated1 || updated2) {
            if(!TryWriteShadowMemory(shadowAddress, shadowData)) {
                // someone updated the shadow memory before we could, we need to redo the entire process
                continue;
            }
            // Commit ref count to labels
            if (updated1) {
                CommitChangesToShadowMemory(oldR1Label, myLabel);
            }
            if (updated2) {
                CommitChangesToShadowMemory(oldR2Label, myLabel);
            }
        }
        break;
    }while(1);
}

void CheckWrite(DataraceInfo_t * shadowAddress, Label * myLabel, uint32_t opaqueHandle, THREADID threadId) {
    bool reported = false;
    
    do {
        
        DataraceInfo_t shadowData;
        ReadShadowMemory(shadowAddress, &shadowData);
        
        //#define DEBUG
#ifdef DEBUG
        if(shadowData.write1) {
            fprintf(stderr,"\n Comparing labels:");
            myLabel->PrintLabel();
            shadowData.write1->PrintLabel();
        } else {
            fprintf(stderr,"\n shadowData.write1 is NULL");
        }
#endif
        // If we have reported a data race originating from this read
        // then let's not inundate with more data races at the same location.
        if (!reported && !HappensBefore(shadowData.write1, myLabel)) {
            // Report W->W Data race
            reported = true;
            fprintf(stderr, "\n W->W race");
            DumpRaceInfo(shadowData.write1Context,shadowData.write1, GetContextHandle(threadId, opaqueHandle), myLabel);
        }
        if (!reported && !HappensBefore(shadowData.read1, myLabel)) {
            // Report R->W Data race
            reported = true;
            fprintf(stderr, "\n R->W race");
            DumpRaceInfo(shadowData.read1Context, shadowData.read1, GetContextHandle(threadId, opaqueHandle), myLabel);
        }
        if (!reported && !HappensBefore(shadowData.read2, myLabel)) {
            // Report R->W Data race
            fprintf(stderr, "\n R->W race");
            DumpRaceInfo(shadowData.read2Context, shadowData.read2, GetContextHandle(threadId, opaqueHandle), myLabel);
            reported = true;
        }
        
        Label * oldW1Label = shadowData.write1;
        // Update label
        UpdateLabel(&shadowData.write1, myLabel);
        UpdateContext(&shadowData.write1Context, GetContextHandle(threadId, opaqueHandle));
        
        if(!TryWriteShadowMemory(shadowAddress, shadowData)) {
            // someone updated the shadow memory before we could, we need to redo the entire process
            continue;
        }
        CommitChangesToShadowMemory(oldW1Label, myLabel);
        break;
    }while(1);
}


// Run the datarace protocol and report race.
void ExecuteOffsetSpanPhaseProtocol(DataraceInfo_t *status, Label * myLabel, bool accessType, uint32_t opaqueHandle, THREADID threadId) {
    if(accessType == WRITE_ACCESS){
        CheckWrite(status, myLabel, opaqueHandle, threadId);
    } else { // READ_ACCESS
        CheckRead(status, myLabel, opaqueHandle, threadId);
    }
    
}


VOID CheckRace( VOID * addr, uint32_t accessLen, bool accessType, uint32_t opaqueHandle, THREADID threadId) {
    // Get my Label
    Label * myLabel = GetMyLabel(threadId);
    
    // if myLabel is NULL, then we are in the initial serial part of the program, hence we can skip the rest
    if (myLabel == NULL)
        return;
    
    
    DataraceInfo_t * status = GetOrCreateShadowBaseAddress<DataraceInfo_t>(addr);
    int overflow = (int)(PAGE_OFFSET((uint64_t)addr)) -  (int)((PAGE_OFFSET_MASK - (accessLen-1)));
    status += PAGE_OFFSET((uint64_t)addr);
    if(overflow <= 0 ){
        for(uint32_t i = 0 ; i < accessLen; i++){
            ExecuteOffsetSpanPhaseProtocol(&status[i], myLabel, accessType, opaqueHandle, threadId);
        }
    } else {
        for(uint32_t nonOverflowBytes = 0 ; nonOverflowBytes < accessLen - overflow; nonOverflowBytes++){
            ExecuteOffsetSpanPhaseProtocol(&status[nonOverflowBytes], myLabel, accessType, opaqueHandle, threadId);
        }
        status = GetOrCreateShadowBaseAddress<DataraceInfo_t>(((char *)addr) + accessLen); // +accessLen so that we get next page
        for( int i = 0; i < overflow; i++){
            ExecuteOffsetSpanPhaseProtocol(&status[i], myLabel, accessType, opaqueHandle, threadId);
        }
    }
}


// If it is one of ignoreable instructions, then skip instrumentation.
bool IsIgnorableIns(INS ins){
    //TODO .. Eliminate this check with a better one
    /*
     Access to the stack simply means that the instruction accesses memory relative to the stack pointer (ESP or RSP), or the frame pointer (EBP or RBP). In code compiled without a frame pointer (where EBP/RBP is used as a general register), this may give a misleading result.
     */
    if (INS_IsStackRead(ins) || INS_IsStackWrite(ins) )
        return true;
    
    // skip call, ret and JMP instructions
    if(INS_IsBranchOrCall(ins) || INS_IsRet(ins)){
        return true;
    }
    
    // If ins is in libgomp.so, or /lib64/ld-linux-x86-64.so.2 skip it
    
    for(int i = 0; i < gNumCurSkipImages ; i++) {
        if( (INS_Address(ins) >= gSkipImageAddressRanges[i][0])  && ((INS_Address(ins) < gSkipImageAddressRanges[i][1]))){
            return true;
        }
    }
    
    return false;
}
#define MASTER_BEGIN_FN_NAME "gomp_datarace_master_begin_dynamic_work"
#define DYNAMIC_BEGIN_FN_NAME "gomp_datarace_begin_dynamic_work"
#define DYNAMIC_END_FN_NAME "gomp_datarace_master_end_dynamic_work"
#define ORDERED_ENTER_FN_NAME "gomp_datarace_begin_ordered_section"
#define ORDERED_EXIT_FN_NAME "gomp_datarace_end_ordered_section"
#define CRITICAL_ENTER_FN_NAME "gomp_datarace_begin_critical"
#define CRITICAL_EXIT_FN_NAME "gomp_datarace_end_critical"




//    void gomp_datarace_begin_dynamic_work(uint64_t region_id, long span, long iter);
//    void gomp_datarace_master_end_dynamic_work()
//    void gomp_datarace_master_begin_dynamic_work(uint64_t region_id, long span);
//    void gomp_datarace_begin_ordered_section(uint64_t region_id);
//    void gomp_datarace_begin_critical(void *);
//    void gomp_datarace_end_critical(void *);



typedef void (*FP_MASTER)(uint64_t region_id, long span);
typedef void (*FP_WORKER)(uint64_t region_id, long span, long iter);
typedef void (*FP_WORKER_END)();
typedef void (*FP_ORDERED_ENTER)(uint64_t region_id);
typedef void (*FP_ORDERED_EXIT)(uint64_t region_id);
typedef void (*FP_CRITICAL_ENTER)(void *);
typedef void (*FP_CRITICAL_EXIT)(void *);


void new_MASTER_BEGIN_FN_NAME(uint64_t region_id, long span, THREADID threadid){
    assert(region_id < MAX_REGIONS);
    // Publish my label into the labelHashTable
    assert(gRegionIdToMasterLabelMap[region_id] == 0);
    Label * myLabel =  GetMyLabel(threadid);
    // if the label was NULL, let us create a new initial label
    if (myLabel == NULL) {
        myLabel = new Label();
        SetMyLabel(threadid, myLabel);
    }
    gRegionIdToMasterLabelMap[region_id] = myLabel;
}

void new_DYNAMIC_BEGIN_FN_NAME(uint64_t region_id, long span, long iter, THREADID threadid){
    // Fetch parent label and create new one
    
    //fprintf(stderr,"\n fetched parent label");
    
    assert(gRegionIdToMasterLabelMap[region_id] != NULL);
    
    Label * parentLabel =  gRegionIdToMasterLabelMap[region_id];
    // Create child label
    LabelComponent extension;
    extension.span = span;
    extension.offset = iter;
    extension.phase = 0;
    Label * myLabel = new Label(CREATE_AFTER_FORK, *parentLabel, extension);
    SetMyLabel(threadid, myLabel);
    //myLabel->PrintLabel();
}


void new_DYNAMIC_END_FN_NAME(THREADID threadId){
    
    // Fetch current label and create new one
    Label * parentLabel =  GetMyLabel(threadId);
    Label * myLabel = new Label(CREATE_AFTER_JOIN, *parentLabel);
    SetMyLabel(threadId, myLabel);
    return;
    //myLabel->PrintLabel();
}


void new_ORDERED_ENTER_FN_NAME(uint64_t region_id, THREADID threadId){
    
    // Fetch current label and create new one
    Label * parentLabel =  GetMyLabel(threadId);
    Label * myLabel = new Label(CREATE_AFTER_ENTERING_ORDERED_SECTION, *parentLabel);
    SetMyLabel(threadId, myLabel);
    return;
    //myLabel->PrintLabel();
}

void new_ORDERED_EXIT_FN_NAME(uint64_t region_id, THREADID threadId){
    
    // Fetch current label and create new one
    Label * parentLabel =  GetMyLabel(threadId);
    Label * myLabel = new Label(CREATE_AFTER_EXITING_ORDERED_SECTION, *parentLabel);
    SetMyLabel(threadId, myLabel);
    return;
    //myLabel->PrintLabel();
}




void new_CRITICAL_ENTER_FN_NAME( void * name, THREADID threadid){
    if(name){
        // name is the address of the a symbol i.g. 0x602d20 <.gomp_critical_user_FOO> for a lock FOO
    } else {
        // Analymous locks
    }
}

void new_CRITICAL_EXIT_FN_NAME(void * name, THREADID threadid){
    if(name) {
        // name is the address of the a symbol i.g. 0x602d20 <.gomp_critical_user_FOO> for a lock FOO
    } else {
        // Analymous locks
    }
}


// Wrap all malloc and free in any image
VOID Overrides (IMG img, VOID * v) {
    // Master setup
    
    RTN rtn = RTN_FindByName (img, MASTER_BEGIN_FN_NAME);
    
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO proto_master = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                             MASTER_BEGIN_FN_NAME, PIN_PARG (uint64_t),PIN_PARG (long),
                                             PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_MASTER_BEGIN_FN_NAME),
                              IARG_PROTOTYPE, proto_master,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (proto_master);
    }
    
    
    // Dynamic Start
    rtn = RTN_FindByName (img, DYNAMIC_BEGIN_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO proto_worker = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                             DYNAMIC_BEGIN_FN_NAME, PIN_PARG (uint64_t),PIN_PARG (long),PIN_PARG (long),
                                             PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_DYNAMIC_BEGIN_FN_NAME),
                              IARG_PROTOTYPE, proto_worker,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (proto_worker);
    }
    
    // Dynamic end
    
    rtn = RTN_FindByName (img, DYNAMIC_END_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO proto_end = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                          DYNAMIC_END_FN_NAME, PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_DYNAMIC_END_FN_NAME),
                              IARG_PROTOTYPE, proto_end,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (proto_end);
    }
    
    // Ordered Enter
    
    rtn = RTN_FindByName (img, ORDERED_ENTER_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO ordered_enter = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                              ORDERED_ENTER_FN_NAME, PIN_PARG (uint64_t), PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_ORDERED_ENTER_FN_NAME),
                              IARG_PROTOTYPE, ordered_enter,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (ordered_enter);
    }
    
    // Ordered Exit
    
    rtn = RTN_FindByName (img, ORDERED_EXIT_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO ordered_exit = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                             ORDERED_EXIT_FN_NAME, PIN_PARG (uint64_t), PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_ORDERED_EXIT_FN_NAME),
                              IARG_PROTOTYPE, ordered_exit,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (ordered_exit);
    }
    
    // Critical Enter
    rtn = RTN_FindByName (img, CRITICAL_ENTER_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO critical_enter = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                               CRITICAL_ENTER_FN_NAME, PIN_PARG (void*), PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_CRITICAL_ENTER_FN_NAME),
                              IARG_PROTOTYPE, critical_enter,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (critical_enter);
    }
    // Critical Exit
    rtn = RTN_FindByName (img, CRITICAL_EXIT_FN_NAME);
    if (RTN_Valid (rtn)) {
        
        // Define a function prototype that describes the application routine
        // that will be replaced.
        //
        PROTO critical_exit = PROTO_Allocate (PIN_PARG (void), CALLINGSTD_DEFAULT,
                                              CRITICAL_EXIT_FN_NAME, PIN_PARG (void*), PIN_PARG_END ());
        
        // Replace the application routine with the replacement function.
        // Additional arguments have been added to the replacement routine.
        //
        RTN_ReplaceSignature (rtn, AFUNPTR (new_CRITICAL_EXIT_FN_NAME),
                              IARG_PROTOTYPE, critical_exit,
                              IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                              IARG_THREAD_ID, IARG_END);
        
        // Free the function prototype.
        //
        PROTO_Free (critical_exit);
    }
}


// Is called for every load, store instruction and instruments reads and writes
static VOID InstrumentInsCallback(INS ins, VOID* v, const uint32_t opaqueHandle) {
    if (IsIgnorableIns(ins))
        return;
    
    // If this is an atomic instruction, act as if a lock (HW LOCK) was taken and released
    
    if (INS_IsAtomicUpdate(ins)) {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
                                 (AFUNPTR) new_CRITICAL_ENTER_FN_NAME,
                                 IARG_PTR,HW_LOCK,
                                 IARG_THREAD_ID, IARG_END);
    }
    
    
    // How may memory operations?
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    
    // Iterate over each memory operand of the instruction and add Analysis routine to check races.
    // We correctly handle instructions that do both read and write.
    
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
                                     (AFUNPTR) CheckRace,
                                     IARG_MEMORYOP_EA,memOp, IARG_MEMORYWRITE_SIZE,  IARG_BOOL, WRITE_ACCESS /* write */, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_END);
            
        } else if (INS_MemoryOperandIsRead(ins, memOp)) {
            
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE,(AFUNPTR) CheckRace, IARG_MEMORYOP_EA, memOp, IARG_MEMORYREAD_SIZE, IARG_BOOL, READ_ACCESS /* read */, IARG_UINT32, opaqueHandle, IARG_THREAD_ID, IARG_END);
            
        }
    }
    
    if (INS_IsAtomicUpdate(ins)) {
        INS_InsertPredicatedCall(ins, IPOINT_AFTER,
                                 (AFUNPTR) new_CRITICAL_EXIT_FN_NAME,
                                 IARG_PTR,HW_LOCK,
                                 IARG_THREAD_ID, IARG_END);
    }
}



INT32 Usage() {
    PIN_ERROR("PinTool for datarace detection.\n" + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v) {
    
    // Get the stack base address:
    
    ADDRINT stackBaseAddr = PIN_GetContextReg(ctxt, REG_STACK_PTR);
    pthread_attr_t attr;
    size_t stacksize;
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    
    ThreadData_t* tdata = new ThreadData_t(stackBaseAddr, stackBaseAddr + stacksize, stackBaseAddr);
    //fprintf(stderr,"\n m_stackBaseAddress = %lu, m_stackEndAddress = %lu, size = %lu", stackBaseAddr, stackBaseAddr + stacksize, stacksize);
    // Label will be NULL
    PIN_SetThreadData(tls_key, tdata, threadid);
}

static inline VOID InstrumentImageLoad(IMG img, VOID *v){
    for(uint i = 0; i < MAX_SKIP_IMAGES; i++) {
        if(IMG_Name(img) == skipImages[i]){
            gSkipImageAddressRanges[gNumCurSkipImages][0] = IMG_LowAddress(img);
            gSkipImageAddressRanges[gNumCurSkipImages][1]  = IMG_HighAddress(img);
            gNumCurSkipImages++;
            fprintf(stderr,"\n Skipping image %s", skipImages[i].c_str());
            break;
        }
    }
}

// Initialized the needed data structures before launching the target program
void InitDataRaceSpy(int argc, char *argv[]){
    
    // Create output file
    
    char name[MAX_FILE_PATH] = "DataRaceSpy.out.";
    char * envPath = getenv("OUTPUT_FILE");
    if(envPath){
        // assumes max of MAX_FILE_PATH
        strcpy(name, envPath);
    }
    gethostname(name + strlen(name), MAX_FILE_PATH - strlen(name));
    pid_t pid = getpid();
    sprintf(name + strlen(name),"%d",pid);
    cerr << "\n Creating dead info file at:" << name << "\n";
    
    gTraceFile = fopen(name, "w");
    // print the arguments passed
    fprintf(gTraceFile,"\n");
    for(int i = 0 ; i < argc; i++){
        fprintf(gTraceFile,"%s ",argv[i]);
    }
    fprintf(gTraceFile,"\n");
    
    // Allocate gRegionIdToMasterLabelMap
    gRegionIdToMasterLabelMap = (Label **) calloc(sizeof(Label*) * MAX_REGIONS, 1);
    
    // Obtain  a key for TLS storage.
    tls_key = PIN_CreateThreadDataKey(0);
    
    // Register ThreadStart to be called when a thread starts.
    PIN_AddThreadStartFunction(ThreadStart, 0);
    
    // Record Module information about OMP runtime
    IMG_AddInstrumentFunction(InstrumentImageLoad, 0);
}

// Main for DeadSpy, initialize the tool, register instrumentation functions and call the target program.

int main(int argc, char *argv[]) {
    
    // Initialize PIN
    if (PIN_Init(argc, argv))
        return Usage();
    
    // Initialize Symbols, we need them to report functions and lines
    PIN_InitSymbols();
    
    // Intialize DataraceSpy
    InitDataRaceSpy(argc, argv);
    
    // Intialize CCTLib
    PinCCTLibInit(INTERESTING_INS_MEMORY_ACCESS, gTraceFile, InstrumentInsCallback, 0);
    
    
    // Look up and replace some functions
    IMG_AddInstrumentFunction (Overrides, 0);
    fprintf(stderr,"\n TODO TODO ... eliminate stack local check and make it robust");
    
    // Launch program now
    PIN_StartProgram();
    return 0;
}


