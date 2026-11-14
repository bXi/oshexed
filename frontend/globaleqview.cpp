#include "frontend/globaleqview.h"

#include "frontend/firmwaretextures.h"
#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <numbers>

namespace editor {

namespace {

// The 12 numeric settings in cmd 76's order (HX Edit's EQLowFreq .. EQLevel), with the ranges the editor allows
// (the unit has the last word: its notice shows what it took). Low cut 19.9 Hz and high cut 20.1 kHz are "off".
enum class Kind { Freq, Q, Gain, LowCut, HighCut, Level };
struct EqField {
    const char *label;
    Kind kind;
    double lo, hi;
};
constexpr EqField kFields[] = {
    {"Freq", Kind::Freq, 20, 20000},  {"Q", Kind::Q, 0.1, 10},  {"Gain", Kind::Gain, -12, 12},
    {"Freq", Kind::Freq, 20, 20000},  {"Q", Kind::Q, 0.1, 10},  {"Gain", Kind::Gain, -12, 12},
    {"Freq", Kind::Freq, 20, 20000},  {"Q", Kind::Q, 0.1, 10},  {"Gain", Kind::Gain, -12, 12},
    {"Freq", Kind::LowCut, 19.9, 1000}, {"Freq", Kind::HighCut, 500, 20100}, {"Level", Kind::Level, -60, 12},
};
constexpr int kApplyIndex = 12;  // 1 = 1/4", 2 = XLR, 3 = 1/4" + XLR (HX Edit's Apply menu)
constexpr int kLowCut = 9, kHighCut = 10, kLevel = 11;
constexpr double kLowCutOff = 19.9, kHighCutOff = 20100;
// What Reset puts back: the values a fresh unit had (NOTES "Global EQ": 110 / 0.707 / 0, 2000, 8000, cuts off, 0 dB).
constexpr double kDefaults[] = {110, 0.707, 0, 2000, 0.707, 0, 8000, 0.707, 0, kLowCutOff, kHighCutOff, 0};

// The graph: 20 Hz .. 20 kHz on a log scale across, +14 .. -14 dB down; the curve without the output level.
constexpr double kFMin = 20, kFMax = 20000, kDbTop = 14, kDbBottom = -14, kFs = 48000;
constexpr double kFlushInterval = 0.05;  // s between sends of a value being dragged
constexpr double kEchoTimeout = 5.0;     // s: after this an echo we wait for counts as lost, and notices apply again

// The curve as an anti-aliased line in an RGBA image (premultiplied): each pixel's coverage from its distance to the
// nearest piece of the polyline through `pts` (px), `width` px thick.
std::vector<std::uint8_t> drawLine(const std::vector<std::pair<double, double>> &pts, int w, int h, double width,
                                   std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<float> cover(std::size_t(w) * std::size_t(h), 0.f);
    const double half = width / 2;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const auto [ax, ay] = pts[i - 1];
        const auto [bx, by] = pts[i];
        const int x0 = std::max(0, int(std::floor(std::min(ax, bx) - half - 1)));
        const int x1 = std::min(w - 1, int(std::ceil(std::max(ax, bx) + half + 1)));
        const int y0 = std::max(0, int(std::floor(std::min(ay, by) - half - 1)));
        const int y1 = std::min(h - 1, int(std::ceil(std::max(ay, by) + half + 1)));
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5 - ax, py = y + 0.5 - ay;
                const double t = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0.0;
                const double d = std::hypot(px - t * dx, py - t * dy);
                const float c = float(std::clamp(half + 0.5 - d, 0.0, 1.0));
                float &dst = cover[std::size_t(y) * std::size_t(w) + std::size_t(x)];
                dst = std::max(dst, c);
            }
    }
    std::vector<std::uint8_t> rgba(cover.size() * 4);
    for (std::size_t i = 0; i < cover.size(); ++i) {
        const float a = cover[i];
        rgba[i * 4 + 0] = std::uint8_t(r * a + 0.5f);
        rgba[i * 4 + 1] = std::uint8_t(g * a + 0.5f);
        rgba[i * 4 + 2] = std::uint8_t(b * a + 0.5f);
        rgba[i * 4 + 3] = std::uint8_t(255 * a + 0.5f);
    }
    return rgba;
}

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Rml::Element *graphElement() {
    Rml::Context *ctx = RmlUI::GetContext();
    if (!ctx) return nullptr;
    for (int i = 0; i < ctx->GetNumDocuments(); ++i)
        if (Rml::Element *e = ctx->GetDocument(i)->GetElementById("eq-graph")) return e;
    return nullptr;
}

