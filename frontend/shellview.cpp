#include "frontend/shellview.h"

#include "core/firmware.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace editor {

namespace {

// Tab order in the rail (top to bottom, F1..). Settings sits at the bottom but still counts.
constexpr const char *kPages[] = {"presets", "irs", "helix", "firmware", "mods", "monitor", "settings"};
constexpr const char *kFirmwareTabs[] = {"base", "unit", "file", "flash"};

using IdKind = hx::FirmwareIdentity::Kind;

// What a firmware identity says, as a card. unit: it describes a connected unit (a refusal of command 200 there
// means stock or an older build, not "unknown").
// What each registry slot brings (MODTAG.md); the card shows it under the mod's name.
std::string modCovers(int slot) {
    switch (slot) {
    case 0: return "NAM capture block + its title on the unit's screen";
    case 1: return "polyphonic guitar to MIDI";
    default: return "";
    }
}

void fillCard(ShellView::Card &c, const hx::FirmwareIdentity &id, bool unit) {
    c.rows.clear();
    c.mods.clear();
    switch (id.kind) {
    case IdKind::Stock:
        c.headline = "Stock firmware";
        c.detail = "Line 6 " + id.version;
        c.tone = "stock";
        break;
    case IdKind::Ours:
        c.headline = "Our build";
        c.detail = std::to_string(id.build->mods.size() + 1) + " mods in it" +
                   (id.build->preGit() ? ", built before the project was under git" : ", commit " + id.build->commit) +
                   (id.build->dirty && !id.build->preGit() ? " with uncommitted changes" : "");
        c.tone = "ours";
        break;
    case IdKind::Damaged:
        c.headline = "Damaged build tag";
        c.detail = id.summary;
        c.tone = "bad";
        break;
    case IdKind::Other:
        c.headline = unit ? "Stock or older build" : "Unknown firmware";
        c.detail = unit ? "It doesn't answer the build-tag command, so it's stock firmware or one of our builds from "
                          "before that command."
                        : "Not stock and not built by these tools.";
        c.tone = unit ? "stock" : "bad";
        break;
    }
    if (!id.version.empty() && id.kind != IdKind::Stock) c.rows.push_back({"Based on", id.version});
    if (id.build) {
        const hx::ModTag &b = *id.build;
        const auto in = [&](int slot) { return std::find(b.mods.begin(), b.mods.end(), slot) != b.mods.end(); };
        // Always in every build (MODTAG.md), so never "off".
        c.mods.push_back({"Custom category", "our models' browser category (always in our builds)"});
        for (int slot : hx::knownModSlots()) c.mods.push_back({hx::modSlotName(slot), modCovers(slot), !in(slot)});
        for (int slot : b.mods)
            if (hx::modSlotName(slot).rfind("slot ", 0) == 0)
                c.mods.push_back({hx::modSlotName(slot), "unknown to this editor: built by a newer tool"});
        c.rows.push_back({"Commit", b.preGit() ? "none: built before the project was under git" : b.commit});
        c.rows.push_back({"Source", b.preGit()  ? "unknown (no git yet)" :
                                    b.dirty     ? "uncommitted changes: the commit alone doesn't rebuild it" :
                                                  "clean: the commit rebuilds it exactly",
                          b.dirty && !b.preGit()});
        c.rows.push_back({"Mod slots", b.mods.empty() ? "none" : [&] {
                              std::string s;
                              for (int m : b.mods) s += (s.empty() ? "" : ", ") + std::to_string(m);
                              return s;
                          }()});
        c.rows.push_back({"Tag layout", std::to_string(b.layout)});
    }
    if (!id.tag.empty()) c.rows.push_back({"Tag", id.tag, id.kind == IdKind::Damaged});
}

std::string hex(std::uint32_t v, int digits = 8) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*x", digits, unsigned(v));
    return buf;
}

