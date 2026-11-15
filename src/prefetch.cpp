#include "prefetch.h"
#include "headers.h"
#include "memory.h"
#include "queue.h"
#include <stdexcept>

std::queue<FiberArgs> readyFibers;
// Holds fibers waiting for reduction in max concurrent multiply.
// The concurrent multiply is meant to simulate a limited number of writeback registers being available.
// One A fiber has to have one writeback register so the intermediate results of C can be stored somewhere.
std::unordered_map<int, std::vector<FiberArgs>> fibersWaitingToStart;
int currentII = 0;
int conccurrentMultiplyCount = 0;
int maxConcurrentMultiply = 32;
long long aFetchCount = 0;
long long bFetchCount = 0;

// reset prefetch values at start of new tile
void initPrefetch() {
    currentII = 0;
    schedule(prefetchA);
}

void prefetchA(std::any any) {
    int initialII = currentII;

    // need to prefetch the next ii+prefetchsize+1
    if (prefetchNow >= prefetchSize) return;

    int prefetchCount = 0;
    bool wasBlocked = false;
    // schedule prefetch for up to dramChannels
    for (; prefetchNow < prefetchSize && currentII < iii && currentII < (initialII + dramChannels); currentII++) {
        if (TI + currentII > I)
        break;

        prefetchRowNow = TI + currentII;
        if (!canSendToDRAM(prefetchRowNow)) { wasBlocked = true; break; }

        // return 0 if can't prefetch that row now
        if (!prefetchrow(prefetchRowNow)) {
        break;
        }
        
        prefetchCount++; aFetchCount++;
        sendToDRAM(DRAMRequest{A_ROW, prefetchRowNow, currsizeA[prefetchRowNow] * 3.0, false, simTime});
    }
    
    // Confirm there was space before prefetching again
    if (prefetchCount) schedule(prefetchA, NULL, prefetchCount); // prefetchCount cycles to prefetch prefetchCount As
    if (wasBlocked) schedule(prefetchA, NULL, 10);
}

void getBFibers(std::any any) {
    RowInfo info = std::any_cast<RowInfo>(any);
    for(;info.fetchedFiberIndex < info.fibers.size(); info.fetchedFiberIndex++) {
        int fiber = info.fibers[info.fetchedFiberIndex];
        if (canSendToDRAM(fiber)) {
            bFetchCount++;
            requestFiber(FiberArgs{info.row, fiber, info.cDramWords, simTime, info.completedFiberCount, info.hasIncrementedConcurrentCount});
        }
        else break;
    }

    if (info.fetchedFiberIndex < info.fibers.size()) schedule(getBFibers, info, 10);
    else aFetchCount--;
}

// dram returned A fiber so now bet B fibers for A
void fiberAReady(std::any any) {
    FiberArgs args = std::any_cast<FiberArgs>(any);
    RowInfo info = startRowGust(args.row);
    if (!info.fibers.size()) {
        aFetchCount--;
        attemptFinish(-1);
        return;
    }

    info.fetchedFiberIndex = 0;
    info.row = args.row;
    int* fiberCount = new(int);
    *fiberCount = info.fibers.size();
    info.completedFiberCount = fiberCount;
    bool* hasIncremented = new(bool);
    *hasIncremented = false;
    info.hasIncrementedConcurrentCount = hasIncremented;
    stallA += simTime - args.requestInitTime;

    getBFibers(info);
}

// dram returned B fiber so try to multiply
void fiberBReady(std::any any) {
    FiberArgs args = std::any_cast<FiberArgs>(any);
    stallB += simTime - args.requestInitTime;
    bool found = false;
    for (int pe = 0; pe < PEcnt; pe++) {
        // Find PE ready to execute multiply
        if (peReady(pe) && (maxConcurrentMultiply > conccurrentMultiplyCount || *args.hasIncrementedConcurrentCount)) {
            if (!*args.hasIncrementedConcurrentCount) {
                conccurrentMultiplyCount++;
                *args.hasIncrementedConcurrentCount = true;
            }
            peFiberReady(PEArgs{args, pe});
            // If fibers were wating, add to readyFibers then wake PEs
            if (fibersWaitingToStart.find(args.row) != fibersWaitingToStart.end()) {
                for(auto args: fibersWaitingToStart[args.row]) readyFibers.push(args);
                fibersWaitingToStart.erase(args.row);
                wakeAllIdlePes();
            }
            found = true;
            break;
        }
    }
    bFetchCount--;
    // If no pe found put in queue of waiting pes
    if (!found) {
        if (*args.hasIncrementedConcurrentCount) readyFibers.push(args);
        else fibersWaitingToStart[args.row].push_back(args);
    }
}

void fiberCDone(std::any any) {
    DRAMRequest req = std::any_cast<DRAMRequest>(any);
    stallC += simTime - req.requestInitTime;
}
