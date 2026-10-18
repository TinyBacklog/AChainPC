// SPDX-License-Identifier: GPL-2.0-or-later
// Live data for the overlay's AOT and Log tabs: the recomp diagnostics
// snapshot (fallback sites always recorded, blocks only with statistics) and
// the ring of recent log lines.
#include <cstdio>
#include "common/recent_log.h"
#include "core/arm/recomp/recomp_diagnostics.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    {
        Core::ConfigureRecompDiagnostics(false, false);
        Core::RecordRecompFallback(0, 0x8000'8c20, "missing_block", "main", 0x8c20, 0xd503201f);
        Core::RecordRecompFallback(0, 0x8000'8c20, "missing_block", "main", 0x8c20, 0xd503201f);
        Core::RecordRecompFallback(2, 0x8000'9000, "unhandled_instruction", "sdk", 0x9000, 0);
        Core::RecordRecompExecution(0, false, 100, 5);
        const auto snapshot = Core::RecompDiagnosticsSnapshot();
        CHECK(!snapshot.statistics);
        CHECK(snapshot.blocks[0] == 0);              // blocks only with statistics
        CHECK(snapshot.sites.size() == 2);
        u64 main_count = 0;
        for (const auto& site : snapshot.sites) {
            if (site.module == "main" && site.offset == 0x8c20 && site.reason == "missing_block") {
                main_count = site.count;
            }
        }
        CHECK(main_count == 2);
        CHECK(snapshot.dropped_sites == 0);
    }
    {
        Core::ConfigureRecompDiagnostics(false, true);
        Core::RecordRecompExecution(1, false, 100, 5);
        Core::RecordRecompExecution(1, false, 100, 7);
        const auto snapshot = Core::RecompDiagnosticsSnapshot();
        CHECK(snapshot.statistics);
        CHECK(snapshot.blocks[1] == 12);
        CHECK(snapshot.sites.empty());               // a new session starts clean
    }
    {
        Common::RecentLog log(3);
        log.Add(Common::Log::Level::Info, "one");
        log.Add(Common::Log::Level::Warning, "two");
        log.Add(Common::Log::Level::Debug, "three");
        log.Add(Common::Log::Level::Error, "four");    // "one" falls out
        CHECK(log.Lines(10, Common::Log::Level::Trace) == std::vector<std::string>({"two", "three", "four"}));
        CHECK(log.Lines(10, Common::Log::Level::Warning) == std::vector<std::string>({"two", "four"}));
        CHECK(log.Lines(1, Common::Log::Level::Trace) == std::vector<std::string>({"four"}));
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("recomp snapshot and recent log passed\n");
    return 0;
}
