// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/recomp_identity.h"
#include "core/arm/recomp/recomp_diagnostics.h"

namespace Core {
namespace {
std::atomic<bool> strict_mode{}, statistics{}, active{}, failed{};
std::mutex diagnostic_mutex;
std::string failure;
struct Counters {
    std::atomic<u64> aot_ns{}, jit_ns{}, blocks{}, entries{};
};
std::array<Counters, 4> counters;
using Site = std::tuple<std::size_t, u64, std::string, std::string, u64, u32>;
std::map<Site, u64> sites;
u64 dropped_sites{};
std::span<const RecompModuleIdentity> expected_modules;
std::filesystem::path input_dump;
nlohmann::json loaded_modules = nlohmann::json::array();
struct ModuleRange { std::string name; u64 base; u64 text_begin; u64 text_end; };
std::vector<ModuleRange> module_ranges;
} // namespace

bool RecompInputDumpEnabled() { return !input_dump.empty(); }
void FinishRecompInputDump(u64 program_id) {
    nlohmann::json manifest{{"version", 3}, {"program_id", program_id},
                            {"runtime_abi_version", RecompAbiVersion}};
    manifest["modules"] = nlohmann::json::array();
    for (const auto& entry : std::filesystem::directory_iterator(input_dump)) {
        if (!entry.is_directory()) continue;
        std::ifstream input{entry.path() / "module.json"};
        if (input) manifest["modules"].push_back(nlohmann::json::parse(input));
    }
    std::ofstream output{input_dump / "aot_manifest.json"};
    output << manifest.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("Cannot write input manifest");
}
void SetRecompInputDump(const std::string& directory) {
    input_dump = directory;
    if (!input_dump.empty()) {
        if (std::filesystem::exists(input_dump) && !std::filesystem::is_empty(input_dump))
            throw std::runtime_error("Input dump directory must be empty");
        std::filesystem::create_directories(input_dump);
    }
}
void SetRecompIdentities(std::span<const RecompModuleIdentity> identities) {
    expected_modules = identities; // Static storage, configured before process load.
}
bool ValidateRecompModule(std::string_view name,
                          const std::array<std::span<const u8>, 3>& segments,
                          u64 base, const std::array<u64, 3>& offsets) {
    if (expected_modules.empty() && !RecompStatisticsEnabled() && input_dump.empty()) return true;
    std::array<std::string, 3> hashes;
    for (std::size_t i = 0; i < hashes.size(); ++i) hashes[i] = RecompSha256(segments[i]);
    if (!expected_modules.empty()) {
        const auto it = std::find_if(expected_modules.begin(), expected_modules.end(),
            [&](const auto& mod) { return mod.name == name; });
        if (it == expected_modules.end()) {
            RecompFail("unexpected_module: " + std::string{name});
            return false;
        }
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            if (hashes[i] != it->sha256[i]) {
                RecompFail("module_hash_mismatch: " + std::string{name} + " segment=" + std::to_string(i));
                return false;
            }
        }
    }
    if (!input_dump.empty()) {
        // Names originate in the NSO loader; do not let a filename escape the destination.
        if (name.empty() || name.find_first_of("/\\") != std::string_view::npos || name == "..")
            throw std::runtime_error("Invalid module name for input dump");
        const auto dir = input_dump / std::string{name} / "data";
        std::filesystem::create_directories(dir);
        constexpr std::array names{"text", "rodata", "data"};
        nlohmann::json metadata{{"name", std::string{name}}, {"segment_sha256", hashes}};
        for (std::size_t i = 0; i < segments.size(); ++i) {
            std::ofstream output{dir / (std::string{names[i]} + ".bin"), std::ios::binary};
            output.write(reinterpret_cast<const char*>(segments[i].data()),
                         static_cast<std::streamsize>(segments[i].size()));
            output.close();
            if (!output) throw std::runtime_error("Cannot write module input dump");
            metadata[std::string{names[i]} + "_vaddr"] = offsets[i];
            metadata[std::string{names[i]} + "_size"] = segments[i].size();
        }
        std::ofstream output{dir.parent_path() / "module.json"};
        output << metadata.dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write module metadata");
    }
    std::scoped_lock lock{diagnostic_mutex};
    loaded_modules.push_back({{"name", std::string{name}}, {"sha256", hashes}, {"base", base}});
    module_ranges.push_back({std::string{name}, base, base + offsets[0], base + offsets[0] + segments[0].size()});
    return true;
}
bool AllRecompModulesValidated() {
    if (expected_modules.empty()) return true;
    if (loaded_modules.size() != expected_modules.size()) {
        RecompFail("recompiled_module_count_mismatch");
        return false;
    }
    return true;
}
std::string RecompModuleAt(u64 pc, u64& offset) {
    std::scoped_lock lock{diagnostic_mutex};
    for (const auto& mod : module_ranges) {
        if (pc >= mod.text_begin && pc < mod.text_end) {
            offset = pc - mod.base;
            return mod.name;
        }
    }
    offset = pc;
    return "unknown";
}

