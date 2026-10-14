// sceUtilitySavedata: the PSP save/load system dialog.
//
// Saves are stored unencrypted under <save dir>/SAVEDATA/<gameName><saveName>/: the game's data file, the
// icon/picture files it supplies, and PARAM.txt (titles and details for the save picker). The list modes show
// the frontend's picker (see set_savedata_ui): the dialog stays RUNNING until answer_save_dialog(). Without a
// frontend the slot is chosen from the game's focus hint (load: the newest save; save: the focused or first
// free slot) and the status goes INIT -> FINISHED at once.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "ff7cc/io/path.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/io.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::rt {

namespace {


GuestMemory& mem() { return kernel().memory(); }

// SceUtilitySavedataParam offsets (pspsdk psputility_savedata.h).
enum : std::uint32_t {
    kResult = 0x1C, kMode = 0x30, kOverwrite = 0x38, kGameName = 0x3C, kSaveName = 0x4C, kSaveNameList = 0x60,
    kFileName = 0x64, kDataBuf = 0x74, kDataBufSize = 0x78, kDataSize = 0x7C, kSfoTitle = 0x80,
    kSfoSavedataTitle = 0x100, kSfoDetail = 0x180, kSfoParental = 0x580, kIcon0 = 0x584, kIcon1 = 0x594,
    kPic1 = 0x5A4, kSnd0 = 0x5B4, kFocus = 0x5C8,
};
enum Mode : std::uint32_t {
    kAutoLoad = 0, kAutoSave = 1, kLoad = 2, kSave = 3, kListLoad = 4, kListSave = 5, kListDelete = 6,
    kDelete = 7, kSizes = 8, kList = 11,
};
enum Focus : std::uint32_t {
    kFocusFirstList = 1, kFocusLastList = 2, kFocusLatest = 3, kFocusOldest = 4, kFocusFirstEmpty = 7,
    kFocusLastEmpty = 8,
};
constexpr std::uint32_t kErrorLoadNoData = 0x80110307;
constexpr std::uint32_t kErrorLoadBroken = 0x80110308;
constexpr std::uint32_t kErrorSaveNoSpace = 0x80110383;
constexpr std::uint32_t kErrorDeleteNoData = 0x80110347;

constexpr std::uint32_t kResultCancelled = 1;

enum : std::uint32_t { kStatusNone = 0, kStatusInit = 1, kStatusRunning = 2, kStatusFinished = 3, kStatusShutdown = 4 };
std::uint32_t g_status = kStatusNone;

bool g_ui = false;
struct Pending {
    std::uint32_t param = 0;
    std::uint32_t mode = 0;
    nx::string game;
    SaveDialog dialog;
};
std::optional<Pending> g_pending;
bool g_deferred = false;
std::optional<int> g_queued_answer;

void apply_answer(int slot);

nx::string guest_string(std::uint32_t addr, std::size_t max) {
    nx::string s = mem().string(addr, max);
    return s;
}

nx::string save_folder(const nx::string& game, const nx::string& save) { return path_join(save_dir(), "SAVEDATA", game + save); }

nx::vector<nx::string> save_name_list(std::uint32_t param) {
    nx::vector<nx::string> names;
    const std::uint32_t list = mem().read32(param + kSaveNameList);
    if (!list) return names;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const nx::string n = guest_string(list + i * 20, 20);
        if (n.empty()) break;
        names.push_back(n);
    }
    return names;
}

bool save_exists(const nx::string& game, const nx::string& save, const nx::string& file) {
    return nx::fs::exists(path_join(save_folder(game, save), file));
}

// Last write, nanoseconds since the Unix epoch (0 if unknown).
std::uint64_t save_time(const nx::string& game, const nx::string& save, const nx::string& file) {
    return nx::fs::stat_file(path_join(save_folder(game, save), file)).mtime_ns;
}

