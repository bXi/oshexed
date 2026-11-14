#include "core/nam.h"

#include "core/ir.h"
#include "core/json.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace hx {

namespace {

// nam2wav.py format v1 and namdsp.py's A2 Lite structure.
constexpr int kHeaderWords = 8, kUsable = 1928, kChannels = 3, kHeadKernel = 16, kWeights = 1871;
constexpr float kMagic0 = 0.6180339887f, kMagic1 = -0.3819660113f;
constexpr double kLimit = 4.0, kSlope = 0.01;
constexpr int kKernels[] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
constexpr int kDilations[] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};

bool allOf(const json::Value* list, auto pred) {
    if (!list || !list->isArray()) return false;
    return std::all_of(list->items.begin(), list->items.end(), pred);
}

// A metadata number, 0 when missing, not a number (booleans don't count) or not finite.
double metaNumber(const json::Value* meta, std::string_view key) {
    const json::Value* v = meta ? meta->get(key) : nullptr;
    return v && v->isNumber() && std::isfinite(v->n) ? v->n : 0.0;
}

} // namespace

std::optional<NamIr> namToIr(std::string_view text, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<NamIr> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::string parseError;
    const std::optional<json::Value> doc = json::parse(text, &parseError);
    if (!doc || !doc->isObject()) return fail("not a .nam file (" + (parseError.empty() ? "not JSON" : parseError) + ")");

    // The 3-channel WaveNet: one of a SlimmableContainer's sub-models, or the file itself.
    std::vector<const json::Value*> models;
    if (doc->get("architecture") && doc->get("architecture")->asString() == "SlimmableContainer") {
        const json::Value* cfg = doc->get("config");
        if (const json::Value* subs = cfg ? cfg->get("submodels") : nullptr; subs && subs->isArray())
            for (const json::Value& s : subs->items)
                if (const json::Value* m = s.get("model")) models.push_back(m);
    } else {
        models.push_back(&*doc);
    }
    const json::Value* m = nullptr;
    for (const json::Value* c : models) {
        const json::Value* cfg = c->get("config");
        const json::Value* layers = cfg ? cfg->get("layers") : nullptr;
        const json::Value* first = layers && layers->isArray() && !layers->items.empty() ? &layers->items[0] : nullptr;
        if (c->get("architecture") && c->get("architecture")->asString() == "WaveNet" && first &&
            first->get("channels") && first->get("channels")->asInt() == kChannels) {
            m = c;
            break;
        }
    }
    if (!m) return fail("no 3-channel WaveNet in it: only the small (\"A2 Lite\") model of a slimmable capture fits");

    // The features our kernel runs (namref.load + namdsp.check_structure).
    const json::Value* cfg = m->get("config");
    const json::Value* layers = cfg->get("layers");
    if (layers->items.size() != 1 || (cfg->get("head") && cfg->get("head")->type != json::Type::Null))
        return fail("unsupported: more than one layer array, or a WaveNet-level head");
    const json::Value& la = layers->items[0];
    auto is = [&](std::string_view key, auto value) {
        const json::Value* v = la.get(key);
        return v && v->asNumber() == double(value);
    };
    auto sub = [&](std::string_view a, std::string_view b) -> const json::Value* {
        const json::Value* x = la.get(a);
        return x ? x->get(b) : nullptr;
    };
    std::vector<std::string> bad;
    if (!allOf(la.get("gating_mode"), [](const json::Value& g) { return g.asString() == "none"; })) bad.push_back("gating");
    if (!allOf(la.get("activation"), [](const json::Value& a) {
            const json::Value* slope = a.isObject() ? a.get("negative_slope") : nullptr;
            return a.get("type") && a.get("type")->asString() == "LeakyReLU" && slope &&
                   std::abs(slope->asNumber() - kSlope) < 1e-12;
        }))
        bad.push_back("activation");
    for (std::size_t i = 0; i < la.keys.size(); ++i)
        if (la.keys[i].ends_with("film") && la.items[i].get("active") && la.items[i].get("active")->asBool())
            bad.push_back("FiLM");
    if (!(sub("layer1x1", "active") && sub("layer1x1", "active")->asBool() && sub("layer1x1", "groups") &&
          sub("layer1x1", "groups")->asInt() == 1))
        bad.push_back("layer1x1");
    if (sub("head1x1", "active") && sub("head1x1", "active")->asBool()) bad.push_back("head1x1");
    if (!is("bottleneck", kChannels)) bad.push_back("bottleneck");
    if (!is("input_size", 1) || !is("condition_size", 1)) bad.push_back("input / condition size");
    if (!is("groups_input", 1) || !is("groups_input_mixin", 1)) bad.push_back("groups");
    if (!(sub("head", "out_channels") && sub("head", "out_channels")->asInt() == 1 && sub("head", "bias") &&
          sub("head", "bias")->asBool()))
        bad.push_back("head");
    if (!(sub("head", "kernel_size") && sub("head", "kernel_size")->asInt() == kHeadKernel)) bad.push_back("head size");
    if (!allOf(la.get("secondary_activation"), [](const json::Value& s) { return s.type == json::Type::Null; }))
        bad.push_back("secondary activation");
    auto sameList = [&](std::string_view key, const int* want, std::size_t n) {
        const json::Value* v = la.get(key);
        if (!v || !v->isArray() || v->items.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (v->items[i].asInt() != want[i]) return false;
        return true;
    };
    if (!sameList("kernel_sizes", kKernels, std::size(kKernels)) || !sameList("dilations", kDilations, std::size(kDilations)))
        bad.push_back("kernel sizes / dilations");
    if (const json::Value* sr = m->get("sample_rate"); sr && sr->type != json::Type::Null && sr->asNumber() != 48000)
        bad.push_back("sample rate " + std::to_string(sr->asInt()));
    if (!bad.empty()) {
        std::string list;
        for (const std::string& b : bad) list += (list.empty() ? "" : ", ") + b;
        return fail("not the A2 Lite structure the NAM block runs (" + list + ")");
    }
    const json::Value* wv = m->get("weights");
    if (!wv || !wv->isArray() || int(wv->items.size()) != kWeights)
        return fail("expected " + std::to_string(kWeights) + " weights, the file has " +
                    std::to_string(wv && wv->isArray() ? wv->items.size() : 0));

    // pack(): float32 weights, denormals to 0, scaled by the smallest 2^e that brings every |w| within 4.
    NamIr out;
    std::vector<float> w(wv->items.size());
    float peak = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double d = wv->items[i].asNumber();
        if (!wv->items[i].isNumber() || !std::isfinite(d)) return fail("the weights contain a value that isn't a number");
        float f = float(d);
        if (f != 0 && std::abs(f) < FLT_MIN) {
            f = 0;
            ++out.flushed;
        }
        w[i] = f;
        peak = std::max(peak, std::abs(f));
    }
    while (double(peak) / std::ldexp(1.0, out.exponent) > kLimit) ++out.exponent;
    std::uint64_t sum = 0;
    out.samples.assign(kIrSamples, 0.f);
    for (std::size_t i = 0; i < w.size(); ++i) {
        const float s = std::ldexp(w[i], -out.exponent);
        if (std::ldexp(s, out.exponent) != w[i]) return fail("a weight can't be scaled exactly (too small)");
        out.samples[std::size_t(kHeaderWords) + i] = s;
        sum += std::bit_cast<std::uint32_t>(s);
    }
    const json::Value* meta = doc->get("metadata");
    out.weights = int(w.size());
    out.levelDbu = metaNumber(meta, "input_level_dbu");
    out.loudness = metaNumber(meta, "loudness");
    if (meta && meta->get("name")) out.name = meta->get("name")->asString();
    if (meta && meta->get("gear_type")) out.gearType = meta->get("gear_type")->asString();
    if (meta && meta->get("modeled_by")) out.modeledBy = meta->get("modeled_by")->asString();
    if (meta && meta->get("gear_make")) out.gearMake = meta->get("gear_make")->asString();
    if (meta && meta->get("gear_model")) out.gearModel = meta->get("gear_model")->asString();
    if (meta && meta->get("tone_type")) out.toneType = meta->get("tone_type")->asString();
    if (const json::Value* sr = doc->get("sample_rate"); sr && sr->isNumber()) out.sampleRate = sr->n;
    auto clamp = [](double x) { return std::clamp(x, -64.0, 64.0); };
    const float header[kHeaderWords] = {kMagic0,
                                        kMagic1,
                                        1.0f,
                                        float(out.exponent / 16.0),
                                        float(out.weights / 1024.0),
                                        float(clamp(out.levelDbu) / 16.0),
                                        float(clamp(out.loudness) / 16.0),
                                        float(double(sum & 0xffff) / 65536.0)};
    std::copy(std::begin(header), std::end(header), out.samples.begin());
    static_assert(kHeaderWords + kWeights <= kUsable);
    return out;
}