double logPos(double v, double lo, double hi) { return std::log(std::clamp(v, lo, hi) / lo) / std::log(hi / lo); }
double logVal(double p, double lo, double hi) { return lo * std::pow(hi / lo, std::clamp(p, 0.0, 1.0)); }
double xOf(double f) { return logPos(f, kFMin, kFMax); }
double yOf(double db) { return (kDbTop - std::clamp(db, kDbBottom, kDbTop)) / (kDbTop - kDbBottom); }

std::string pct(double v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.3f%%", v * 100.0);
    return buf;
}

std::string fmt(const char *f, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

bool isOff(int i, double v) {
    return (i == kLowCut && v <= kLowCutOff + 0.05) || (i == kHighCut && v >= kHighCutOff - 1);
}

// As HX Edit shows them: "216 Hz", "1.4 kHz", "0.71", "+12.0 dB", "Off".
std::string text(int i, double v) {
    if (isOff(i, v)) return "Off";
    switch (kFields[i].kind) {
    case Kind::Freq:
    case Kind::LowCut:
    case Kind::HighCut: return v < 1000 ? fmt("%.0f Hz", v) : fmt("%.1f kHz", v / 1000);
    case Kind::Q: {
        std::string s = fmt("%.2f", v);
        while (s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
        return s;
    }
    case Kind::Gain:
    case Kind::Level: return std::abs(v) < 0.05 ? "0.0 dB" : fmt("%+.1f dB", v);
    }
    return {};
}

// The bar: log for frequencies and Q, linear for gains.
double posOf(int i, double v) {
    const EqField &f = kFields[i];
    return f.kind == Kind::Gain || f.kind == Kind::Level ? (v - f.lo) / (f.hi - f.lo) : logPos(v, f.lo, f.hi);
}

double valueAt(int i, double p) {
    const EqField &f = kFields[i];
    const double v = f.kind == Kind::Gain || f.kind == Kind::Level ? f.lo + std::clamp(p, 0.0, 1.0) * (f.hi - f.lo)
                                                                    : logVal(p, f.lo, f.hi);
    switch (f.kind) {
    case Kind::Q: return std::round(v * 100) / 100;
    case Kind::Gain:
    case Kind::Level: return std::round(v * 10) / 10;
    case Kind::LowCut: return p <= 0.002 ? kLowCutOff : std::round(v);
    case Kind::HighCut: return p >= 0.998 ? kHighCutOff : std::round(v);
    default: return std::round(v);
    }
}

// One biquad's gain in dB at f (RBJ cookbook coefficients).
struct Biquad {
    double b0, b1, b2, a0, a1, a2;
    double db(double f) const {
        const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / kFs), z2 = z1 * z1;
        const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2);
        return 20.0 * std::log10(std::max(std::abs(h), 1e-9));
    }
};

// A peak as 1 + (G - 1) * bandpass(Q): boosts are broad, cuts narrow and deep (not mirror images). This matches HX
// Edit's curve (screens/eq.png, 216 Hz +12 / 1.4 kHz -11.8 / 6.7 kHz -12, Q 0.7: within ~0.5 dB from 50 Hz to 4 kHz);
// the symmetric cookbook peak was off by up to 6 dB there.
Biquad peaking(double f0, double q, double gainDb) {
    const double g = std::pow(10.0, gainDb / 20.0), w = 2.0 * std::numbers::pi * f0 / kFs;
    const double alpha = std::sin(w) / (2.0 * std::max(q, 0.01)), c = std::cos(w);
    return {1 + alpha + (g - 1) * alpha, -2 * c, 1 - alpha - (g - 1) * alpha, 1 + alpha, -2 * c, 1 - alpha};
}