nx::vector<std::uint8_t> read_file(nx::fs::path_view path) {
    const auto data = nx::fs::file_read(path);
    if (!data.has_value()) return {};
    return nx::vector<std::uint8_t>(data.value().data(), data.value().data() + data.value().size());
}

void write_file(nx::fs::path_view path, const std::uint8_t* data, std::size_t size) {
    (void)nx::fs::file_write(path, std::span(data, size));
}

// PARAM.txt holds one key=value per line; backslashes and newlines in values are escaped.
nx::string escape(nx::string_view v) {
    nx::string out;
    for (const char ch : v) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch != '\r') out += ch;
    }
    return out;
}
nx::string unescape(nx::string_view v) {
    nx::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            ++i;
            out += v[i] == 'n' ? '\n' : v[i];
        } else {
            out += v[i];
        }
    }
    return out;
}

// Optional files (ICON0.PNG etc.): {buf, bufSize, size, unknown}.
void save_extra(std::uint32_t param, std::uint32_t field, nx::fs::path_view path) {
    const std::uint32_t buf = mem().read32(param + field), size = mem().read32(param + field + 8);
    if (buf && size) write_file(path, mem().ptr(buf), size);
}
void load_extra(std::uint32_t param, std::uint32_t field, nx::fs::path_view path) {
    const std::uint32_t buf = mem().read32(param + field), cap = mem().read32(param + field + 4);
    if (!buf || !cap || !nx::fs::exists(path)) return;
    const auto data = read_file(path);
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(data.size(), cap));
    std::memcpy(mem().ptr(buf), data.data(), n);
    mem().write32(param + field + 8, n);
}

std::uint32_t do_save(std::uint32_t param, const nx::string& game, const nx::string& save) {
    const nx::string file = guest_string(param + kFileName, 13);
    const nx::string folder = save_folder(game, save);
    if (!nx::fs::create_directories(folder).has_value()) return kErrorSaveNoSpace;
    const std::uint32_t buf = mem().read32(param + kDataBuf), size = mem().read32(param + kDataSize);
    if (buf && !file.empty()) write_file(path_join(folder, file), mem().ptr(buf), size);
    save_extra(param, kIcon0, path_join(folder, "ICON0.PNG"));
    save_extra(param, kIcon1, path_join(folder, "ICON1.PMF"));
    save_extra(param, kPic1, path_join(folder, "PIC1.PNG"));
    save_extra(param, kSnd0, path_join(folder, "SND0.AT3"));
    const nx::string sfo = nx::format("title={}\nsavedata_title={}\nparental_level={}\ndetail={}\n",
                                      escape(guest_string(param + kSfoTitle, 128)),
                                      escape(guest_string(param + kSfoSavedataTitle, 128)),
                                      int{*mem().ptr(param + kSfoParental)}, escape(guest_string(param + kSfoDetail, 1024)));
    (void)nx::fs::file_write_text(path_join(folder, "PARAM.txt"), sfo);
    std::fprintf(stderr, "savedata: saved %s/%s (%u bytes)\n", (game + save).c_str(), file.c_str(), size);
    return 0;
}

std::uint32_t do_load(std::uint32_t param, const nx::string& game, const nx::string& save) {
    const nx::string file = guest_string(param + kFileName, 13);
    const nx::string folder = save_folder(game, save);
    if (file.empty() || !nx::fs::exists(path_join(folder, file))) return kErrorLoadNoData;
    const auto data = read_file(path_join(folder, file));
    const std::uint32_t buf = mem().read32(param + kDataBuf), cap = mem().read32(param + kDataBufSize);
    if (!buf || data.size() > cap) return kErrorLoadBroken;
    std::memcpy(mem().ptr(buf), data.data(), data.size());
    mem().write32(param + kDataSize, static_cast<std::uint32_t>(data.size()));
    load_extra(param, kIcon0, path_join(folder, "ICON0.PNG"));
    load_extra(param, kPic1, path_join(folder, "PIC1.PNG"));
    mem().write_string(param + kSaveName, save);
    std::fprintf(stderr, "savedata: loaded %s/%s (%zu bytes)\n", (game + save).c_str(), file.c_str(), data.size());
    return 0;
}

