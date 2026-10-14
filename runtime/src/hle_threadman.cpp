// ThreadManForUser, Kernel_Library, InterruptManager.

#include <algorithm>

#include "psprecore/runtime/hle.hpp"
#include "psprecore/runtime/kernel.hpp"

namespace psprecore::rt {

namespace {

GuestMemory& mem() { return kernel().memory(); }

// ---- threads -----------------------------------------------------------------------------------

void CreateThread(Context& c) {
    Kernel& k = kernel();
    const int id = k.create_thread(mem().string(arg(c, 0), 32), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4));
    if (Thread* t = k.thread(id)) t->gp = c.r[28];
    ret(c, static_cast<std::uint32_t>(id));
}
void StartThread(Context& c) { ret(c, kernel().start_thread(static_cast<int>(arg(c, 0)), arg(c, 1), arg(c, 2))); }
void ExitThread(Context& c) { kernel().exit_current_thread(arg(c, 0), false); }
void ExitDeleteThread(Context& c) { kernel().exit_current_thread(arg(c, 0), true); }
void DeleteThread(Context& c) { ret(c, kernel().delete_thread(static_cast<int>(arg(c, 0)))); }
void GetThreadId(Context& c) { ret(c, static_cast<std::uint32_t>(kernel().current_id())); }

void DelayThread(Context& c) {
    kernel().wait(WaitType::Delay, 0, 0, false, std::max<std::uint32_t>(arg(c, 0), 1));
    ret(c, 0);
}
void DelayThreadCB(Context& c) {
    kernel().wait(WaitType::Delay, 0, 0, true, std::max<std::uint32_t>(arg(c, 0), 1));
    ret(c, 0);
}

void sleep(Context& c, bool callbacks) {
    Kernel& k = kernel();
    Thread& t = k.current();
    if (t.wakeup_requests > 0) {
        --t.wakeup_requests;
        ret(c, 0);
        return;
    }
    ret(c, k.wait(WaitType::Sleep, 0, 0, callbacks));
}
void SleepThread(Context& c) { sleep(c, false); }
void SleepThreadCB(Context& c) { sleep(c, true); }

void WakeupThread(Context& c) {
    Kernel& k = kernel();
    Thread* t = k.thread(static_cast<int>(arg(c, 0)));
    if (!t) return ret(c, kErrorUnknownThread);
    if (t->state == Thread::State::Waiting && t->wait == WaitType::Sleep) k.wake(*t, 0);
    else ++t->wakeup_requests;
    ret(c, 0);
    k.yield_to_higher_priority();
}

void WaitThreadEnd(Context& c) {
    Kernel& k = kernel();
    Thread* t = k.thread(static_cast<int>(arg(c, 0)));
    if (!t) return ret(c, kErrorUnknownThread);
    if (t->state == Thread::State::Dead || t->state == Thread::State::Dormant) return ret(c, t->exit_status);
    ret(c, k.wait(WaitType::ThreadEnd, t->id, arg(c, 1), false));
}

// ---- semaphores --------------------------------------------------------------------------------

void CreateSema(Context& c) {
    Kernel& k = kernel();
    auto s = nx::make_unique<Semaphore>();
    s->id = k.new_id();
    s->name = mem().string(arg(c, 0), 32);
    s->attr = arg(c, 1);
    s->count = static_cast<int>(arg(c, 2));
    s->max = static_cast<int>(arg(c, 3));
    const int id = s->id;
    k.objects<Semaphore>().emplace(id, std::move(s));
    ret(c, static_cast<std::uint32_t>(id));
}

Semaphore* sema(std::uint32_t id) {
    auto& m = kernel().objects<Semaphore>();
    const auto it = m.find(static_cast<int>(id));
    return it == m.end() ? nullptr : it->second.get();
}

void wake_sema_waiters(Semaphore& s) {
    Kernel& k = kernel();
    for (Thread* w : k.waiters(WaitType::Sema, s.id)) {
        const int need = static_cast<int>(w->wait_value);
        if (need > s.count) break;  // FIFO/priority order: stop at the first that cannot be satisfied
        s.count -= need;
        k.wake(*w, 0);
    }
}

void DeleteSema(Context& c) {
    Kernel& k = kernel();
    Semaphore* s = sema(arg(c, 0));
    if (!s) return ret(c, kErrorUnknownSema);
    for (Thread* w : k.waiters(WaitType::Sema, s->id)) k.wake(*w, kErrorWaitDelete);
    k.objects<Semaphore>().erase(s->id);
    ret(c, 0);
    k.yield_to_higher_priority();
}

void SignalSema(Context& c) {
    Semaphore* s = sema(arg(c, 0));
    if (!s) return ret(c, kErrorUnknownSema);
    const int n = static_cast<int>(arg(c, 1));
    if (s->count + n > s->max) return ret(c, kErrorSemaOverflow);
    s->count += n;
    wake_sema_waiters(*s);
    ret(c, 0);
    kernel().yield_to_higher_priority();
}

void wait_sema(Context& c, bool callbacks) {
    Kernel& k = kernel();
    Semaphore* s = sema(arg(c, 0));
    if (!s) return ret(c, kErrorUnknownSema);
    const int need = static_cast<int>(arg(c, 1));
    if (s->count >= need && k.waiters(WaitType::Sema, s->id).empty()) {
        s->count -= need;
        return ret(c, 0);
    }
    k.current().wait_value = static_cast<std::uint32_t>(need);
    ret(c, k.wait(WaitType::Sema, s->id, arg(c, 2), callbacks));
}
void WaitSema(Context& c) { wait_sema(c, false); }

void CancelSema(Context& c) {
    Kernel& k = kernel();
    Semaphore* s = sema(arg(c, 0));
    if (!s) return ret(c, kErrorUnknownSema);
    const auto waiting = k.waiters(WaitType::Sema, s->id);
    if (const std::uint32_t out = arg(c, 2)) mem().write32(out, static_cast<std::uint32_t>(waiting.size()));
    for (Thread* w : waiting) k.wake(*w, kErrorWaitCancel);
    const int count = static_cast<int>(arg(c, 1));
    s->count = count < 0 ? s->count : count;
    ret(c, 0);
    k.yield_to_higher_priority();
}

// ---- event flags -------------------------------------------------------------------------------

enum : std::uint32_t { kWaitOr = 0x01, kWaitClearAll = 0x10, kWaitClear = 0x20 };

EventFlag* event_flag(std::uint32_t id) {
    auto& m = kernel().objects<EventFlag>();
    const auto it = m.find(static_cast<int>(id));
    return it == m.end() ? nullptr : it->second.get();
}

// Checks a wait condition; on success writes the bits, applies the clear mode and returns true.
bool try_event_flag(EventFlag& f, std::uint32_t pattern, std::uint32_t mode, std::uint32_t out) {
    const bool ok = (mode & kWaitOr) ? (f.bits & pattern) != 0 : (f.bits & pattern) == pattern;
    if (!ok) return false;
    if (out) mem().write32(out, f.bits);
    if (mode & kWaitClearAll) f.bits = 0;
    else if (mode & kWaitClear) f.bits &= ~pattern;
    return true;
}

void CreateEventFlag(Context& c) {
    Kernel& k = kernel();
    auto f = nx::make_unique<EventFlag>();
    f->id = k.new_id();
    f->name = mem().string(arg(c, 0), 32);
    f->attr = arg(c, 1);
    f->bits = arg(c, 2);
    const int id = f->id;
    k.objects<EventFlag>().emplace(id, std::move(f));
    ret(c, static_cast<std::uint32_t>(id));
}

void DeleteEventFlag(Context& c) {
    Kernel& k = kernel();
    EventFlag* f = event_flag(arg(c, 0));
    if (!f) return ret(c, kErrorUnknownEventFlag);
    for (Thread* w : k.waiters(WaitType::EventFlag, f->id)) k.wake(*w, kErrorWaitDelete);
    k.objects<EventFlag>().erase(f->id);
    ret(c, 0);
    k.yield_to_higher_priority();
}

void SetEventFlag(Context& c) {
    Kernel& k = kernel();
    EventFlag* f = event_flag(arg(c, 0));
    if (!f) return ret(c, kErrorUnknownEventFlag);
    f->bits |= arg(c, 1);
    for (Thread* w : k.waiters(WaitType::EventFlag, f->id))
        if (try_event_flag(*f, w->wait_value, w->wait_mode, w->wait_out)) k.wake(*w, 0);
    ret(c, 0);
    k.yield_to_higher_priority();
}

void ClearEventFlag(Context& c) {
    EventFlag* f = event_flag(arg(c, 0));
    if (!f) return ret(c, kErrorUnknownEventFlag);
    f->bits &= arg(c, 1);
    ret(c, 0);
}

void WaitEventFlag(Context& c) {
    Kernel& k = kernel();
    EventFlag* f = event_flag(arg(c, 0));
    if (!f) return ret(c, kErrorUnknownEventFlag);
    const std::uint32_t pattern = arg(c, 1), mode = arg(c, 2), out = arg(c, 3);
    if (try_event_flag(*f, pattern, mode, out)) return ret(c, 0);
    Thread& t = k.current();
    t.wait_value = pattern;
    t.wait_mode = mode;
    t.wait_out = out;
    ret(c, k.wait(WaitType::EventFlag, f->id, arg(c, 4), false));
}

// ---- fixed-size pools --------------------------------------------------------------------------

void CreateFpl(Context& c) {
    Kernel& k = kernel();
    auto p = nx::make_unique<Fpl>();
    p->id = k.new_id();
    p->name = mem().string(arg(c, 0), 32);
    p->attr = arg(c, 2);
    p->block_size = (arg(c, 3) + 3) & ~3u;
    const std::uint32_t blocks = arg(c, 4);
    p->base = k.user_heap().alloc(p->block_size * blocks, 16, (p->attr & 0x4000) != 0);
    if (!p->base) return ret(c, kErrorNoMemory);
    p->used.assign(blocks, false);
    const int id = p->id;
    k.objects<Fpl>().emplace(id, std::move(p));
    ret(c, static_cast<std::uint32_t>(id));
}

void AllocateFpl(Context& c) {
    Kernel& k = kernel();
    auto& m = k.objects<Fpl>();
    const auto it = m.find(static_cast<int>(arg(c, 0)));
    if (it == m.end()) return ret(c, kErrorUnknownFpl);
    Fpl& p = *it->second;
    for (std::size_t i = 0; i < p.used.size(); ++i) {
        if (p.used[i]) continue;
        p.used[i] = true;
        mem().write32(arg(c, 1), p.base + static_cast<std::uint32_t>(i) * p.block_size);
        return ret(c, 0);
    }
    ret(c, k.wait(WaitType::Fpl, p.id, arg(c, 2), false));  // TODO: hand out blocks on free
}

// ---- callbacks, time, profiling ----------------------------------------------------------------

void CreateCallback(Context& c) {
    ret(c, static_cast<std::uint32_t>(kernel().create_callback(mem().string(arg(c, 0), 32), arg(c, 1), arg(c, 2))));
}
void DeleteCallback(Context& c) {
    auto& m = kernel().objects<Callback>();
    ret(c, m.erase(static_cast<int>(arg(c, 0))) ? 0 : kErrorUnknownCallback);
}
void GetSystemTimeLow(Context& c) {
    Kernel& k = kernel();
    k.advance(1);  // busy loops on the clock must still see time pass
    ret(c, static_cast<std::uint32_t>(k.now()));
}
void ReferProfiler(Context& c) { ret(c, 0); }

// ---- Kernel_Library / InterruptManager ---------------------------------------------------------

void CpuSuspendIntr(Context& c) { ret(c, 1); }
void CpuResumeIntr(Context& c) { ret(c, 0); }
void RegisterSubIntrHandler(Context& c) {
    kernel().set_subintr_handler(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)), arg(c, 2), arg(c, 3));
    ret(c, 0);
}
void ReleaseSubIntrHandler(Context& c) {
    kernel().set_subintr_handler(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)), 0, 0);
    ret(c, 0);
}
void EnableSubIntr(Context& c) {
    kernel().enable_subintr(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)), true);
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceKernelCreateThread", CreateThread},
    {"sceKernelStartThread", StartThread},
    {"sceKernelExitThread", ExitThread},
    {"sceKernelExitDeleteThread", ExitDeleteThread},
    {"sceKernelDeleteThread", DeleteThread},
    {"sceKernelGetThreadId", GetThreadId},
    {"sceKernelDelayThread", DelayThread},
    {"sceKernelDelayThreadCB", DelayThreadCB},
    {"sceKernelSleepThread", SleepThread},
    {"sceKernelSleepThreadCB", SleepThreadCB},
    {"sceKernelWakeupThread", WakeupThread},
    {"sceKernelWaitThreadEnd", WaitThreadEnd},
    {"sceKernelCreateSema", CreateSema},
    {"sceKernelDeleteSema", DeleteSema},
    {"sceKernelSignalSema", SignalSema},
    {"sceKernelWaitSema", WaitSema},
    {"sceKernelCancelSema", CancelSema},
    {"sceKernelCreateEventFlag", CreateEventFlag},
    {"sceKernelDeleteEventFlag", DeleteEventFlag},
    {"sceKernelSetEventFlag", SetEventFlag},
    {"sceKernelClearEventFlag", ClearEventFlag},
    {"sceKernelWaitEventFlag", WaitEventFlag},
    {"sceKernelCreateFpl", CreateFpl},
    {"sceKernelAllocateFpl", AllocateFpl},
    {"sceKernelCreateCallback", CreateCallback},
    {"sceKernelDeleteCallback", DeleteCallback},
    {"sceKernelGetSystemTimeLow", GetSystemTimeLow},
    {"sceKernelReferThreadProfiler", ReferProfiler},
    {"sceKernelReferGlobalProfiler", ReferProfiler},
    {"sceKernelCpuSuspendIntr", CpuSuspendIntr},
    {"sceKernelCpuResumeIntr", CpuResumeIntr},
    {"sceKernelRegisterSubIntrHandler", RegisterSubIntrHandler},
    {"sceKernelReleaseSubIntrHandler", ReleaseSubIntrHandler},
    {"sceKernelEnableSubIntr", EnableSubIntr},
};

}  // namespace

void hle_install_threadman() { hle_bind(kBindings); }

}  // namespace psprecore::rt