void ConfigureRecompDiagnostics(bool strict, bool stats) {
    std::scoped_lock lock{diagnostic_mutex};
    strict_mode = strict;
    statistics = stats;
    active = true;
    failed = false;
    failure.clear();
    sites.clear();
    dropped_sites = 0;
    for (auto& c : counters) {
        c.aot_ns = 0;
        c.jit_ns = 0;
        c.blocks = 0;
        c.entries = 0;
    }
    loaded_modules = nlohmann::json::array();
    module_ranges.clear();
    for (auto& c : counters) {
        c.aot_ns = 0; c.jit_ns = 0; c.blocks = 0; c.entries = 0;
    }
}
bool RecompStrict() { return strict_mode.load(std::memory_order_relaxed); }
bool RecompStatisticsEnabled() { return statistics.load(std::memory_order_relaxed); }
bool RecompHasFailure() { return failed.load(std::memory_order_acquire); }
std::string RecompFailureReason() {
    std::scoped_lock lock{diagnostic_mutex};
    return failure;
}
void RecompFail(std::string_view reason) {
    std::scoped_lock lock{diagnostic_mutex};
    if (!failed.load(std::memory_order_relaxed)) {
        failure = reason;
        failed.store(true, std::memory_order_release);
        LOG_CRITICAL(Core_ARM, "Execution failed: {}", reason);
    }
}
void RecompGuestPanic() {
    if (active.load(std::memory_order_relaxed)) RecompFail("guest_panic");
}
void RecordRecompExecution(std::size_t core, bool jit, u64 ns, u64 blocks) {
    if (!RecompStatisticsEnabled() || core >= counters.size()) return;
    auto& c = counters[core];
    (jit ? c.jit_ns : c.aot_ns).fetch_add(ns, std::memory_order_relaxed);
    c.blocks.fetch_add(blocks, std::memory_order_relaxed);
}
void RecordRecompFallback(std::size_t core, u64 pc, std::string_view reason,
                          std::string_view module, u64 offset, u32 opcode) {
    // Always on: the overlay lists these live, and the path is already the slow one.
    if (core < counters.size()) counters[core].entries.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{diagnostic_mutex};
    Site site{core, pc, std::string{reason}, std::string{module}, offset, opcode};
    auto it = sites.find(site);
    if (it != sites.end()) ++it->second;
    else if (sites.size() < 4096) sites.emplace(std::move(site), 1);
    else ++dropped_sites;
}
RecompSnapshot RecompDiagnosticsSnapshot() {
    RecompSnapshot snapshot;
    snapshot.statistics = RecompStatisticsEnabled();
    for (std::size_t i = 0; i < counters.size(); ++i) {
        snapshot.blocks[i] = counters[i].blocks.load(std::memory_order_relaxed);
    }
    std::scoped_lock lock{diagnostic_mutex};
    // The report keys sites by core and PC; the overlay wants one row per place.
    std::map<std::tuple<std::string, u64, std::string>, u64> merged;
    for (const auto& [site, count] : sites) {
        const auto& [core, pc, reason, module, offset, opcode] = site;
        merged[{module, offset, reason}] += count;
    }
    for (const auto& [key, count] : merged) {
        const auto& [module, offset, reason] = key;
        snapshot.sites.push_back({module, reason, offset, count});
    }
    snapshot.dropped_sites = dropped_sites;
    return snapshot;
}
void WriteRecompReport(const std::string& path, std::string_view termination,
                       std::string_view game, std::string_view user_dir, u64 program_id) {
    nlohmann::json report{{"schema_version", 1}, {"runtime_revision", std::string{Common::g_scm_rev}},
        {"strict", RecompStrict()}, {"backend", GetRecompLookup() ? "aot" : "jit"},
        {"termination", std::string{termination}}, {"game", std::string{game}}, {"user_dir", std::string{user_dir}},
        {"program_id", program_id}, {"timing_unit", "nanoseconds"},
        {"timing_scope", "host elapsed time in AOT blocks or JIT slices, summed per core; not instruction coverage"}};
    report["cores"] = nlohmann::json::array();
    for (std::size_t i = 0; i < counters.size(); ++i) {
        auto& c = counters[i];
        report["cores"].push_back({{"core", i}, {"aot_blocks", c.blocks.load()},
            {"aot_ns", c.aot_ns.load()}, {"jit_ns", c.jit_ns.load()},
            {"fallback_requests", c.entries.load()}});
    }
    {
        std::scoped_lock lock{diagnostic_mutex};
        report["failure"] = failure;
        report["recomp_abi_version"] = RecompAbiVersion;
        report["modules"] = loaded_modules;
        report["unrecorded_site_events"] = dropped_sites;
        report["fallback_sites"] = nlohmann::json::array();
        for (const auto& [site, count] : sites) {
            const auto& [core, pc, reason, module, offset, opcode] = site;
            report["fallback_sites"].push_back({{"core", core}, {"pc", pc}, {"reason", reason},
                {"module", module}, {"offset", offset}, {"opcode", opcode}, {"count", count}});
        }
    }
    const auto temporary = path + ".tmp";
    std::ofstream out{temporary, std::ios::trunc};
    out << report.dump(2) << '\n';
    out.close();
    if (!out) throw std::runtime_error("Cannot write recomp report: " + temporary);
    std::filesystem::rename(temporary, path);
}
} // namespace Core
