#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// POSIX threading over several guest threads sharing one address space.
//
// Every thread has its own guest stack and its own register state. The run loop executes one
// thread at a time and switches at 200,000-instruction quanta or as soon as a thread blocks,
// which is real round-robin scheduling without host threads touching guest memory. Mutexes
// are real: a lock held by another thread blocks the caller and the owner hands it over on
// unlock, with normal, error-checking and recursive types behaving as their headers describe.
class Posix final {
   public:
    Posix(Memory& memory, GuestHeap& heap) : memory(memory), heap(heap) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    std::string describe() const;

    // Scheduler interface used by the run loop.
    void adoptMainThread(const CpuState& state);
    // Saves `state` into the running thread and loads the next runnable one into it. Returns
    // false when nothing can run any more (every thread exited or all are blocked).
    bool switchThreads(CpuState& state);
    bool wantsSwitch() const { return switchPending; }
    bool finished() const { return allExited; }
    // Why the last switchThreads() failed, for the stop report.
    const std::string& stallReason() const { return stall; }
    size_t threadCount() const { return threads.size(); }
    // The running thread waits for something only the host can deliver (the main run loop
    // has no event source yet). It stays blocked until the scheduler switches back to it, so
    // the other guest threads — the game's own main loop, for instance — can run.
    void blockCurrentIndefinitely();
    // The main thread waits for a run-loop event (a selector another thread asked to run on
    // it). It is woken by wakeRunLoopThread() and resumes on the gate it parked on.
    void blockInRunLoop();
    void wakeRunLoopThread();
    // A guest thread that wants a call executed on the main thread waits on an event id until
    // the main thread has run it (performSelectorOnMainThread:withObject:waitUntilDone:YES).
    void blockOnEvent(uint32_t eventId);
    void wakeEvent(uint32_t eventId);
    size_t runnableCount() const;
    // Threads that have not exited, whether or not they can run right now. A title whose loop
    // thread is sleeping between frames is still a multi-threaded title, so a decision about
    // parking the main thread must look at this, not at the runnable set.
    size_t liveThreadCount() const;
    // Sleeps the running guest thread until its deadline. Another guest thread runs in the
    // meantime, and the host is idle rather than spinning when nothing else can run.
    void sleepFor(double seconds);
    // The gate a created thread returns into; a thread routine that returns ends here.
    void setExitGate(uint32_t address) { exitGate = address; }
    // Guest-visible lines (a sleep that had to be capped, for instance) go through the same
    // log the rest of the runtime uses.
    void setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
    // Creates a guest thread that starts executing at `start` with `argument` in r0 and the
    // exit gate as its return address. Host-initiated threads (NSThread -start) use this so
    // the selector runs on a thread this scheduler owns.
    uint32_t spawnThread(uint32_t start, uint32_t argument) {
        CpuState creator;
        creator.cpsr = 0x10;  // ARM user mode
        return spawn(start, argument, creator);
    }

   private:
    Memory& memory;
    GuestHeap& heap;
    struct Thread {
        CpuState state;
        bool blocked = false, exited = false, main = false, detached = false;
        uint32_t result = 0;        // value the start routine returned
        uint32_t joining = 0;       // thread id this thread is waiting to join
        uint32_t joinResultAt = 0;  // guest address that receives the joined result
        uint32_t blockedOn = 0;     // mutex address this thread waits for
        uint32_t blockedEvent = 0;  // main-thread event this thread waits for
        bool inRunLoop = false;     // main thread parked in UIKit's run loop
        // A sleeping thread has a deadline; the scheduler wakes it and runs it again.
        std::chrono::steady_clock::time_point wakeAt;
        bool hasWakeAt = false;
    };
    struct Mutex {
        bool locked = false, destroyed = false;
        int type = 0;  // 0 normal, 1 error-checking, 2 recursive
        int depth = 0;
        uint32_t owner = 0;
    };
    std::map<uint32_t, Thread> threads;
    std::map<uint32_t, Mutex> mutexes;
    std::map<uint32_t, int> attributes;
    uint32_t current = 0, nextThreadId = 2, exitGate = 0;
    uint32_t stackCursor = 0, stackTop = 0, stackBase = 0;
    bool switchPending = false, allExited = false;
    std::string stall;
    std::function<void(const std::string&)> logSink;
    void log(const std::string& text) const {
        if (logSink) logSink(text);
    }
    size_t locksTaken = 0, lockErrors = 0, switches = 0;

    uint32_t spawn(uint32_t start, uint32_t argument, const CpuState& creator);
    Thread& currentThread();
    Mutex& mutexAt(uint32_t address);
    int acquire(uint32_t address, bool wait);
    void wakeMutexWaiter(uint32_t address);
    void blockOn(uint32_t mutexAddress);
    // Wakes every sleeping thread whose deadline passed. Returns true when one did.
    bool wakeDueSleepers();
    // Seconds until the earliest sleeping deadline, or a negative value when none sleeps.
    double nextWakeIn() const;
    void retireThread(uint32_t result);
    // Allocates the per-thread stack area once and hands out 256 KiB stacks from it.
    bool allocateStack(uint32_t& top);
    void writeSchedParam(uint32_t address, int policy);
};
}  // namespace podnative::runtime
