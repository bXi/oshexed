#pragma once

#include "core/device/editorclient.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
class Event;
}

namespace editor {

// The Helix's global EQ (Helix tab > Global EQ), laid out like HX Edit's Global EQ window: Reset, Apply EQ, bypass;
// the response graph with a handle per band (drag: frequency and gain; wheel: Q) and per cut (drag sideways); under
// it a column per band (low cut, low / mid / high peak, high cut) with each value, up / down steps and a bar to drag,
// and the output level. Read when the editor connects (cmd 76 + setting 203), changed live with cmd 25; the unit's
// own changes come back as global-setting notices. The curve is our own model (peaks as 1 + (G-1)·bandpass, which
// matches HX Edit's drawing; 12 dB / octave cuts at 48 kHz; without the level): not a measurement of the unit.
class GlobalEqView {
public:
    void bind(Rml::DataModelConstructor &model);
    void setConnection(bool connected);
    // Every frame: the curve is laid out in pixels, so a new graph size (window resize, first show) redraws it.
    void update(bool visible);
    void setValues(const std::vector<double> &values, bool bypassed);  // the 13 values of cmd 76
    void applyNotice(const hx::EditorNotice &notice);                   // a global setting changed on the unit

    std::function<void(int command, hx::mp::Value args)> onSend;  // a live edit (DeviceLink::sendEdit)

private:
    // One value in a band column: label, text, its bar position (0..1).
    struct Param {
        int index = 0;  // 0..11: setting 190 + index
        std::string label, text;
        float pos = 0;
    };
    struct Band {
        int index = 0;      // 0 low cut, 1..3 low / mid / high peak, 4 high cut (handle and selection order)
        std::string name, cls;
        bool selected = false;
        std::vector<Param> params;
    };
    struct Handle {
        int index = 0;  // as Band::index
        std::string left, top, cls;
        bool selected = false, off = false;
    };
    struct GridLine {
        std::string pos, label;
        bool major = false;
    };

    void setText(int index, const std::string &text);
    void setNumber(int index, double v);  // clamped to the value's range, sent when it changed
    void step(int index, int dir);
    void setFromBar(int index, float pos);
    void setApply(int apply);
    void setBypass(bool bypassed);
    void reset();
    void resetBand(int band);  // Band::index: a peak to its defaults, a cut off
    void dragHandle(int handle, Rml::Event &ev);
    void wheelHandle(int handle, float delta);
    void rebuild();
    void dirty();

    Rml::DataModelHandle handle_;
    bool connected_ = false, known_ = false;
    std::vector<double> values_;  // 13
    bool bypassed_ = true;
    int apply_ = 3;
    std::string applyText_ = "3";  // the Apply EQ dropdown's value
    std::vector<Band> bands_;
    Param level_;
    std::string curveSrc_;  // the curve, drawn anti-aliased into an image the graph's size (fwtex::putImage)
    std::vector<Handle> handles_;
    std::vector<GridLine> freqLines_, dbLines_;
    int selected_ = 2;  // Band::index; mid peak at first
    float graphW_ = 0, graphH_ = 0;  // the graph's size in px when the curve was drawn

    // Sending: a changed value waits (pending_) and goes out with the next flush, at most every kFlushInterval, so a
    // drag sends its newest position instead of queueing every step. Until the unit's notice brings back the value
    // last sent (awaiting_), its notices for that setting are older steps and are ignored: they would pull the handle
    // back, seconds later when the link is busy.
    void flush();
    bool pending_[13] = {}, awaiting_[13] = {};
    double sentValue_[13] = {}, sentAt_[13] = {};
    double lastFlush_ = 0;
};

} // namespace editor
