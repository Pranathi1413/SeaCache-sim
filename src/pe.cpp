#include "pe.h"
#include "headers.h"
#include "cache.h"
#include "memory.h"
#include "prefetch.h"
#include "queue.h"
#include "statistics.h"

#include <deque>

struct PE {
  int row;
  bool multiplying;
  long long waitStart;     // when it started waiting, for the stall stats
};
std::vector<PE> pes;

std::vector<long long> peBusy, peStallA, peStallB, peIdle, peDoneAt;

long long stallA = 0, stallB = 0, stallC = 0;
// how many A rows / B fibers / C rows the stalls above add up, to print averages
long long stallACount = 0, stallBCount = 0, stallCCount = 0;

// pran: set once the tile's finishCalculate is scheduled. attemptFinish can be reached from several
// events in the same cycle, and a second finishCalculate would run the next tiles twice
bool tileFinishing = false;

// how many times the adaptive prefetch size was updated
long long prefetchAdjustments = 0;

void initPEs() {
  pes.assign(PEcnt, PE());
  peBusy.assign(PEcnt, 0);
  peStallA.assign(PEcnt, 0);
  peStallB.assign(PEcnt, 0);
  peIdle.assign(PEcnt, 0);
  peDoneAt.assign(PEcnt, 0);
  stallA = stallB = stallC = 0;
  stallACount = stallBCount = stallCCount = 0;
  prefetchAdjustments = 0;
}

bool peReady(int pe) {
  return !pes[pe].multiplying;
}

void wakeAllIdlePes() {
  for(int i = 0; i < PEcnt; ++i) {
    // pran: scheduled, not called directly. peTakeRow can call wakeAllIdlePes again, so direct calls
    // nested inside each other. peTakeRow skips PEs that got a fiber in the meantime
    if (!pes[i].multiplying) schedule(peTakeRow, i);
  }
}

// check if any fibers are waiting in queue. If not, go idle
void peTakeRow(std::any any) {
  int pe = std::any_cast<int>(any);
  if (pes[pe].multiplying) return;

  if (readyFibers.empty() && (fibersWaitingToStart.empty() || (maxConcurrentMultiply <= conccurrentMultiplyCount))) {
    pes[pe].multiplying = false;
    peStartWaiting(pe); // Hari: bookkeeping, PE idle, nothing ready
    attemptFinish(-1);
    return;
  }

  FiberArgs args;
  if (!readyFibers.empty()) {
    args = readyFibers.front(); readyFibers.pop();
    peFiberReady(PEArgs{args, pe});
  } else {
    auto beginIter = fibersWaitingToStart.begin();
    auto waitingFibers = beginIter->second;
    if (!waitingFibers.size()) throw std::runtime_error("Invalid waiting fibers size");
    args = waitingFibers[0]; 
    *args.hasIncrementedConcurrentCount = true;
    conccurrentMultiplyCount++;
    for(int i = 1; i < waitingFibers.size(); ++i) readyFibers.push(waitingFibers[i]);
    fibersWaitingToStart.erase(beginIter);

    peFiberReady(PEArgs{args, pe});
    wakeAllIdlePes();
  }
}

// Start processing ready fiber
void peFiberReady(std::any any) {
  PEArgs args = std::any_cast<PEArgs>(any);
  int pe = args.pe;
  int fiber = args.fiberArgs.fiber;
  args.hasFinished = false;

  // Hari: bookkeeping, resolve wait if PE was idle
  if (!pes[pe].multiplying) {
    peStopWaiting(pe);
    peIdle[pe] += simTime - pes[pe].waitStart;
  }

  pes[pe].waitStart = simTime;
  pes[pe].multiplying = true;
  peBusy[pe] += currsizeB[fiber];
  perPEWork[pe] += currsizeB[fiber];

  schedule(peRowDone, args, currsizeB[fiber]); // 1 multiply per cycle
}

bool isTileDone() {
  return readyFibers.empty() && fibersWaitingToStart.empty() && !aFetchCount && !bFetchCount && (currentII >= iii || TI + currentII > I);
}