// The cuts: 12 dB / octave Butterworths, placed at 1.3x (low cut) / 0.8x (high cut) the shown frequency. Fitted to
// HX Edit's curve (screens/eq2.png: low cut 196 Hz, high cut 10.6 kHz with the three peaks): within ~1 dB from 100 Hz
// to 12 kHz; a Butterworth at the shown frequency was 2-4 dB off near both cuts.
constexpr double kLowCutShift = 1.3, kHighCutShift = 0.8;

Biquad pass(double shown, bool high) {
    const double f0 = shown * (high ? kLowCutShift : kHighCutShift);
    const double w = 2.0 * std::numbers::pi * std::min(f0, kFs * 0.45) / kFs;
    const double alpha = std::sin(w) / std::numbers::sqrt2, c = std::cos(w);  // Q = 1/sqrt(2)
    if (high) return {(1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha};
    return {(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha};
}

// Band columns and handles in HX Edit's order: low cut, three peaks, high cut.
struct BandDef {
    const char *name, *cls;
    int first, count;
};
constexpr BandDef kBands[] = {
    {"Low Cut", "lowcut", kLowCut, 1}, {"Low Peak", "low", 0, 3},          {"Mid Peak", "mid", 3, 3},
    {"High Peak", "high", 6, 3},       {"High Cut", "highcut", kHighCut, 1},
};

} // namespace

void GlobalEqView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Param>()) {
        s.RegisterMember("index", &Param::index);
        s.RegisterMember("label", &Param::label);
        s.RegisterMember("text", &Param::text);
        s.RegisterMember("pos", &Param::pos);
    }
    model.RegisterArray<std::vector<Param>>();
    if (auto s = model.RegisterStruct<Band>()) {
        s.RegisterMember("index", &Band::index);
        s.RegisterMember("name", &Band::name);
        s.RegisterMember("cls", &Band::cls);
        s.RegisterMember("selected", &Band::selected);
        s.RegisterMember("params", &Band::params);
    }
    model.RegisterArray<std::vector<Band>>();
    if (auto s = model.RegisterStruct<Handle>()) {
        s.RegisterMember("index", &Handle::index);
        s.RegisterMember("left", &Handle::left);
        s.RegisterMember("top", &Handle::top);
        s.RegisterMember("cls", &Handle::cls);
        s.RegisterMember("selected", &Handle::selected);
        s.RegisterMember("off", &Handle::off);
    }
    model.RegisterArray<std::vector<Handle>>();
    if (auto s = model.RegisterStruct<GridLine>()) {
        s.RegisterMember("pos", &GridLine::pos);
        s.RegisterMember("label", &GridLine::label);
        s.RegisterMember("major", &GridLine::major);
    }
    model.RegisterArray<std::vector<GridLine>>();

    model.Bind("geq_ready", &known_);
    model.Bind("geq_bypassed", &bypassed_);
    model.Bind("geq_apply", &applyText_);
    model.Bind("geq_bands", &bands_);
    model.Bind("geq_level", &level_);
    model.Bind("geq_curve", &curveSrc_);
    model.Bind("geq_handles", &handles_);
    model.Bind("geq_flines", &freqLines_);
    model.Bind("geq_dblines", &dbLines_);
    using Args = const Rml::VariantList &;
    auto intArg = [](Args a) { return a.empty() ? -1 : a[0].Get<int>(-1); };
    // Typed values apply on Enter or when the field loses focus.
    model.BindEventCallback("geq_text", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, Args a) {
        if (ev.GetType() == "change" && !ev.GetParameter<bool>("linebreak", false)) return;
        if (Rml::Element *el = ev.GetCurrentElement()) setText(intArg(a), el->GetAttribute<Rml::String>("value", ""));
    });
    model.BindEventCallback("geq_step", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (a.size() >= 2) step(a[0].Get<int>(-1), a[1].Get<int>(0));
    });
    model.BindEventCallback("geq_bar", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, Args a) {
        setFromBar(intArg(a), ev.GetParameter<float>("value", 0.f));
    });
    model.BindEventCallback("geq_apply_set", [this](Rml::DataModelHandle, Rml::Event &ev, Args) {
        setApply(std::atoi(ev.GetParameter<Rml::String>("value", "0").c_str()));
    });
    model.BindEventCallback("geq_power", [this](Rml::DataModelHandle, Rml::Event &, Args) { setBypass(!bypassed_); });
    model.BindEventCallback("geq_reset", [this](Rml::DataModelHandle, Rml::Event &, Args) { reset(); });
    model.BindEventCallback("geq_band_reset", [this, intArg](Rml::DataModelHandle, Rml::Event &, Args a) { resetBand(intArg(a)); });
    model.BindEventCallback("geq_pick", [this, intArg](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (const int b = intArg(a); b >= 0 && b < int(std::size(kBands)) && b != selected_) {
            selected_ = b;
            rebuild();
        }
    });
    model.BindEventCallback("geq_drag", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, Args a) { dragHandle(intArg(a), ev); });
    model.BindEventCallback("geq_wheel", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, Args a) {
        wheelHandle(intArg(a), ev.GetParameter<float>("wheel_delta_y", 0.f));
        ev.StopPropagation();  // the wheel sets Q; don't also scroll the page
    });
    handle_ = model.GetModelHandle();
    rebuild();
}