std::string bytes(std::size_t n) {
    char buf[32];
    if (n >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", double(n) / (1024 * 1024));
    else if (n >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(n) / 1024);
    else std::snprintf(buf, sizeof buf, "%zu B", n);
    return buf;
}

} // namespace

void ShellView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<InfoRow>()) {
        s.RegisterMember("label", &InfoRow::label);
        s.RegisterMember("value", &InfoRow::value);
        s.RegisterMember("bad", &InfoRow::bad);
    }
    model.RegisterArray<std::vector<InfoRow>>();
    if (auto s = model.RegisterStruct<Card>()) {
        s.RegisterMember("headline", &Card::headline);
        s.RegisterMember("detail", &Card::detail);
        s.RegisterMember("tone", &Card::tone);
        s.RegisterMember("rows", &Card::rows);
        s.RegisterMember("mods", &Card::mods);
    }
    if (auto s = model.RegisterStruct<RecordRow>()) {
        s.RegisterMember("name", &RecordRow::name);
        s.RegisterMember("size", &RecordRow::size);
        s.RegisterMember("crc", &RecordRow::crc);
        s.RegisterMember("checks", &RecordRow::checks);
        s.RegisterMember("ok", &RecordRow::ok);
    }
    model.RegisterArray<std::vector<RecordRow>>();
    model.RegisterArray<std::vector<std::string>>();

    model.Bind("page", &page_);
    model.Bind("rail_open", &railOpen_);
    model.Bind("fw_path", &fwPath_);
    model.Bind("fw_info", &fwInfo_);
    model.Bind("fw_warnings", &fwWarnings_);
    model.Bind("fw_records", &fwRecords_);
    model.Bind("fw_tab", &fwTab_);
    model.Bind("helix_tab", &helixTab_);
    model.BindEventCallback("pick_helix_tab", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        const std::string tab = a[0].Get<Rml::String>();
        if ((tab != "eq" && tab != "settings") || tab == helixTab_) return;
        helixTab_ = tab;
        if (handle_) handle_.DirtyVariable("helix_tab");
    });
    model.Bind("unit_card", &unitCard_);
    model.Bind("file_card", &fileCard_);

    model.BindEventCallback("pick_fw_tab", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) setFirmwareTab(a[0].Get<Rml::String>());
    });
    // open_url('https://...'): a link in a page, opened in the system's browser (only http(s) links).
    model.BindEventCallback("open_url", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        const std::string url = a[0].Get<Rml::String>();
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) SDL_OpenURL(url.c_str());
    });
    // The Firmware tab's notice: the tab stays covered until "I understand".
    model.Bind("fw_notice", &noticeShown_);
    model.Bind("fw_notice_no_more", &noticeNoMore_);
    model.BindEventCallback("fw_notice_toggle", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        noticeNoMore_ = !noticeNoMore_;
        if (handle_) handle_.DirtyVariable("fw_notice_no_more");
    });
    model.BindEventCallback("fw_notice_ok", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        noticeShown_ = false;
        noticeDone_ = true;
        if (handle_) handle_.DirtyVariable("fw_notice");
        if (noticeNoMore_) {
            noticeEnabled_ = false;
            changed();
        }
    });

    model.BindEventCallback("pick_page", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        setPage(a[0].Get<Rml::String>());
        changed();
    });
    model.BindEventCallback("toggle_rail", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        setRailOpen(!railOpen_);
        changed();
    });
    handle_ = model.GetModelHandle();
}

void ShellView::setFirmwareTab(const std::string &tab) {
    if (std::find(std::begin(kFirmwareTabs), std::end(kFirmwareTabs), tab) == std::end(kFirmwareTabs) || tab == fwTab_)
        return;
    fwTab_ = tab;
    if (handle_) handle_.DirtyVariable("fw_tab");
}