std::string namGearHint(const std::string& gearType) {
    if (gearType == "amp_cab") return "amp + cab capture: use it without a cab block";
    if (gearType == "amp") return "amp capture (no cab): add a cab or IR after it";
    if (gearType == "pedal") return "pedal capture";
    if (gearType == "pedal_amp") return "pedal + amp capture (no cab): add a cab or IR after it";
    if (gearType == "preamp") return "preamp capture: add a power amp / cab after it";
    return "";
}

std::optional<NamHeader> namHeader(const float* w, std::size_t n) {
    if (n < 7 || w[0] != kMagic0 || w[1] != kMagic1) return std::nullopt;
    NamHeader h;
    h.version = int(std::lround(w[2]));
    h.exponent = int(std::lround(double(w[3]) * 16.0));
    h.weights = int(std::lround(double(w[4]) * 1024.0));
    h.levelDbu = double(w[5]) * 16.0;
    h.loudness = double(w[6]) * 16.0;
    if (h.version < 1) return std::nullopt;
    return h;
}

std::string namSlotName(const std::string& gearType, const std::string& name) {
    std::string prefix;
    if (gearType == "pedal") prefix = "P";
    else if (gearType == "amp") prefix = "A";
    else if (gearType == "amp_cab") prefix = "AC";
    else if (gearType == "pedal_amp") prefix = "PA";
    else if (gearType == "amp_pedal_cab" || gearType == "pedal_amp_cab") prefix = "PAC";
    else if (gearType == "preamp") prefix = "Pre";
    return irName(prefix.empty() ? name : prefix + " " + name);
}

} // namespace hx
