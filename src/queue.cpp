#include "queue.h"

std::priority_queue<QueueEntry, std::vector<QueueEntry>, EventOrder> LoopQueue;

long long simTime = 0;

// counts up for every event added, so same-cycle events keep their order
long long eventOrder = 0;

void schedule(void (*func)(std::any), std::any args, long long delay) {
  LoopQueue.push({func, args, simTime + delay, eventOrder++});
}

std::vector<DRAMQueueEntry> DRAMQueue;
std::vector<PrefetchQueueEntry> PrefetchQueue;
std::vector<CacheMissQueueEntry> CacheMissQueue;
std::vector<OpsWaitingOnMemoryQueueEntry> OpsWaitingOnMemoryQueue;
std::vector<OpsWaitingOnPEsQueueEntry> OpsWaitingOnPEsQueue;

// Hari: bookkeeping helpers for OpsWaitingOnPEsQueue, pure visibility,
// no effect on scheduling or output. Call when a PE starts/stops waiting.
void peStartWaiting(int pe) {
  OpsWaitingOnPEsQueueEntry e;
  e.pe = pe;
  e.waitStart = simTime;
  OpsWaitingOnPEsQueue.push_back(e);
}

void peStopWaiting(int pe) {
  for (std::size_t i = 0; i < OpsWaitingOnPEsQueue.size(); i++) {
    if (OpsWaitingOnPEsQueue[i].pe == pe) {
      OpsWaitingOnPEsQueue.erase(OpsWaitingOnPEsQueue.begin() + i);
      return;
    }
  }
}
std::vector<WritebackQueueEntry> WritebackQueue;