void ShellView::setPage(const std::string &page) {
    if (page == "flash") {  // the old Flash tab (an older settings.ini): now part of Firmware
        setFirmwareTab("flash");
        setPage("firmware");
        return;
    }
    if (std::find(std::begin(kPages), std::end(kPages), page) == std::end(kPages) || page == page_) return;
    page_ = page;
    if (handle_) handle_.DirtyVariable("page");
    showNoticeIfDue();
}

void ShellView::showNoticeIfDue() {
    if (page_ != "firmware" || !noticeEnabled_ || noticeDone_ || noticeShown_) return;
    noticeShown_ = true;
    if (handle_) handle_.DirtyVariable("fw_notice");
}

void ShellView::setRailOpen(bool open) {
    if (open == railOpen_) return;
    railOpen_ = open;
    if (handle_) handle_.DirtyVariable("rail_open");
}

void ShellView::pickTab(int n) {
    if (n < 0 || n >= int(std::size(kPages)) || page_ == kPages[n]) return;
    setPage(kPages[n]);
    changed();
}

void ShellView::changed() {
    if (onChanged) onChanged();
}

void ShellView::setUnit(const hx::FirmwareIdentity *id) {
    if (id) fillCard(unitCard_, *id, true);
    else unitCard_ = Card{};
    if (handle_) handle_.DirtyVariable("unit_card");
}

void ShellView::setFirmware(const hx::Firmware *fw) {
    fwInfo_.clear();
    fwWarnings_.clear();
    fwRecords_.clear();
    fwPath_ = fw ? fw->path : "";
    if (fw && fw->hxf) {
        const hx::HxfImage &img = *fw->hxf;
        const std::string product = hex(img.productId) + (img.productId == 0x00210004 ? "  (Helix LT)" : "");
        fillCard(fileCard_, fw->identity, false);
        fwInfo_ = {
            {"File", fw->path},
            {"Product", product},
            {"File size", bytes(img.fileSize)},
            {"Image", bytes(img.image.size()) + ", " + std::to_string(img.records.size()) + " records"},
            {"Image MD5", img.md5Ok ? "ok" : "BAD", !img.md5Ok},
            {"M4 app", "entry " + hex(fw->app.entry) + ", " + std::to_string(fw->app.segments.size()) + " segments"},
            {"Model data", std::to_string(fw->meta.symbolCount()) + " symbols, " +
                               std::to_string(fw->meta.entries().size()) + " entries"},
            {"Models", std::to_string(fw->models.size()) + " in the cost table, " +
                           std::to_string(fw->tables.lists.size()) + " browser lists"},
            {"Setlists", std::to_string(fw->presets.setlists.size()) + " setlists, " +
                             std::to_string(fw->presets.presets.size()) + " preset slots"},
        };
        fwWarnings_ = fw->warnings;
        for (std::size_t i = 0; i < img.records.size(); ++i) {
            const hx::HxfRecord &r = img.records[i];
            RecordRow row;
            row.name = std::to_string(r.index) + "  " + hx::recordName(r.index);
            row.size = bytes(r.size);
            row.crc = hex(r.crc);
            if (i < fw->checks.size()) {
                const hx::RecordChecks &c = fw->checks[i];
                row.ok = c.all();
                std::string failed;
                auto add = [&](bool ok, const char *what) {
                    if (!ok) failed += failed.empty() ? what : std::string(", ") + what;
                };
                add(c.crc, "crc");
                add(c.words, "words");
                add(c.hash8, "hash8");
                if (c.inner != hx::InnerKind::None) add(c.innerOk, c.inner == hx::InnerKind::App ? "app" : "ui");
                row.checks = failed.empty() ? "ok" : "BAD: " + failed;
            }
            fwRecords_.push_back(std::move(row));
        }
    }
    if (!fw || !fw->hxf) fileCard_ = noFileCard();
    if (handle_) {
        handle_.DirtyVariable("file_card");
        handle_.DirtyVariable("fw_path");
        handle_.DirtyVariable("fw_info");
        handle_.DirtyVariable("fw_warnings");
        handle_.DirtyVariable("fw_records");
    }
}

} // namespace editor