// Picks the slot a list dialog would end on.
nx::string pick_slot(std::uint32_t param, const nx::string& game, bool for_save) {
    const nx::string file = guest_string(param + kFileName, 13);
    nx::vector<nx::string> names = save_name_list(param);
    if (names.empty()) names.push_back(guest_string(param + kSaveName, 20));
    nx::vector<nx::string> existing;
    for (const auto& n : names)
        if (save_exists(game, n, file)) existing.push_back(n);
    const auto newest = [&] {
        return *std::max_element(existing.begin(), existing.end(), [&](const auto& a, const auto& b) {
            return save_time(game, a, file) < save_time(game, b, file);
        });
    };
    if (!for_save) return existing.empty() ? nx::string{} : newest();
    switch (mem().read32(param + kFocus)) {
        case kFocusLastList: return names.back();
        case kFocusLatest:
            if (!existing.empty()) return newest();
            break;
        case kFocusLastEmpty:
            for (auto it = names.rbegin(); it != names.rend(); ++it)
                if (!save_exists(game, *it, file)) return *it;
            break;
        default: break;
    }
    for (const auto& n : names)  // first empty slot, else the newest save
        if (!save_exists(game, n, file)) return n;
    return existing.empty() ? names.front() : newest();
}


SaveSlotInfo slot_info(const nx::string& game, const nx::string& save, const nx::string& file) {
    SaveSlotInfo info;
    info.name = save;
    info.exists = save_exists(game, save, file);
    if (!info.exists) return info;
    const nx::string folder = save_folder(game, save);
    info.time = static_cast<std::int64_t>(save_time(game, save, file) / 1'000'000'000);
    if (nx::fs::exists(path_join(folder, "ICON0.PNG"))) info.icon = path_join(folder, "ICON0.PNG");
    const auto sfo = nx::fs::file_read_text(path_join(folder, "PARAM.txt"));
    nx::string_view rest = sfo.has_value() ? nx::string_view(sfo.value()) : nx::string_view{};
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        nx::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto eq = line.find('=');
        if (eq == nx::string_view::npos) continue;
        const nx::string_view key = line.substr(0, eq);
        const nx::string value = unescape(line.substr(eq + 1));
        if (key == "savedata_title") info.title = value;
        else if (key == "detail") info.detail = value;
    }
    return info;
}

// Opens the frontend's picker for a list dialog; the game polls GetStatus until answer_save_dialog().
void open_dialog(std::uint32_t param, std::uint32_t mode, const nx::string& game) {
    const nx::string file = guest_string(param + kFileName, 13);
    nx::vector<nx::string> names = save_name_list(param);
    if (names.empty()) names.push_back(guest_string(param + kSaveName, 20));
    Pending p{param, mode, game, {}};
    p.dialog.save = mode == kListSave;
    const nx::string focus = pick_slot(param, game, p.dialog.save);
    for (const auto& n : names) {
        SaveSlotInfo info = slot_info(game, n, file);
        if (!p.dialog.save && !info.exists) continue;
        if (n == focus) p.dialog.focus = static_cast<int>(p.dialog.slots.size());
        p.dialog.slots.push_back(std::move(info));
    }
    g_pending = std::move(p);
}

