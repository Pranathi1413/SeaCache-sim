#ifndef PE_H
#define PE_H

#include <any>
#include <vector>
#include "memory.h"

// what one row of A needs (in simulator.cpp)
struct RowInfo {
  std::vector<int> fibers; // B rows it needs
  double aDramWords, cDramWords;
  long long aSramCycles;
  int fetchedFiberIndex = 0;
  int row;
  int* completedFiberCount;
  bool* hasIncrementedConcurrentCount;
};
RowInfo startRowGust(int row);
void slidePrefetchWindowGust(int ii);

// one B fiber a PE asks for
struct FiberArgs {
  int row, fiber; double cDramWords; 
  long long requestInitTime; 
  int* completedFiberCount; 
  bool* hasIncrementedConcurrentCount; 
  FiberAccess access;
  bool isInitialFiberRequest;
  long long bReadyTime;
};
struct PEArgs {
  FiberArgs fiberArgs; int pe; bool hasFinished;
};

void initPEs();
bool peReady(int pe);
void wakeAllIdlePes();

extern long long stallA, stallB, stallC;
extern long long delayPrefetch, delayBFetch, delayWriteback, delayMultiply;

// events
void peTakeRow(std::any any);
void peFiberReady(std::any any);
void peRowDone(std::any any);
void attemptFinish(std::any any);

void printPEStats(long long totalCycles);

#endif