void GlobalEqView::setConnection(bool connected) {
    connected_ = connected;
    if (!connected) known_ = false;
    dirty();
}

void GlobalEqView::update(bool visible) {
    if (now() - lastFlush_ >= kFlushInterval) flush();
    if (!visible || !known_) return;
    if (Rml::Element *g = graphElement()) {
        const float w = g->GetClientWidth(), h = g->GetClientHeight();
        if (w > 0 && h > 0 && (std::abs(w - graphW_) > 0.5f || std::abs(h - graphH_) > 0.5f)) rebuild();
    }
}

void GlobalEqView::flush() {
    lastFlush_ = now();
    for (int i = 0; i < hx::editor::kGlobalEqCount; ++i) {
        if (!pending_[i]) continue;
        pending_[i] = false;
        awaiting_[i] = true;
        sentValue_[i] = values_[std::size_t(i)];
        sentAt_[i] = lastFlush_;
        if (!onSend) continue;
        const hx::mp::Value v = i == kApplyIndex ? hx::mp::integer(std::int64_t(values_[std::size_t(i)]))
                                                 : hx::mp::real(values_[std::size_t(i)]);
        onSend(hx::editor::kSetSetting, hx::editor::settingArgs(hx::editor::kGlobalEqFirst + i, v));
    }
}

void GlobalEqView::setValues(const std::vector<double> &values, bool bypassed) {
    if (values.size() < std::size_t(hx::editor::kGlobalEqCount)) return;
    values_ = values;
    bypassed_ = bypassed;
    apply_ = int(values_[kApplyIndex]);
    known_ = connected_;
    rebuild();
}

