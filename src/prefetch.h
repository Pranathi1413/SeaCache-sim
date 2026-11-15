#ifndef PREFETCH_H
#define PREFETCH_H

#include <any>
#include <queue>
#include "pe.h"

extern int currentII;
extern long long aFetchCount, bFetchCount;
extern int conccurrentMultiplyCount, maxConcurrentMultiply;
extern std::queue<FiberArgs> readyFibers;
extern std::unordered_map<int, std::vector<FiberArgs>> fibersWaitingToStart;

void initPrefetch();

// events
void prefetchA(std::any any);
void fiberAReady(std::any any);
void fiberBReady(std::any any);
void fiberCDone(std::any any);

#endif//PREFETCH_H