void InitStart(Context& c) {
    const std::uint32_t param = arg(c, 0);
    const std::uint32_t mode = mem().read32(param + kMode);
    const nx::string game = guest_string(param + kGameName, 13);
    const nx::string save = guest_string(param + kSaveName, 20);
    const auto names = save_name_list(param);
    std::fprintf(stderr, "savedata: mode %u, %s%s/%s, %zu names in the list (%s..%s)\n", mode, game.c_str(), save.c_str(),
                 guest_string(param + kFileName, 13).c_str(), names.size(), names.empty() ? "" : names.front().c_str(), names.empty() ? "" : names.back().c_str());
    std::uint32_t result = 0;
    if (g_ui && (mode == kListLoad || mode == kListSave)) {
        open_dialog(param, mode, game);
        g_status = kStatusInit;
        return ret(c, 0);
    }
    switch (mode) {
        case kAutoLoad:
        case kLoad: result = do_load(param, game, save); break;
        case kAutoSave:
        case kSave: result = do_save(param, game, save); break;
        case kListLoad: {
            const nx::string slot = pick_slot(param, game, false);
            result = slot.empty() ? kErrorLoadNoData : do_load(param, game, slot);
            break;
        }
        case kListSave: {
            const nx::string slot = pick_slot(param, game, true);
            result = do_save(param, game, slot);
            if (result == 0) mem().write_string(param + kSaveName, slot);
            break;
        }
        case kListDelete:
        case kDelete: {
            const nx::string folder = save_folder(game, save);
            result = nx::fs::exists(folder) && nx::fs::remove_all(folder).has_value() ? 0 : kErrorDeleteNoData;
            break;
        }
        default:
            std::fprintf(stderr, "savedata: mode %u not implemented, reporting no data\n", mode);
            result = kErrorLoadNoData;
            break;
    }
    if (result && mode != kAutoLoad && mode != kListLoad && mode != kLoad)
        std::fprintf(stderr, "savedata: mode %u failed (0x%08X)\n", mode, result);
    mem().write32(param + kResult, result);
    g_status = kStatusFinished;
    ret(c, 0);
}
void GetStatus(Context& c) {
    const std::uint32_t s = g_status;
    if (g_status == kStatusShutdown) g_status = kStatusNone;
    if (g_status == kStatusInit && g_pending) g_status = kStatusRunning;  // the dialog is now on screen
    ret(c, s);
}
void ShutdownStart(Context& c) {
    g_status = kStatusShutdown;
    ret(c, 0);
}
void Update(Context& c) { ret(c, 0); }

constexpr HleBinding kBindings[] = {
    {"sceUtilitySavedataInitStart", InitStart},
    {"sceUtilitySavedataGetStatus", GetStatus},
    {"sceUtilitySavedataShutdownStart", ShutdownStart},
    {"sceUtilitySavedataUpdate", Update},
};

}  // namespace

void hle_install_savedata() { hle_bind(kBindings); }

void set_savedata_ui(bool enabled) { g_ui = enabled; }

const SaveDialog* pending_save_dialog() { return g_pending && !g_queued_answer ? &g_pending->dialog : nullptr; }

void answer_save_dialog(int slot) {
    if (!g_pending) return;
    if (g_deferred) {
        g_queued_answer = slot;
        return;
    }
    apply_answer(slot);
}

void set_save_dialog_deferred(bool deferred) { g_deferred = deferred; }

bool apply_save_dialog_answer(int& slot) {
    if (!g_queued_answer) return false;
    slot = *g_queued_answer;
    g_queued_answer.reset();
    apply_answer(slot);
    return true;
}

namespace {

void apply_answer(int slot) {
    if (!g_pending) return;
    const Pending p = std::move(*g_pending);
    g_pending.reset();
    std::uint32_t result = p.dialog.slots.empty() ? kErrorLoadNoData : kResultCancelled;
    if (slot >= 0 && static_cast<std::size_t>(slot) < p.dialog.slots.size()) {
        const nx::string& name = p.dialog.slots[static_cast<std::size_t>(slot)].name;
        if (p.mode == kListSave) {
            result = do_save(p.param, p.game, name);
            if (result == 0) mem().write_string(p.param + kSaveName, name);
        } else {
            result = do_load(p.param, p.game, name);
        }
    }
    mem().write32(p.param + kResult, result);
    g_status = kStatusFinished;
}

}  // namespace

}  // namespace ff7cc::rt
