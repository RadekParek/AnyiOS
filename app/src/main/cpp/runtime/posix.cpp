#include "posix.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "error.h"

namespace podnative::runtime {
namespace {
// Darwin's errno values, which a pthread entry point returns directly.
constexpr int kEperm = 1;
constexpr int kEsrch = 3;
constexpr int kEdeadlk = 11;
constexpr int kEbusy = 16;
constexpr int kEinval = 22;
// pthread_mutexattr_t types.
constexpr int MutexNormal = 0;
constexpr int MutexErrorCheck = 1;
constexpr int MutexRecursive = 2;
constexpr int SchedOther = 1;
constexpr int DefaultPriority = 31;
// Guest stack area for created threads, handed out in 256 KiB stacks: the size a
// device-era title expects for a worker thread, in a window chosen at runtime.
constexpr uint32_t AttrSignature = 0x504E4441;  // "PNDA"
constexpr uint32_t DefaultStackSize = 512 * 1024;
constexpr uint32_t StackRegion = 0x6f000000;  // ceiling: the service gates start here
constexpr uint32_t StackRegionSize = 16 * 1024 * 1024;
constexpr uint32_t StackSize = 256 * 1024;
}  // namespace
void Posix::adoptMainThread(const CpuState& state) {
    if (!threads.empty()) return;
    Thread main;
    main.state = state;
    main.main = true;
    threads[1] = main;
    current = 1;
}
Posix::Thread& Posix::currentThread() {
    auto found = threads.find(current);
    if (found == threads.end()) throw Error("POSIX_THREAD", "no running guest thread", current);
    return found->second;
}
bool Posix::allocateStack(uint32_t& top) {
    if (!stackTop) {
        // Find a free window for the thread stacks instead of assuming one: the loader maps
        // the image's own segments and the guest may map more, so a fixed address would
        // collide. The search takes the first gap large enough below the service gates.
        std::vector<Region> regions = memory.regions();
        std::sort(regions.begin(), regions.end(),
                  [](const Region& a, const Region& b) { return a.base < b.base; });
        uint32_t base = 0, cursor = 0x10000000;
        for (const Region& region : regions) {
            if (region.base > cursor && region.base - cursor >= StackRegionSize) {
                base = cursor;
                break;
            }
            cursor = std::max(cursor, region.base + region.size);
        }
        if (!base) base = cursor;
        if (base < 0x10000000 || base + StackRegionSize > StackRegion)
            throw Error("POSIX_THREAD", "no free guest address range for thread stacks", base);
        memory.map({base, StackRegionSize, uint8_t(Read | Write), "guest thread stacks"});
        stackTop = base + StackRegionSize;
        stackCursor = stackTop;
        stackBase = base;
    }
    if (stackCursor < stackBase + StackSize + 4096) return false;
    stackCursor -= StackSize;
    // The top word is left unused so the routine's first push has room under the limit.
    top = stackCursor + StackSize - 16;
    return true;
}
uint32_t Posix::spawn(uint32_t start, uint32_t argument, const CpuState& creator) {
    uint32_t top = 0;
    if (!start || !allocateStack(top))
        throw Error("POSIX_THREAD", "could not create a guest thread", start);
    const uint32_t id = nextThreadId++;
    Thread thread;
    thread.state.registers[0] = argument;
    thread.state.registers[13] = top;
    thread.state.registers[14] = exitGate;  // a thread that returns ends here
    thread.state.registers[15] = start & ~1u;
    thread.state.cpsr = (creator.cpsr & ~0x20u) | (start & 1 ? 0x20 : 0);
    thread.state.fpscr = creator.fpscr;
    threads[id] = thread;
    return id;
}
Posix::Mutex& Posix::mutexAt(uint32_t address) {
    if (!address || !memory.permits(address, 8, Read))
        throw Error("POSIX_MUTEX", "mutex is outside guest memory", address);
    auto found = mutexes.find(address);
    if (found == mutexes.end()) throw Error("POSIX_MUTEX", "mutex was never initialised", address);
    if (found->second.destroyed)
        throw Error("POSIX_MUTEX", "mutex used after pthread_mutex_destroy", address);
    return found->second;
}
int Posix::acquire(uint32_t address, bool wait) {
    Mutex& mutex = mutexAt(address);
    if (!mutex.locked) {
        mutex.locked = true;
        mutex.owner = current;
        mutex.depth = 1;
        locksTaken++;
        return 0;
    }
    if (mutex.owner == current) {
        if (mutex.type == MutexRecursive) {
            mutex.depth++;
            locksTaken++;
            return 0;
        }
        lockErrors++;
        return kEdeadlk;
    }
    if (!wait) {
        lockErrors++;
        return kEbusy;
    }
    // Held by another guest thread: this one waits. The owner hands the mutex over in
    // wakeMutexWaiter(), so when this thread runs again its lock has really succeeded.
    blockOn(address);
    return 0;
}
void Posix::blockOn(uint32_t mutexAddress) {
    Thread& thread = currentThread();
    thread.blocked = true;
    thread.blockedOn = mutexAddress;
    switchPending = true;
}
void Posix::wakeMutexWaiter(uint32_t address) {
    // First waiter in id order wins, which is deterministic and prevents starvation among
    // the small number of threads a title creates.
    for (auto& entry : threads) {
        Thread& thread = entry.second;
        if (!thread.blocked || thread.blockedOn != address) continue;
        Mutex& mutex = mutexes[address];
        mutex.locked = true;
        mutex.owner = entry.first;
        mutex.depth = 1;
        thread.blocked = false;
        thread.blockedOn = 0;
        locksTaken++;
        if (thread.joinResultAt) {
            // A thread waking from a join receives the joined thread's result in r1.
            thread.state.registers[1] = thread.result;
            thread.joinResultAt = 0;
        }
        switchPending = true;
        return;
    }
}
void Posix::retireThread(uint32_t result) {
    Thread& thread = currentThread();
    thread.exited = true;
    thread.result = result;
    thread.blocked = false;
    // Wake everyone joining this thread.
    for (auto& entry : threads) {
        Thread& waiter = entry.second;
        if (!waiter.blocked || waiter.joining != current) continue;
        waiter.blocked = false;
        waiter.joining = 0;
        if (waiter.joinResultAt && memory.permits(waiter.joinResultAt, 4, Write))
            memory.write32(waiter.joinResultAt, result);
        waiter.state.registers[0] = 0;  // pthread_join returns 0
        waiter.joinResultAt = 0;
        switchPending = true;
    }
    switchPending = true;
}
bool Posix::switchThreads(CpuState& state) {
    auto running = threads.find(current);
    if (running != threads.end()) running->second.state = state;
    wakeDueSleepers();
    // Round robin over runnable threads, starting after the current one.
    uint32_t chosen = 0;
    auto walk = [&](uint32_t from, bool wrap) {
        for (auto& entry : threads) {
            if (entry.first <= from || entry.second.exited || entry.second.blocked) continue;
            chosen = entry.first;
            return true;
        }
        if (!wrap) return false;
        for (auto& entry : threads) {
            if (entry.first > from || entry.second.exited || entry.second.blocked) continue;
            chosen = entry.first;
            return true;
        }
        return false;
    };
    if (!walk(current, true)) {
        bool anyAlive = false;
        for (auto& entry : threads)
            if (!entry.second.exited) anyAlive = true;
        if (!anyAlive) {
            allExited = true;
            stall = "every guest thread has exited";
            switchPending = false;
            return false;
        }
        // Only sleepers are left: wait for the nearest deadline instead of spinning, then
        // run whoever woke. This is the scheduler's idle state, not a stall.
        const double wait = nextWakeIn();
        if (wait >= 0.0) {
            const double slice = std::min(wait, 0.005);
            std::this_thread::sleep_for(std::chrono::duration<double>(slice));
            wakeDueSleepers();
            if (walk(current, true)) {
                current = chosen;
                state = threads[chosen].state;
                switchPending = false;
                switches++;
                return true;
            }
        }
        stall = "every remaining guest thread is blocked on a lock, a join or a sleep";
        switchPending = false;
        return false;
    }
    current = chosen;
    state = threads[chosen].state;
    switchPending = false;
    switches++;
    return true;
}
void Posix::blockInRunLoop() {
    Thread& thread = currentThread();
    thread.blocked = true;
    thread.inRunLoop = true;
    thread.blockedOn = 0;
    switchPending = true;
}
void Posix::wakeRunLoopThread() {
    for (auto& [id, thread] : threads) {
        if (thread.exited || !thread.inRunLoop) continue;
        thread.inRunLoop = false;
        thread.blocked = false;
    }
}
void Posix::blockOnEvent(uint32_t eventId) {
    // The main thread cannot wait for itself: the call it is asking for will be dispatched on
    // its next run-loop pass, which is after this call returns.
    if (current == 1) return;
    Thread& thread = currentThread();
    thread.blocked = true;
    thread.blockedOn = 0;
    thread.blockedEvent = eventId;
    switchPending = true;
}
void Posix::wakeEvent(uint32_t eventId) {
    for (auto& [id, thread] : threads) {
        if (thread.exited || thread.blockedEvent != eventId) continue;
        thread.blockedEvent = 0;
        thread.blocked = false;
    }
}
void Posix::blockCurrentIndefinitely() {
    Thread& thread = currentThread();
    thread.blocked = true;
    thread.blockedOn = 0;
    switchPending = true;
}
void Posix::sleepFor(double seconds) {
    Thread& thread = currentThread();
    // A bounded cap keeps a title from parking the emulator for minutes; the request beyond
    // it is logged rather than silently shortened.
    constexpr double Cap = 5.0;
    const double waiting = std::min(std::max(0.0, seconds), Cap);
    if (seconds > Cap)
        log("sleep: asked for " + std::to_string(seconds) + " s; waiting " + std::to_string(Cap) +
            " s");
    thread.wakeAt = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(waiting));
    thread.hasWakeAt = true;
    thread.blocked = true;
    thread.blockedOn = 0;
    switchPending = true;
}
bool Posix::wakeDueSleepers() {
    const auto now = std::chrono::steady_clock::now();
    bool woke = false;
    for (auto& [id, thread] : threads) {
        if (thread.exited || !thread.hasWakeAt) continue;
        if (now < thread.wakeAt) continue;
        thread.hasWakeAt = false;
        thread.blocked = false;
        woke = true;
    }
    return woke;
}
double Posix::nextWakeIn() const {
    const auto now = std::chrono::steady_clock::now();
    double best = -1.0;
    for (const auto& [id, thread] : threads) {
        if (thread.exited || !thread.hasWakeAt) continue;
        const double seconds = std::chrono::duration<double>(thread.wakeAt - now).count();
        if (best < 0.0 || seconds < best) best = seconds;
    }
    return best;
}
size_t Posix::liveThreadCount() const {
    size_t count = 0;
    for (const auto& entry : threads)
        if (!entry.second.exited) count++;
    return count;
}
size_t Posix::runnableCount() const {
    size_t count = 0;
    for (const auto& entry : threads)
        if (!entry.second.exited && !entry.second.blocked) count++;
    return count;
}
void Posix::writeSchedParam(uint32_t address, int policy) {
    (void)policy;
    if (address && memory.permits(address, 4, Write))
        memory.write32(address, uint32_t(DefaultPriority));
}
bool Posix::invoke(const std::string& symbol, CpuState& cpu) {
    auto& r = cpu.registers;
    if (symbol == "_pthread_create") {
        // (pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
        const uint32_t out = r[0], start = r[2], argument = r[3];
        const uint32_t id = spawn(start, argument, cpu);
        if (out) {
            if (!memory.permits(out, 4, Write))
                throw Error("POSIX_THREAD", "pthread_t destination is not writable", out);
            memory.write32(out, id);
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_join") {
        const uint32_t id = r[0], resultAt = r[1];
        auto found = threads.find(id);
        if (found == threads.end()) {
            r[0] = uint32_t(kEsrch);
            return true;
        }
        if (found->second.exited) {
            if (resultAt && memory.permits(resultAt, 4, Write))
                memory.write32(resultAt, found->second.result);
            threads.erase(found);
            r[0] = 0;
            return true;
        }
        Thread& thread = currentThread();
        thread.blocked = true;
        thread.joining = id;
        thread.joinResultAt = resultAt;
        switchPending = true;
        r[0] = 0;  // completed by the scheduler when the joined thread retires
        return true;
    }
    if (symbol == "___podnative_thread_exit") {
        retireThread(r[0]);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutex_init") {
        const uint32_t address = r[0];
        if (!address || !memory.permits(address, 8, Write))
            throw Error("POSIX_MUTEX", "pthread_mutex_init needs a writable mutex", address);
        Mutex fresh;
        if (r[1]) {
            auto attribute = attributes.find(r[1]);
            if (attribute == attributes.end())
                throw Error("POSIX_MUTEX", "mutex attribute was never initialised", r[1]);
            fresh.type = attribute->second;
        }
        mutexes[address] = fresh;
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutex_destroy") {
        auto found = mutexes.find(r[0]);
        if (found == mutexes.end()) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        if (found->second.locked) {
            r[0] = uint32_t(kEbusy);
            return true;
        }
        found->second.destroyed = true;
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutex_lock") {
        r[0] = uint32_t(acquire(r[0], true));
        return true;
    }
    if (symbol == "_pthread_mutex_trylock") {
        r[0] = uint32_t(acquire(r[0], false));
        return true;
    }
    if (symbol == "_pthread_mutex_unlock") {
        Mutex& mutex = mutexAt(r[0]);
        if (!mutex.locked || mutex.owner != current) {
            lockErrors++;
            r[0] = uint32_t(kEperm);
            return true;
        }
        if (--mutex.depth == 0) {
            mutex.locked = false;
            mutex.owner = 0;
            wakeMutexWaiter(r[0]);
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutexattr_init") {
        const uint32_t address = r[0];
        if (!address || !memory.permits(address, 4, Write))
            throw Error("POSIX_MUTEX", "pthread_mutexattr_init needs writable storage", address);
        attributes[address] = MutexNormal;
        memory.write32(address, uint32_t(MutexNormal));
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutexattr_settype") {
        const int type = int(r[1]);
        if (type != MutexNormal && type != MutexErrorCheck && type != MutexRecursive) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        attributes[r[0]] = type;
        if (memory.permits(r[0], 4, Write)) memory.write32(r[0], uint32_t(type));
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_mutexattr_destroy") {
        attributes.erase(r[0]);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_self") {
        r[0] = current;
        return true;
    }
    if (symbol == "_pthread_getschedparam") {
        if (r[1] && memory.permits(r[1], 4, Write)) memory.write32(r[1], uint32_t(SchedOther));
        writeSchedParam(r[2], SchedOther);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_setschedparam") {
        if (int(r[1]) != SchedOther) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        r[0] = 0;
        return true;
    }
    // pthread attributes live in the guest's own opaque block, exactly as Darwin's do: the
    // runtime writes its own fields into that storage and validates the signature on every
    // call, so a guest cannot hand back a block it never initialised.
    if (symbol == "_pthread_attr_init") {
        const uint32_t attr = r[0];
        if (!attr || !memory.permits(attr, 12, Write))
            throw Error("POSIX_ATTR", "pthread_attr_init needs a writable attribute block", attr);
        memory.write32(attr, AttrSignature);
        memory.write32(attr + 4, DefaultStackSize);
        memory.write32(attr + 8, 0);  // joinable
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_attr_destroy") {
        const uint32_t attr = r[0];
        if (attr && memory.permits(attr, 4, Write)) memory.write32(attr, 0);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_attr_setstacksize" || symbol == "_pthread_attr_getstacksize") {
        const uint32_t attr = r[0];
        if (!attr || !memory.permits(attr, 8, Read) || memory.read32(attr) != AttrSignature) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        if (symbol == "_pthread_attr_getstacksize") {
            if (!memory.permits(r[1], 4, Write))
                throw Error("POSIX_ATTR", "stack size out is not writable", r[1]);
            memory.write32(r[1], memory.read32(attr + 4));
            r[0] = 0;
            return true;
        }
        const uint32_t requested = r[1];
        // A stack has to hold at least one frame and must stay inside the thread arena.
        if (requested < 64 * 1024 || requested > StackRegionSize) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        memory.write32(attr + 4, requested);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_attr_setdetachstate") {
        const uint32_t attr = r[0];
        if (!attr || !memory.permits(attr, 12, Read) || memory.read32(attr) != AttrSignature) {
            r[0] = uint32_t(kEinval);
            return true;
        }
        const uint32_t state = r[1];
        if (state != 0 && state != 1) throw Error("POSIX_ATTR", "unknown detach state", state);
        memory.write32(attr + 8, state);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_detach") {
        const uint32_t id = r[0];
        auto found = threads.find(id);
        if (found == threads.end()) {
            r[0] = uint32_t(kEsrch);
            return true;
        }
        found->second.detached = true;
        if (found->second.exited) threads.erase(found);
        r[0] = 0;
        return true;
    }
    if (symbol == "_pthread_equal") {
        r[0] = r[0] == r[1] ? 1 : 0;
        return true;
    }
    if (symbol == "_pthread_kill") {
        const uint32_t id = r[0], signal = r[1];
        if (!threads.count(id)) {
            r[0] = uint32_t(kEsrch);
            return true;
        }
        // Signal 0 only asks whether the thread exists; delivering a real signal is not
        // modelled, and claiming a delivery happened would be a lie.
        if (signal != 0) throw Error("POSIX_SIGNAL", "signal delivery is not modelled", signal);
        r[0] = 0;
        return true;
    }
    if (symbol == "_usleep") {
        // One thousandth of a second resolution is what Darwin's usleep offers; the wait is
        // a guest-thread sleep, not a host sleep, so the game's loop thread keeps running.
        sleepFor(double(uint32_t(r[0])) / 1e6);
        r[0] = 0;
        return true;
    }
    if (symbol == "_nanosleep" || symbol == "_sleep") {
        double seconds = 0;
        if (symbol == "_sleep") {
            seconds = double(uint32_t(r[0]));
        } else {
            const uint32_t request = r[0];
            if (!request || !memory.permits(request, 8, Read))
                throw Error("POSIX_TIME", "nanosleep request is not readable", request);
            seconds = double(memory.read32(request)) + double(memory.read32(request + 4)) / 1e9;
            if (r[1] && memory.permits(r[1], 8, Write)) {
                memory.write32(r[1], 0);
                memory.write32(r[1] + 4, 0);
            }
        }
        // Sleeps are real, but a title cannot park the whole machine for minutes: the wait is
        // served in slices and a request beyond the cap is reported, not silently shortened.
        constexpr double Cap = 5.0;
        const bool capped = seconds > Cap;
        const double waiting = capped ? Cap : std::max(0.0, seconds);
        if (capped)
            log("nanosleep: asked for " + std::to_string(seconds) + " s; waiting " +
                std::to_string(Cap) + " s");
        sleepFor(waiting);
        r[0] = capped ? 1 : 0;
        return true;
    }
    if (symbol == "_pthread_exit") {
        retireThread(r[0]);
        r[0] = 0;
        return true;
    }
    if (symbol == "_sched_yield") {
        // A real yield: the scheduler hands the CPU to the next runnable guest thread. A title
        // that spins on sched_yield while it waits for its main-thread callback must not be
        // able to starve the very thread that would run it.
        switchPending = true;
        r[0] = 0;
        return true;
    }
    return false;
}
std::string Posix::describe() const {
    size_t exited = 0, blocked = 0;
    for (const auto& entry : threads) {
        if (entry.second.exited) exited++;
        if (entry.second.blocked) blocked++;
    }
    std::string text =
        std::to_string(threads.size()) + " guest thread(s) (" + std::to_string(exited) +
        " exited, " + std::to_string(blocked) + " blocked), " + std::to_string(switches) +
        " context switch(es), " + std::to_string(mutexes.size()) + " mutex(es), " +
        std::to_string(locksTaken) + " locks taken, " + std::to_string(lockErrors) + " refused";
    // What each thread is waiting for: a stall report that only says "blocked" leaves the
    // reader guessing which primitive the title is parked on.
    for (const auto& [id, thread] : threads) {
        text += "\nGUEST_THREAD " + std::to_string(id) + (thread.main ? " (main)" : "");
        if (thread.exited) {
            text += ": exited with " + std::to_string(thread.result);
            continue;
        }
        if (thread.inRunLoop) text += ": parked in the UIKit run loop";
        if (thread.blockedEvent)
            text += ": waiting for main-thread event " + std::to_string(thread.blockedEvent);
        if (thread.blockedOn) text += ": waiting for mutex 0x" + std::to_string(thread.blockedOn);
        if (thread.joining) text += ": joining thread " + std::to_string(thread.joining);
        if (thread.hasWakeAt) text += ": sleeping until a deadline";
        if (!thread.blocked) text += ": runnable";
        text += ", pc=0x" + std::to_string(thread.state.registers[15]);
    }
    return text;
}
}  // namespace podnative::runtime
