#include "ff7cc/runtime/hle.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>

#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

namespace {

struct Entry {
    const HleImport* import = nullptr;
    HleFunction fn = nullptr;
};

std::unordered_map<std::uint32_t, Entry>& table() {
    static std::unordered_map<std::uint32_t, Entry> t;
    return t;
}

}  // namespace

void hle_bind(const HleBinding* bindings, std::size_t count) {
    auto& t = table();
    if (t.empty())
        for (const auto& imp : hle_imports()) t[imp.nid].import = &imp;
    for (std::size_t i = 0; i < count; ++i) {
        bool found = false;
        for (auto& [nid, e] : t) {
            if (std::strcmp(e.import->name, bindings[i].name) == 0) {
                e.fn = bindings[i].fn;
                found = true;
            }
        }
        if (!found) std::fprintf(stderr, "hle_bind: %s is not imported by the game\n", bindings[i].name);
    }
}

void hle_install() {
    hle_install_threadman();
    hle_install_sysmem();
    hle_install_io();
    hle_install_display();
    hle_install_audio();
}

}  // namespace ff7cc::rt

extern "C" void hle_call(Context* ctx, uint32_t nid) {
    using namespace ff7cc::rt;
    const auto it = table().find(nid);
    const Entry* e = it == table().end() ? nullptr : &it->second;
    Kernel& k = kernel();
    if (k.trace_hle && e)
        std::fprintf(stderr, "[%8llu us] %-16s %s(%08x, %08x, %08x, %08x) ra=%08x\n",
                     static_cast<unsigned long long>(k.now()), k.current().name.c_str(), e->import->name,
                     ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7], ctx->r[31]);
    if (e && e->fn) {
        e->fn(*ctx);
        return;
    }
    static std::set<std::uint32_t> reported;
    static std::mutex mutex;
    {
        std::lock_guard lock(mutex);
        if (reported.insert(nid).second)
            std::fprintf(stderr, "HLE: unimplemented %s::%s (NID %08X), returning 0\n",
                         e ? e->import->library : "?", e ? e->import->name : "?", nid);
    }
    ctx->r[2] = 0;
}
