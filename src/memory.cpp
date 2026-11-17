#include "memory.h"
#include "headers.h"
#include "cache.h"
#include "config.h"
#include "pe.h"
#include "prefetch.h"
#include "queue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <vector>
#include <stdexcept>

int dramChannels = 4;
int maxDramChannelSize = 100; // limit max dram channel size
long long dramLatency = 100; // our guess, not from the paper

// one queue per channel
std::vector<std::deque<DRAMRequest>> dramQueue;
std::vector<bool> channelBusy;
std::vector<long long> channelBusyCycles;

// fiber -> requests waiting for it, while it's on its way from DRAM
std::unordered_map<int, std::vector<FiberArgs>> waitingForFiber;

// events waiting for space in a full channel queue, one list per channel
struct SpaceWaiter {
  void (*func)(std::any);
  std::any args;
};
std::vector<std::deque<SpaceWaiter>> waitingForSpace;

long long hits, misses, merged;
// words and requests that went through DRAM, per type (A rows, B fibers, C rows)
double dramWords[3];
long long dramRequests[3];
// how many times a request had to wait for space in a full channel queue
long long spaceWaits;

void channelServe(std::any any);
void channelFree(std::any any);
void dramArrive(std::any any);

void initMemory() {
  dramQueue.assign(dramChannels, std::deque<DRAMRequest>());
  channelBusy.assign(dramChannels, false);
  channelBusyCycles.assign(dramChannels, 0);
  waitingForSpace.assign(dramChannels, std::deque<SpaceWaiter>());
  waitingForFiber.clear();
  hits = misses = merged = 0;
  for (int t = 0; t < 3; t++) {
    dramWords[t] = 0;
    dramRequests[t] = 0;
  }
  spaceWaits = 0;
}

bool canSendToDRAM(int id) {
  int ch = id % dramChannels;
  return dramQueue[ch].size() < maxDramChannelSize;
}

// pran: [fix] the channel queue for id is full. func runs again as soon as the channel takes a request
// out of its queue, instead of retrying every 10 cycles (that added up to 10 made-up cycles each time)
void waitForDRAMSpace(int id, void (*func)(std::any), std::any args) {
  int ch = id % dramChannels;
  waitingForSpace[ch].push_back({func, args});
  spaceWaits++;
}

void sendToDRAM(std::any any) {
  DRAMRequest req = std::any_cast<DRAMRequest>(any);
  int ch = req.id % dramChannels;
  if (dramQueue[ch].size() >= maxDramChannelSize) throw std::runtime_error("DRAM channel capacity exceeded");
  dramQueue[ch].push_back(req);
  schedule(channelServe, ch);
}

// the channel takes the next request from its queue, if it's free
void channelServe(std::any any) {
  int ch = std::any_cast<int>(any);
  if (channelBusy[ch] || dramQueue[ch].empty()) {
    return;
  }
  DRAMRequest req = dramQueue[ch].front();
  dramQueue[ch].pop_front();

  // a spot in the queue just freed up: wake as many waiting as there are free spots.
  // they check again and wait again if someone else took the spot.
  // pran: [fix] once the queue is empty, wake all of them. a woken one may not send here after all
  // (e.g. its fiber hit in the cache), and if the queue drains nothing else would wake the rest
  size_t freeSpots = maxDramChannelSize - dramQueue[ch].size();
  if (dramQueue[ch].empty()) freeSpots = waitingForSpace[ch].size();
  while (freeSpots > 0 && !waitingForSpace[ch].empty()) {
    SpaceWaiter waiter = waitingForSpace[ch].front();
    waitingForSpace[ch].pop_front();
    schedule(waiter.func, waiter.args);
    freeSpots--;
  }

  // total DRAM bandwidth (words per cycle) is split equally between channels
  double wordsPerCycle = HBMbandwidth / dramChannels;
  long long transfer = std::max(1LL, (long long)std::ceil(req.words / wordsPerCycle));
  channelBusy[ch] = true;
  channelBusyCycles[ch] += transfer;
  dramWords[req.type] += req.words;
  dramRequests[req.type]++;

  // channel is busy for the transfer, the data arrives dramLatency after that
  schedule(channelFree, ch, transfer);
  schedule(dramArrive, req, transfer + dramLatency);
}

void channelFree(std::any any) {
  int ch = std::any_cast<int>(any);
  channelBusy[ch] = false;
  schedule(channelServe, ch);
}

void dramArrive(std::any any) {
  DRAMRequest req = std::any_cast<DRAMRequest>(any);
  if (req.type == A_ROW) {
    schedule(fiberAReady, FiberArgs{req.id, -1, 0, req.requestInitTime});
  } else if (req.type == B_FIBER) {
    for (FiberArgs waiting : waitingForFiber[req.id]) {
      schedule(fiberBReady, waiting);
    }
    waitingForFiber.erase(req.id);
  } else if (req.type == C_ROW) {
    schedule(fiberCDone, req);
  }
}

void requestFiber(std::any any) {
  FiberArgs args = std::any_cast<FiberArgs>(any);

  // pran: the cache still sees every request, even for a fiber already on its way, because
  // SeaCache's LFU counters count down one use per access (skipping it dropped filter3D's hit rate 0.79 -> 0.27)
  long long hitBefore = totalhit, accessBefore = totalaccess;
  long long dataHitBefore = data_access_hit, dataTotalBefore = data_access_total;

  // the cache decides hit/miss now, when the PE asks for it
  FiberAccess access = accessFiberGust(args.fiber, args.row);

  if (waitingForFiber.count(args.fiber)) {
    // already on its way from DRAM, wait for that one.
    // pran: the cache added the fiber when it missed, before its data is here, so its answer now
    // is a false hit (or a second miss if it wasn't added). don't count it as either
    totalhit = hitBefore;
    totalaccess = accessBefore;
    data_access_hit = dataHitBefore;
    data_access_total = dataTotalBefore;
    waitingForFiber[args.fiber].push_back(args);
    merged++;
    return;
  }

  if (access.dramWords > 0) {
    waitingForFiber[args.fiber] = {args};
    misses++;
    sendToDRAM(DRAMRequest{B_FIBER, args.fiber, access.dramWords});
  } else {
    hits++;
    schedule(fiberBReady, args, access.sramCycles);
  }
}

void printMemoryStats(long long totalCycles) {
  printf("DRAM channel busy:");
  for (int c = 0; c < dramChannels; c++) {
    printf(" ch%d = %.1f%%", c, 100.0 * channelBusyCycles[c] / totalCycles);
  }
  printf("\n");
  printf("B fibers: %lld hits, %lld misses, %lld waited for a miss already on its way\n", hits,
         misses, merged);
  printf("DRAM words: A = %.0f (%lld rows), B = %.0f (%lld fibers), C = %.0f (%lld rows)\n",
         dramWords[A_ROW], dramRequests[A_ROW], dramWords[B_FIBER], dramRequests[B_FIBER],
         dramWords[C_ROW], dramRequests[C_ROW]);
  // pran: words / HBMbandwidth = cycles at full bandwidth, the same unit as the "DRAM access" lines above
  printf("DRAM access in event timing: A = %.0f, B = %.0f, C = %.0f\n", dramWords[A_ROW] / HBMbandwidth,
         dramWords[B_FIBER] / HBMbandwidth, dramWords[C_ROW] / HBMbandwidth);
  printf("times a request waited for space in a full DRAM queue = %lld\n", spaceWaits);
}