void GlobalEqView::applyNotice(const hx::EditorNotice &n) {
    if (n.event != hx::editor::kNoticeGlobal || !known_) return;
    const hx::mp::Value *id = n.payload.get(118), *v = n.payload.get(119);
    if (!id || !v) return;
    const int i = int(id->asInt()) - hx::editor::kGlobalEqFirst;
    if (i >= 0 && i < hx::editor::kGlobalEqCount) {
        if (pending_[i]) return;  // ours is newer and not even sent yet
        if (awaiting_[i]) {
            // The unit stores float32: the echo of our last value matches it to a few digits.
            const double sent = sentValue_[i], got = v->asNumber();
            if (std::abs(got - sent) <= 1e-4 * std::max(1.0, std::abs(sent))) {
                awaiting_[i] = false;
                return;
            }
            if (now() - sentAt_[i] < kEchoTimeout) return;  // an older step's echo
            awaiting_[i] = false;  // ours never came back: take what the unit says
        }
    }
    if (int(id->asInt()) == hx::editor::kGlobalEqBypass) {
        bypassed_ = v->asBool();
    } else if (i >= 0 && i < hx::editor::kGlobalEqCount) {
        values_[std::size_t(i)] = v->asNumber();
        if (i == kApplyIndex) apply_ = int(v->asInt());
    } else {
        return;
    }
    rebuild();
}

// Typed: a number, with "k" for kHz ("1.4k", "1.4 kHz"); "off" on a cut. Anything else puts the value back.
void GlobalEqView::setText(int index, const std::string &t) {
    if (!known_ || index < 0 || index >= int(std::size(kFields))) return;
    std::string low;
    for (char c : t) low += char(std::tolower(static_cast<unsigned char>(c)));
    if (low.find("off") != std::string::npos && (index == kLowCut || index == kHighCut)) {
        setNumber(index, index == kLowCut ? kLowCutOff : kHighCutOff);
    } else {
        char *end = nullptr;
        double v = std::strtod(low.c_str(), &end);
        if (end != low.c_str()) {
            if (low.find('k', std::size_t(end - low.c_str())) != std::string::npos) v *= 1000;
            setNumber(index, v);
        }
    }
    rebuild();
}

void GlobalEqView::setNumber(int index, double v) {
    if (!known_ || index < 0 || index >= int(std::size(kFields))) return;
    const double clamped = std::clamp(v, kFields[index].lo, kFields[index].hi);
    if (clamped == values_[std::size_t(index)]) return;
    values_[std::size_t(index)] = clamped;
    pending_[index] = true;  // goes out with the next flush (update)
}

// The up / down arrows: a semitone for frequencies, 0.1 for Q, 0.5 dB for gains.
void GlobalEqView::step(int index, int dir) {
    if (!known_ || index < 0 || index >= int(std::size(kFields)) || dir == 0) return;
    const double v = values_[std::size_t(index)];
    switch (kFields[index].kind) {
    case Kind::Freq:
    case Kind::LowCut:
    case Kind::HighCut: {
        const double from = isOff(index, v) ? (index == kLowCut ? 20.0 : 20000.0) : v;
        const double next = std::round(from * std::pow(2.0, dir / 12.0));
        setNumber(index, index == kLowCut && next < 20 ? kLowCutOff
                         : index == kHighCut && next > 20000 ? kHighCutOff
                         : next == std::round(from) ? from + dir : next);
        break;
    }
    case Kind::Q: setNumber(index, std::round((v + 0.1 * dir) * 100) / 100); break;
    case Kind::Gain:
    case Kind::Level: setNumber(index, std::round((v + 0.5 * dir) * 10) / 10); break;
    }
    rebuild();
}

// The bar under a value. Setting its position (a rebuild) fires "change" too: within half a step of where the value
// already sits, nothing moved.
void GlobalEqView::setFromBar(int index, float pos) {
    if (!known_ || index < 0 || index >= int(std::size(kFields))) return;
    if (std::abs(double(pos) - posOf(index, values_[std::size_t(index)])) < 0.0006) return;
    setNumber(index, valueAt(index, pos));
    rebuild();
}

