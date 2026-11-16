#include "memory.h"
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
int blockedDRAMRequestWaitTime = 10; // wait time when dram request failes

// one queue per channel
std::vector<std::deque<DRAMRequest>> dramQueue;
std::vector<bool> channelBusy;
std::vector<long long> channelBusyCycles;

// fiber -> requests waiting for it, while it's on its way from DRAM
std::unordered_map<int, std::vector<FiberArgs>> waitingForFiber;

long long hits, misses, merged;

void channelServe(std::any any);
void channelFree(std::any any);
void dramArrive(std::any any);

void initMemory() {
  dramQueue.assign(dramChannels, std::deque<DRAMRequest>());
  channelBusy.assign(dramChannels, false);
  channelBusyCycles.assign(dramChannels, 0);
  waitingForFiber.clear();
  hits = misses = merged = 0;
}

bool canSendToDRAM(int id) {
  int ch = id % dramChannels;
  return dramQueue[ch].size() < maxDramChannelSize;
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

  // total DRAM bandwidth (words per cycle) is split equally between channels
  double wordsPerCycle = HBMbandwidth / dramChannels;
  long long transfer = std::max(1LL, (long long)std::ceil(req.words / wordsPerCycle));
  channelBusy[ch] = true;
  channelBusyCycles[ch] += transfer;

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

void requestFiberFromDRAM(std::any any) {
  FiberArgs args = std::any_cast<FiberArgs>(any);
  auto access = args.access;

  if (args.isInitialFiberRequest && waitingForFiber.count(args.fiber)) {
    // already on its way from DRAM, wait for that one
    waitingForFiber[args.fiber].push_back(args);
    merged++;
    return;
  } 

  if (access.dramWords == 0) {
    hits++;
    schedule(fiberBReady, args, access.sramCycles);
    return;
  }

  // Only update waiting on initial request
  if (args.isInitialFiberRequest) {
    waitingForFiber[args.fiber] = {args};
    args.isInitialFiberRequest = false;
  }
  
  if (canSendToDRAM(args.fiber)) {
    misses++;
    sendToDRAM(DRAMRequest{B_FIBER, args.fiber, access.dramWords});
  } else schedule(requestFiberFromDRAM, args, blockedDRAMRequestWaitTime);
}

void requestFiber(std::any any) {
  FiberArgs args = std::any_cast<FiberArgs>(any);

  // the cache decides hit/miss now, when the PE asks for it
  FiberAccess access = accessFiberGust(args.fiber, args.row);
  args.access = access;
  args.isInitialFiberRequest = true;

  requestFiberFromDRAM(args);
}

void printMemoryStats(long long totalCycles) {
  printf("DRAM channel busy:");
  for (int c = 0; c < dramChannels; c++) {
    printf(" ch%d = %.1f%%", c, 100.0 * channelBusyCycles[c] / totalCycles);
  }
  printf("\n");
  printf("B fibers: %lld hits, %lld misses, %lld waited for a miss already on its way\n", hits,
         misses, merged);
}