// check if need to finish tile calculation
void attemptFinish(std::any any) {
  if (isTileDone()) {
    int pe = std::any_cast<int>(any);
    if (pe >= 0) {
      PE &p = pes[pe];
      p.multiplying = false;
      peStartWaiting(pe); // Hari: bookkeeping, this specific PE just went idle
    }
    bool anyMultiplying = false;
    for (int i = 0; i < PEcnt; i++) {
      // pran: only idle PEs. a PE that's still multiplying was busy since its waitStart, not idle
      if (!pes[i].multiplying) {
        peIdle[i] += simTime - pes[i].waitStart;
        pes[i].waitStart = simTime;
      }
      anyMultiplying |= pes[i].multiplying;
    }
    if (!anyMultiplying && !tileFinishing) {
      tileFinishing = true;
      schedule(finishCalculate);
    }
  }
}

// C goes to DRAM, the PE doesn't wait for it and takes the next row
void peRowDone(std::any any) {
  PEArgs args = std::any_cast<PEArgs>(any);
  int pe = args.pe;
  double cDramWords = args.fiberArgs.cDramWords;
  PE &p = pes[pe];
  bool endOfTile = TI + currentII >= I;

  // Since peRowDown can be called multiple times need to make sure count is only decremented once
  if (!args.hasFinished) *args.fiberArgs.completedFiberCount -= 1;
  args.hasFinished = true;

  // all B fibers share a completed fiber Count. Once it is 0 that means all fibers for a row are completed.
  if (*args.fiberArgs.completedFiberCount <= 0) {
    if (canSendToDRAM(args.fiberArgs.row) || cDramWords == 0) {
      wakePrefetcher();
      if (cDramWords > 0) sendToDRAM(DRAMRequest{C_ROW, args.fiberArgs.row, cDramWords, endOfTile, simTime});
      if (*args.fiberArgs.hasIncrementedConcurrentCount) conccurrentMultiplyCount--;
      prefetchNow -= currsizeA[args.fiberArgs.row] * 3;
      // pran: SeaCache's adaptive prefetch size. this used to be in slidePrefetchWindowGust,
      // which nothing calls since the event-based model, so the prefetch size never changed
      if (adaptive_prefetch && elements_processed_since_last_adjustment >= adjustment_interval) {
        update_prefetch_size();
        elements_processed_since_last_adjustment = 0;
        prefetchAdjustments++;
      }
      delete(args.fiberArgs.completedFiberCount); // delete to prevent memory leak
      delete(args.fiberArgs.hasIncrementedConcurrentCount);
    }
    else {
      // channel queue is full: finish the row when it has space
      waitForDRAMSpace(args.fiberArgs.row, peRowDone, args);
      return;
    }
  }

  p.multiplying = false;
  p.waitStart = simTime;
  // If at end of tile and no new fibers then go idle and posssibly stop calculation
  if (isTileDone()) {
    attemptFinish(pe);
  } else {
    // Try to get new row
    schedule(peTakeRow, pe);
  }
}

void printPEStats(long long totalCycles) {
  long long busy = 0, idle = 0;
  for (int pe = 0; pe < PEcnt; pe++) {
    busy += peBusy[pe];
    idle += peIdle[pe];
  }
  double peTime = (double)PEcnt * totalCycles;
  printf("PE busy = %.1f%%, idle = %.1f%%\n", 100.0 * busy / peTime, 100.0 * idle / peTime);
  // pran: stallA/B/C add up the wait of every request, not the time PEs spent waiting,
  // so they're printed as average latency per request instead of a % of PE time
  printf("average latency: A row = %.1f cycles (%lld rows), B fiber = %.1f cycles (%lld fibers), "
         "C write = %.1f cycles (%lld rows)\n",
         stallACount ? (double)stallA / stallACount : 0.0, stallACount,
         stallBCount ? (double)stallB / stallBCount : 0.0, stallBCount,
         stallCCount ? (double)stallC / stallCCount : 0.0, stallCCount);
  printf("Per PE Busy:");
  for(int pe = 0; pe  < PEcnt; pe++) {
    printf(" pe%dBusy: %.1f%% ", pe, 100.0 * peBusy[pe] / totalCycles);
  }
  printf("\n");
}