// A handle follows the pointer over the graph: a peak's frequency (across) and gain (up / down), a cut's frequency
// only (dragged past the end of its range it turns off).
void GlobalEqView::dragHandle(int h, Rml::Event &ev) {
    if (!known_ || h < 0 || h >= int(std::size(kBands))) return;
    Rml::Element *graph = ev.GetTargetElement();
    while (graph && !graph->IsClassSet("eq-graph")) graph = graph->GetParentNode();
    if (!graph || graph->GetOffsetWidth() <= 0 || graph->GetOffsetHeight() <= 0) return;
    const Rml::Vector2f at = graph->GetAbsoluteOffset(Rml::BoxArea::Border);
    const double x = (ev.GetParameter<float>("mouse_x", 0.f) - at.x) / graph->GetOffsetWidth();
    const double y = (ev.GetParameter<float>("mouse_y", 0.f) - at.y) / graph->GetOffsetHeight();
    const double f = std::round(logVal(x, kFMin, kFMax));
    selected_ = h;
    if (h == 0) {
        setNumber(kLowCut, x <= 0.0 || f < 20 ? kLowCutOff : std::min(f, kFields[kLowCut].hi));
    } else if (h == 4) {
        setNumber(kHighCut, x >= 1.0 || f > 20000 ? kHighCutOff : std::max(f, kFields[kHighCut].lo));
    } else {
        const int base = kBands[h].first;
        setNumber(base, f);
        setNumber(base + 2, std::round((kDbTop - std::clamp(y, 0.0, 1.0) * (kDbTop - kDbBottom)) * 10.0) / 10.0);
    }
    rebuild();
}

// The wheel on a peak's handle: its Q, about 15 % per notch.
void GlobalEqView::wheelHandle(int h, float delta) {
    if (!known_ || h < 1 || h > 3 || delta == 0.f) return;
    selected_ = h;
    const int q = kBands[h].first + 1;
    setNumber(q, std::round(values_[std::size_t(q)] * std::pow(1.15, -double(delta)) * 100.0) / 100.0);
    rebuild();
}

void GlobalEqView::setApply(int apply) {
    if (!known_ || apply < 1 || apply > 3 || apply == apply_) return;
    apply_ = apply;
    values_[kApplyIndex] = apply;
    pending_[kApplyIndex] = true;
    rebuild();
}

void GlobalEqView::setBypass(bool bypassed) {
    if (!known_ || bypassed == bypassed_) return;
    bypassed_ = bypassed;
    if (onSend) onSend(hx::editor::kSetSetting, hx::editor::settingArgs(hx::editor::kGlobalEqBypass, hx::mp::boolean(bypassed)));
    rebuild();  // the curve's colour
}

// One column's button: a peak back to its defaults (frequency, Q, 0 dB), a cut off.
void GlobalEqView::resetBand(int band) {
    if (!known_ || band < 0 || band >= int(std::size(kBands))) return;
    selected_ = band;
    if (band == 0) setNumber(kLowCut, kLowCutOff);
    else if (band == 4) setNumber(kHighCut, kHighCutOff);
    else
        for (int i = kBands[band].first; i < kBands[band].first + kBands[band].count; ++i) setNumber(i, kDefaults[i]);
    rebuild();
}

// As HX Edit's Reset: every band, cut and the level back to the defaults (Apply EQ and bypass stay).
void GlobalEqView::reset() {
    if (!known_) return;
    for (int i = 0; i < int(std::size(kDefaults)); ++i) setNumber(i, kDefaults[i]);
    rebuild();
}

