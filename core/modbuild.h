#pragma once

// Building a modded firmware from the user's stock .hxf and the chosen mod packages (editor roadmap step 6, stage E):
// which packages go in, where the file goes, what was done. The work itself is buildMods (core/dspbuild.h); the
// result is the same file tools/modpkg.py apply + tools/fwedit.py build make (tools/oracle/modbuild_ref.txt, in the firmware project).

#include "core/hxf.h"
#include "core/modspec.h"
#include "core/modstore.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hx::mods {

// The build tag's commit and dirty flag (MODTAG.md): this editor's source, filled in at configure time when the
// source is a git checkout; else "0000000" and dirty.
std::string toolCommit();
bool toolDirty();

// The packages for a build from the cache: the always-in ones, the chosen ids and everything they need. Each one is
// a cached package that matches the manifest (or, offline, the cached manifest), or an imported one.
// download: fetch (and check, and cache) listed packages that aren't cached yet. nullopt with a message when one
// isn't there or needs one that isn't anywhere.
std::optional<std::vector<Package>> choosePackages(const Store& store, const Stock& stock, const std::set<std::string>& ids,
                                                   std::string* error = nullptr, bool download = false);

// A record the build changed: its payload size in the stock file and in the build.
struct RecordChange {
    std::uint32_t record = 0;
    std::size_t before = 0, after = 0;
};
// One entry of the build history.
struct BuildEntry {
    std::string tag, sha256, path;
    std::string stockSha256;          // the base image it was built from ("" in lines from before 2026-11-13)
    std::vector<std::string> models;  // "sym=name" of its new models
};

struct BuildReport {
    std::string outPath, tag, sha256;
    std::size_t size = 0;
    std::string stockSha256;                  // the base image (Stock::imageSha256)
    std::vector<std::string> packages;        // "NAM block 1 (slot 0)"
    std::vector<std::string> models;          // "NAM: HD2_Nam, model number 383"
    std::vector<std::string> modelKeys;       // "HD2_Nam=NAM": for comparing builds (history)
    std::vector<std::uint32_t> changedRecords;
    std::vector<RecordChange> records;        // changedRecords with their sizes
    std::vector<std::string> dspLog, m4Log;   // the two stages' lines (patches with addresses, memory, ...)
    std::vector<std::string> log;             // the builder's own lines
    std::optional<BuildEntry> previous;       // the last build from the same base before this one
};

// The report in sections, for the Mods tab (kind: heading, line, warning) and the terminal (reportLines).
struct ReportItem {
    enum class Kind { Heading, Line, Warning } kind = Kind::Line;
    std::string text;
};
std::vector<ReportItem> reportItems(const BuildReport& r);

// Stock image + packages -> outPath (written atomically; never over the stock file). Throws BuildError (nothing is
// written then). commit / dirty: see toolCommit.
// progress (optional): 0..1 over the whole build (the M4 / DSP work, then packing, which is most of the time).
BuildReport buildFirmware(const HxfImage& stock, const Stock& stockInfo, const std::string& stockPath,
                          const std::vector<Package>& packages, const std::string& outPath,
                          const std::string& commit = toolCommit(), bool dirty = toolDirty(),
                          const BuildProgress& progress = {});

// Build history (<app data>/builds.txt, one line per build: tag, SHA-256, path, base image SHA-256, new models), written
// by buildFirmware. Same tag = same packages and tool, so the file is the same. findBuild: the newest recorded file
// with that tag that still exists, "" if none. lastBuild: the newest entry built from that base.
void recordBuild(const BuildReport& r);
std::string findBuild(const std::string& tag);
std::optional<BuildEntry> lastBuild(const std::string& stockSha256);

// Next to the stock file: "<dir>/<Unit> <version> + mods.hxf".
std::string defaultOutPath(const Stock& stock, const std::string& stockPath);

// reportItems as text lines: headings as "== Title", warnings as "! text", the rest indented.
std::vector<std::string> reportLines(const BuildReport& r);

} // namespace hx::mods
