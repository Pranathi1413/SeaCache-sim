#include "pe.h"
#include "headers.h"
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

void initPEs() {
  pes.assign(PEcnt, PE());
  peBusy.assign(PEcnt, 0);
  peStallA.assign(PEcnt, 0);
  peStallB.assign(PEcnt, 0);
  peIdle.assign(PEcnt, 0);
  peDoneAt.assign(PEcnt, 0);
}

bool peReady(int pe) {
  return !pes[pe].multiplying;
}

// check if any fibers are waiting in queue. If not, go idle
void peTakeRow(std::any any) {
  int pe = std::any_cast<int>(any);

  if (readyFibers.empty()) {
    pes[pe].multiplying = false;
    peStartWaiting(pe); // Hari: bookkeeping, PE idle, nothing ready
    attemptFinish(-1);
    return;
  }

  peFiberReady(PEArgs{readyFibers.front(), pe});
  readyFibers.pop();
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
  return readyFibers.empty() && !aFetchCount && !bFetchCount && (currentII >= iii || TI + currentII > I);
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
      peIdle[i] += simTime - pes[i].waitStart;
      pes[i].waitStart = simTime;
      anyMultiplying |= pes[i].multiplying;
    }
    if (!anyMultiplying) schedule(finishCalculate);
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
      schedule(prefetchA);
      if (cDramWords > 0) sendToDRAM(DRAMRequest{C_ROW, args.fiberArgs.row, cDramWords, endOfTile, simTime});
      prefetchNow -= currsizeA[args.fiberArgs.row] * 3;
      delete(args.fiberArgs.completedFiberCount); // delete to prevent memory leak
    }
    else {
      schedule(peRowDone, args, 10);
      return;
    }
  }

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
  printf("PE busy = %.1f%%, waiting for A = %.1f%%, waiting for B = %.1f%%, waiting for C = %.1f%%, idle at tile barrier = %.1f%%\n",
         100.0 * busy / peTime, 100.0 * stallA / peTime, 100.0 * stallB / peTime, 100.0 * stallC / peTime,
         100.0 * idle / peTime);
  printf("StallA = %lld, StallB = %lld, StallC = %lld\n", stallA, stallB, stallC);
  printf("Per PE Busy:");
  for(int pe = 0; pe  < PEcnt; pe++) {
    printf(" pe%dBusy: %.1f%% ", pe, 100.0 * peBusy[pe] / totalCycles);
  }
  printf("\n");
}
