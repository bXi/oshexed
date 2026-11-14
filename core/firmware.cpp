#include "core/firmware.h"

#include <cstdio>

namespace hx {

namespace {

constexpr std::uint32_t kRecApp = 0, kRecMeta = 4, kRecGfxMeta = 8, kRecGfxData = 9, kRecPresets = 11, kRecNames = 12,
                        kRecSetlists = 13;
constexpr int kAmpCabCategory = 11;  // same models as Amp, carrying the linked cab
constexpr std::uint32_t kProductLt = 0x00210004;

std::unique_ptr<Firmware> fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return nullptr;
}

Bytes payloadOf(const HxfImage& img, std::uint32_t index) {
    const HxfRecord* r = img.record(index);
    return r ? r->payload : Bytes{};
}

void indexModels(Firmware& fw) {
    for (const CostEntry& c : fw.tables.costs) {
        ModelRef m;
        m.modelNo = c.modelNo;
        m.symId = c.symId;
        m.cost = c.cost;
        m.meta = fw.meta.bySymId(c.symId);
        m.name = m.meta ? fw.meta.displayName(*m.meta) : fw.meta.symbol(c.symId);
        fw.models.emplace(c.modelNo, std::move(m));
    }
    std::map<std::uint32_t, ModelRef*> bySym;
    for (auto& [no, m] : fw.models) bySym.emplace(m.symId, &m);
    for (std::size_t li = 0; li < fw.tables.lists.size(); ++li) {
        const BrowserList& l = fw.tables.lists[li];
        if (l.category == kAmpCabCategory) continue;
        for (std::size_t ei = 0; ei < l.entries.size(); ++ei) {
            const auto it = bySym.find(l.entries[ei].symId);
            if (it == bySym.end() || it->second->list >= 0) continue;  // first listing wins
            ModelRef& m = *it->second;
            m.name = l.entries[ei].name;
            m.category = l.category;
            m.list = int(li);
            m.entry = int(ei);
        }
    }
}

} // namespace

const ModelRef* Firmware::model(std::uint16_t modelNo) const {
    const auto it = models.find(modelNo);
    return it == models.end() ? nullptr : &it->second;
}

const ModelRef* Firmware::modelBySym(std::uint32_t symId) const {
    for (const auto& [no, m] : models)
        if (m.symId == symId) return &m;
    return nullptr;
}

const MetaEntry* Firmware::metaBySymbol(std::string_view symbol) const {
    for (const MetaEntry& e : meta.entries())
        if (meta.symbol(e.symId) == symbol) return &e;
    return nullptr;
}

std::uint32_t Firmware::pathCost(const PresetPath& path) const {
    // The unit's running total starts at 1500 (FUN_281239b0 sets dspObj+0xa88 = 0x5dc + the blocks' costs), so an
    // empty path is already 1500 of the 9000.
    std::uint32_t sum = kDspBaseCost;
    // A linked cab (amp+cab, dual cab) is a model of its own and costs its own entry (assumed: the unit's sum isn't
    // decompiled for linked models yet).
    for (const PresetLane* lane : {&path.a, &path.b})
        for (const auto& b : lane->slots)
            if (b) {
                if (const ModelRef* m = model(b->modelNo)) sum += m->cost;
                if (b->cabModelNo >= 0)
                    if (const ModelRef* c = model(std::uint16_t(b->cabModelNo))) sum += c->cost;
            }
    return sum;
}

std::unique_ptr<Firmware> loadFirmware(const std::string& path, std::string* error) {
    auto fw = std::make_unique<Firmware>();
    fw->path = path;
    std::string err;
    fw->hxf = loadHxfFile(path, &err);
    if (!fw->hxf) return fail(error, err);

    if (fw->hxf->productId != kProductLt) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "product ID %#010x is not a Helix LT", fw->hxf->productId);
        fw->warnings.push_back(buf);
    }
    if (!fw->hxf->md5Ok) fw->warnings.push_back("HEAD MD5 does not match the image");
    for (const HxfRecord& r : fw->hxf->records) {
        fw->checks.push_back(checkRecord(r));
        if (!fw->checks.back().all()) fw->warnings.push_back("record " + std::to_string(r.index) + " fails its checks");
    }

    const HxfRecord* app = fw->hxf->record(kRecApp);
    const HxfRecord* meta = fw->hxf->record(kRecMeta);
    if (!app || !meta) return fail(error, "record 0 (M4 app) or 4 (DSP metadata) missing");

    std::optional<AppImage> image = parseApp(app->payload, &err);
    if (!image) return fail(error, "record 0: " + err);
    fw->app = std::move(*image);
    fw->identity = identifyFirmware(fw->hxf->imageSha256, fw->app);  // stock, or one of our builds (MODTAG.md)
    // Supported: Helix LT 3.80.0, stock or one of our builds from it (every address the editor reads is that build's).
    if (fw->identity.version != "3.80.0" || fw->identity.kind == FirmwareIdentity::Kind::Other)
        fw->warnings.push_back("Not a supported firmware: this editor supports Helix LT 3.80.0 (stock, or a build made "
                               "from it with these tools). Models, lists and presets read from this file may be wrong.");

    std::optional<DspMeta> m = DspMeta::parse(meta->payload, &err);
    if (!m) return fail(error, "record 4: " + err);
    fw->meta = std::move(*m);

    std::optional<ModelTables> t = readModelTables(fw->app, &err);
    if (!t) return fail(error, err);
    fw->tables = std::move(*t);
    labelLists(fw->tables, fw->meta);
    indexModels(*fw);

    const HxfImage& img = *fw->hxf;
    fw->presets = readPresets(payloadOf(img, kRecPresets), payloadOf(img, kRecNames), payloadOf(img, kRecSetlists),
                              &fw->warnings);

    fw->gfx = Graphics::parse(payloadOf(img, kRecGfxMeta), payloadOf(img, kRecGfxData), &err);
    if (!fw->gfx) fw->warnings.push_back("graphics (rec 8/9): " + err);
    else if (!(fw->icons = readCategoryIcons(fw->app, *fw->gfx, &err))) fw->warnings.push_back(err);
    return fw;
}

} // namespace hx
