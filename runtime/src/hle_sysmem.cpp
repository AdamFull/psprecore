// SysMemUserForUser, UtilsForUser, StdioForUser, LoadExecForUser, ModuleMgrForUser, sceSuspendForUser,
// scePower, sceImpose, sceDmac.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

namespace {

GuestMemory& mem() { return kernel().memory(); }

std::map<int, std::uint32_t> g_blocks;  // partition memory uid -> address

// sceKernelAllocPartitionMemory(partition, name, type, size, addr): type 0 = low, 1 = high, 2 = at addr.
void AllocPartitionMemory(Context& c) {
    Kernel& k = kernel();
    const std::uint32_t type = arg(c, 2), size = arg(c, 3);
    std::uint32_t addr = 0;
    if (type == 2) addr = k.user_heap().alloc_at(arg(c, 4), size);
    else addr = k.user_heap().alloc(size, 256, type == 1);
    if (!addr) return ret(c, kErrorNoMemory);
    const int uid = k.new_id();
    g_blocks[uid] = addr;
    ret(c, static_cast<std::uint32_t>(uid));
}
void FreePartitionMemory(Context& c) {
    const auto it = g_blocks.find(static_cast<int>(arg(c, 0)));
    if (it == g_blocks.end()) return ret(c, 0x800200CB);
    kernel().user_heap().free(it->second);
    g_blocks.erase(it);
    ret(c, 0);
}
void GetBlockHeadAddr(Context& c) {
    const auto it = g_blocks.find(static_cast<int>(arg(c, 0)));
    ret(c, it == g_blocks.end() ? 0x800200CB : it->second);
}
void KernelPrintf(Context& c) {
    std::fprintf(stderr, "[guest printf] %s\n", mem().string(arg(c, 0)).c_str());
    ret(c, 0);
}
void ReturnZero(Context& c) { ret(c, 0); }
// Guest memory is always coherent here, but a writeback is when the game expects the GE to see its CPU writes.
void DcacheWritebackAll(Context& c) {
    mark_guest_written_all();
    ret(c, 0);
}
void DcacheWritebackRange(Context& c) {  // (address, size)
    mark_guest_written(arg(c, 0), arg(c, 1));
    ret(c, 0);
}

// ---- UtilsForUser ------------------------------------------------------------------------------

std::int64_t g_wall_clock_base = static_cast<std::int64_t>(std::time(nullptr));

// Wall clock in microseconds: the configured base plus virtual time (see set_wall_clock_base).
std::int64_t wall_clock_us() { return g_wall_clock_base * 1000000 + static_cast<std::int64_t>(kernel().now()); }

void LibcTime(Context& c) {
    const auto t = static_cast<std::uint32_t>(wall_clock_us() / 1000000);
    if (arg(c, 0)) mem().write32(arg(c, 0), t);
    ret(c, t);
}
void LibcClock(Context& c) { ret(c, static_cast<std::uint32_t>(kernel().now())); }
void LibcGettimeofday(Context& c) {
    const std::int64_t us = wall_clock_us();
    if (const std::uint32_t tv = arg(c, 0)) {
        mem().write32(tv, static_cast<std::uint32_t>(us / 1000000));
        mem().write32(tv + 4, static_cast<std::uint32_t>(us % 1000000));
    }
    ret(c, 0);
}

// ---- StdioForUser / LoadExecForUser ------------------------------------------------------------

void Stdin(Context& c) { ret(c, 0); }
void Stdout(Context& c) { ret(c, 1); }
void Stderr(Context& c) { ret(c, 2); }
void ExitGame(Context&) {
    std::fprintf(stderr, "game requested exit\n");
    throw GameExit{};
}

// ---- ModuleMgrForUser (system codec modules are built into the runtime) ------------------------

void LoadModule(Context& c) {
    std::fprintf(stderr, "sceKernelLoadModule(%s) -> built-in\n", mem().string(arg(c, 0)).c_str());
    ret(c, static_cast<std::uint32_t>(kernel().new_id()));
}
void GetModuleId(Context& c) { ret(c, 1); }

// ---- sceSuspendForUser / scePower / sceDmac ----------------------------------------------------

void VolatileMemLock(Context& c) {  // the 4 MiB "volatile" area at 0x08400000
    if (arg(c, 1)) mem().write32(arg(c, 1), 0x08400000);
    if (arg(c, 2)) mem().write32(arg(c, 2), 0x00400000);
    ret(c, 0);
}
void PllFrequencyInt(Context& c) { ret(c, 333); }
void PllFrequencyFloat(Context& c) {
    float f = 333.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    c.fu[0] = bits;
}
void PowerRegisterCallback(Context& c) { ret(c, 0); }
void DmacMemcpy(Context& c) {
    mark_guest_written(arg(c, 0), arg(c, 2));
    std::memmove(mem().ptr(arg(c, 0)), mem().ptr(arg(c, 1)), arg(c, 2));
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceKernelAllocPartitionMemory", AllocPartitionMemory},
    {"sceKernelFreePartitionMemory", FreePartitionMemory},
    {"sceKernelGetBlockHeadAddr", GetBlockHeadAddr},
    {"sceKernelPrintf", KernelPrintf},
    {"sceKernelSetCompiledSdkVersion380_390", ReturnZero},
    {"sceKernelSetCompilerVersion", ReturnZero},
    {"sceKernelLibcTime", LibcTime},
    {"sceKernelLibcClock", LibcClock},
    {"sceKernelLibcGettimeofday", LibcGettimeofday},
    {"sceKernelDcacheWritebackAll", DcacheWritebackAll},
    {"sceKernelDcacheWritebackInvalidateAll", DcacheWritebackAll},
    {"sceKernelDcacheInvalidateRange", ReturnZero},
    {"sceKernelDcacheWritebackInvalidateRange", DcacheWritebackRange},
    {"sceKernelDcacheWritebackRange", DcacheWritebackRange},
    {"sceKernelSetGPO", ReturnZero},
    {"sceKernelStdin", Stdin},
    {"sceKernelStdout", Stdout},
    {"sceKernelStderr", Stderr},
    {"sceKernelExitGame", ExitGame},
    {"sceKernelRegisterExitCallback", ReturnZero},
    {"sceKernelLoadModule", LoadModule},
    {"sceKernelStartModule", ReturnZero},
    {"sceKernelStopModule", ReturnZero},
    {"sceKernelUnloadModule", ReturnZero},
    {"sceKernelGetModuleId", GetModuleId},
    {"sceKernelGetModuleIdByAddress", GetModuleId},
    {"sceKernelPowerLock", ReturnZero},
    {"sceKernelPowerUnlock", ReturnZero},
    {"sceKernelPowerTick", ReturnZero},
    {"sceKernelVolatileMemLock", VolatileMemLock},
    {"sceKernelVolatileMemUnlock", ReturnZero},
    {"scePowerGetPllClockFrequencyInt", PllFrequencyInt},
    {"scePowerGetPllClockFrequencyFloat", PllFrequencyFloat},
    {"scePowerSetClockFrequency350", ReturnZero},
    {"scePowerRegisterCallback", PowerRegisterCallback},
    {"scePowerUnregisterCallback", ReturnZero},
    {"sceImposeSetLanguageMode", ReturnZero},
    {"sceDmacMemcpy", DmacMemcpy},
};

}  // namespace

void set_wall_clock_base(std::int64_t unix_seconds) { g_wall_clock_base = unix_seconds; }
std::int64_t wall_clock_base() { return g_wall_clock_base; }

void hle_install_sysmem() { hle_bind(kBindings); }

}  // namespace ff7cc::rt
