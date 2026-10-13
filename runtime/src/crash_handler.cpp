// Crash reports: faulting address (and the guest address it maps to) plus a symbolised host stack, which shows
// the recompiled guest functions (f_XXXXXXXX) involved. Windows only for now.

#include <cstdint>
#include <cstdio>

#include "ff7cc/recomp/recomp.h"

#if defined(_WIN32)
#include <windows.h>
// windows.h must come first
#include <dbghelp.h>

namespace ff7cc::rt {

namespace {

LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    std::fprintf(stderr, "\n*** crash: exception 0x%08lX at %p\n", rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const auto addr = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
        const auto base = reinterpret_cast<std::uintptr_t>(g_membase);
        std::fprintf(stderr, "    %s host address 0x%llx", rec->ExceptionInformation[0] ? "writing" : "reading",
                     static_cast<unsigned long long>(addr));
        if (addr >= base && addr < base + 0x20000000) std::fprintf(stderr, " = guest 0x%08llX", static_cast<unsigned long long>(addr - base));
        std::fprintf(stderr, "\n");
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (int i = 0; i < 24; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD line_disp = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
        const bool has_line = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line);
        std::fprintf(stderr, "    #%-2d %s+0x%llx", i, named ? symbol->Name : "?", static_cast<unsigned long long>(displacement));
        if (has_line) std::fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_crash_handler() { SetUnhandledExceptionFilter(on_crash); }

}  // namespace ff7cc::rt

#else

namespace ff7cc::rt {
void install_crash_handler() {}
}  // namespace ff7cc::rt

#endif