void GlobalEqView::rebuild() {
    applyText_ = std::to_string(apply_);
    bands_.clear();
    handles_.clear();
    if (freqLines_.empty()) {
        for (double f : {20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 200.0, 300.0, 400.0, 500.0, 600.0, 700.0,
                         800.0, 900.0, 1000.0, 2000.0, 3000.0, 4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9000.0, 10000.0,
                         20000.0}) {
            // Labelled as HX Edit does: 20 40 60 80 100 200 400 600 800 1k 2k 4k 6k 8k 10k 20k.
            const int lead = int(f / std::pow(10.0, std::floor(std::log10(f))));
            const bool label = lead == 1 || lead == 2 || lead == 4 || lead == 6 || lead == 8;
            std::string text;
            if (label) text = f >= 1000 ? std::to_string(int(f / 1000)) + "k" : std::to_string(int(f));
            freqLines_.push_back({pct(xOf(f)), text, label});
        }
        for (int db = 12; db >= -12; db -= 2)
            dbLines_.push_back({pct(yOf(db)), db == 0 ? "0 dB" : (db > 0 ? "+" : "") + std::to_string(db), db == 0});
    }
    if (values_.size() < std::size(kFields)) return dirty();
    auto v = [&](int i) { return values_[std::size_t(i)]; };

    for (int b = 0; b < int(std::size(kBands)); ++b) {
        Band band;
        band.index = b;
        band.name = kBands[b].name;
        band.cls = kBands[b].cls;
        band.selected = b == selected_;
        for (int i = kBands[b].first; i < kBands[b].first + kBands[b].count; ++i)
            band.params.push_back({i, kFields[i].label, text(i, v(i)), float(posOf(i, v(i)))});
        bands_.push_back(std::move(band));
    }
    level_ = {kLevel, kFields[kLevel].label, text(kLevel, v(kLevel)), float(posOf(kLevel, v(kLevel)))};

    std::vector<Biquad> filters;
    for (int b = 0; b < 3; ++b) filters.push_back(peaking(v(b * 3), v(b * 3 + 1), v(b * 3 + 2)));
    const bool lowCut = !isOff(kLowCut, v(kLowCut)), highCut = !isOff(kHighCut, v(kHighCut));
    if (lowCut) filters.push_back(pass(v(kLowCut), true));
    if (highCut) filters.push_back(pass(v(kHighCut), false));
    auto response = [&](double f) {
        double db = 0;
        for (const Biquad &q : filters) db += q.db(f);
        return db;
    };
    // The curve in pixels of the graph's current size (rotated pieces need real lengths and angles; update() redraws
    // on a resize). Before the graph has a size: a guess, corrected on the next frame.
    graphW_ = 900;
    graphH_ = 290;
    if (Rml::Element *g = graphElement(); g && g->GetClientWidth() > 0 && g->GetClientHeight() > 0) {
        graphW_ = g->GetClientWidth();
        graphH_ = g->GetClientHeight();
    }
    // A point every 2 px across, drawn into an image of the graph's exact size (shown 1:1, so it stays sharp).
    const int w = std::max(1, int(std::lround(graphW_))), h = std::max(1, int(std::lround(graphH_)));
    std::vector<std::pair<double, double>> pts;
    for (int x = 0; x <= w; x += 2) {
        const double t = double(x) / w;
        pts.emplace_back(x, yOf(response(logVal(t, kFMin, kFMax))) * h);
    }
    const std::uint8_t shade = bypassed_ ? 92 : 168;
    curveSrc_ = fwtex::putImage("eqcurve", w, h, drawLine(pts, w, h, 2.2, shade, shade, std::uint8_t(shade + 5)));
    // Peaks at their frequency and gain; cuts at their frequency on the 0 dB line (as HX Edit draws them), at the
    // graph's edge when off.
    const double y0 = yOf(0);
    handles_.push_back({0, pct(lowCut ? xOf(v(kLowCut)) : 0.0), pct(y0), "lowcut", selected_ == 0, !lowCut});
    for (int b = 1; b <= 3; ++b) {
        const int base = kBands[b].first;
        handles_.push_back({b, pct(xOf(v(base))), pct(yOf(v(base + 2))), kBands[b].cls, selected_ == b, false});
    }
    handles_.push_back({4, pct(highCut ? xOf(v(kHighCut)) : 1.0), pct(y0), "highcut", selected_ == 4, !highCut});
    dirty();
}

void GlobalEqView::dirty() {
    if (!handle_) return;
    for (const char *v : {"geq_ready", "geq_bypassed", "geq_apply", "geq_bands", "geq_level", "geq_curve", "geq_handles",
                          "geq_flines", "geq_dblines"})
        handle_.DirtyVariable(v);
}

} // namespace editor
