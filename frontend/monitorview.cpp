#include "frontend/monitorview.h"

#include "frontend/readonlytext.h"
#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <SDL3/SDL_clipboard.h>

#include <cstdio>

namespace editor {

namespace {

constexpr const char *kUsbBox = "mon-usb-text", *kUsbWrap = "mon-usb-wrap";

std::string stamp(double t) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.3f", t);
    return buf;
}

} // namespace

void MonitorView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<MidiRow>()) {
        s.RegisterMember("time", &MidiRow::time);
        s.RegisterMember("port", &MidiRow::port);
        s.RegisterMember("text", &MidiRow::text);
        s.RegisterMember("debug", &MidiRow::debug);
    }
    model.RegisterArray<std::vector<MidiRow>>();
    if (auto s = model.RegisterStruct<WordRow>()) {
        s.RegisterMember("name", &WordRow::name);
        s.RegisterMember("value", &WordRow::value);
        s.RegisterMember("hex", &WordRow::hex);
        s.RegisterMember("age", &WordRow::age);
        s.RegisterMember("seen", &WordRow::seen);
    }
    model.RegisterArray<std::vector<WordRow>>();

    model.Bind("mon_tab", &tab_);
    model.Bind("mon_usb_text", &usbText_);
    model.Bind("mon_usb_count", &usbCount_);
    model.Bind("mon_usb_paused", &usbPaused_);
    model.BindFunc("mon_usb_frozen", [this](Rml::Variant &v) { v = usbFocused_ || usbPaused_; });
    model.Bind("mon_midi", &midiShown_);
    model.Bind("mon_midi_state", &midiState_);
    model.BindFunc("mon_listening", [this](Rml::Variant &v) { v = midi_.listening(); });
    model.Bind("mon_ports", &ports_);
    model.Bind("mon_port_names", &portNames_);
    model.Bind("mon_port", &portChoice_);
    model.Bind("mon_words", &wordRows_);
    model.Bind("mon_hide_rt", &hideRealtime_);
    model.Bind("mon_debug_only", &debugOnly_);

    using Args = const Rml::VariantList &;
    model.BindEventCallback("mon_pick", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (!a.empty()) setTab(a[0].Get<Rml::String>());
    });
    model.BindEventCallback("mon_copy", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        std::string all;
        if (tab_ == "usb") {
            for (const std::string &l : usb_) all += l + "\n";
        } else {
            for (const MidiRow &r : midiAll_) all += r.time + "  " + r.port + "  " + r.text + "\n";
        }
        SDL_SetClipboardText(all.c_str());
    });
    model.BindEventCallback("mon_clear", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (tab_ == "usb") {
            usb_.clear();
            usbDirty_ = true;
        } else {
            midiAll_.clear();
            words_.clear();
            midiDirty_ = true;
            rebuildWords();
        }
    });
    // The text box's focus freezes it, so a selection isn't replaced by new lines.
    model.BindEventCallback("mon_usb_focus", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        usbFocused_ = !a.empty() && a[0].Get<bool>();
        if (!usbFocused_) usbDirty_ = true;  // catch up (and undo any typing in the box)
        dirty("mon_usb_frozen");
    });
    model.BindEventCallback("mon_usb_pause", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        usbPaused_ = !usbPaused_;
        if (!usbPaused_) usbDirty_ = true;
        dirty("mon_usb_paused");
        dirty("mon_usb_frozen");
    });
    model.BindEventCallback("mon_listen", [this](Rml::DataModelHandle, Rml::Event &, Args) { listen(!midi_.listening()); });
    // The dropdown: listening again on the new choice right away.
    model.BindEventCallback("mon_port_changed", [this](Rml::DataModelHandle, Rml::Event &ev, Args) {
        const std::string choice = ev.GetParameter<Rml::String>("value", portChoice_);
        if (choice == portChoice_ && !midi_.listening()) return;
        portChoice_ = choice;
        if (midi_.listening()) {
            listen(false);
            listen(true);
        }
    });
    model.BindEventCallback("mon_toggle", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (a.empty()) return;
        const std::string what = a[0].Get<Rml::String>();
        if (what == "rt") hideRealtime_ = !hideRealtime_;
        if (what == "debug") debugOnly_ = !debugOnly_;
        midiDirty_ = true;
        dirty("mon_hide_rt");
        dirty("mon_debug_only");
    });
    handle_ = model.GetModelHandle();
    rebuildWords();
}

void MonitorView::dirty(const char *name) {
    if (handle_) handle_.DirtyVariable(name);
}

void MonitorView::setTab(const std::string &tab) {
    if ((tab != "usb" && tab != "midi") || tab == tab_) return;
    tab_ = tab;
    if (tab_ == "usb") usbDirty_ = true;
    dirty("mon_tab");
}

void MonitorView::scanPorts() {
    std::vector<std::string> names = hx::midi::Input::portNames();
    if (names == portNames_) return;
    portNames_ = std::move(names);
    // A chosen port that went away: back to all.
    if (portChoice_ != "all" && std::stoul(portChoice_) >= portNames_.size()) portChoice_ = "all";
    dirty("mon_port_names");
    dirty("mon_port");
}

void MonitorView::listen(bool on) {
    if (on) {
        scanPorts();
        midi_.start(portChoice_ == "all" ? -1 : int(std::stoul(portChoice_)));
        if (midi_.ports().empty())
            midiAll_.push_back({stamp(hx::midi::seconds()), "", "No MIDI input ports found.", false, false});
    } else {
        midi_.stop();
    }
    ports_.clear();
    int open = 0;
    for (const hx::midi::Port &p : midi_.ports()) {
        if (p.error == "not selected") continue;
        ports_.push_back(p.name + (p.open ? "" : "  (can't open: " + p.error + ")"));
        open += p.open;
    }
    midiState_ = midi_.listening() ? "Listening, " + std::to_string(open) + " port" + (open == 1 ? "" : "s") : "Off";
    midiDirty_ = true;
    dirty("mon_listening");
    dirty("mon_ports");
    dirty("mon_midi_state");
}

void MonitorView::addLog(std::string line) {
    usb_.push_back(stamp(hx::midi::seconds()) + "  " + std::move(line));
    while (usb_.size() > kUsbLines) usb_.pop_front();
    usbDirty_ = true;
}

void MonitorView::update(bool visible) {
    for (const hx::midi::Message &m : midi_.take()) {
        const bool debug = words_.feed(m);
        MidiRow r{stamp(m.time), "", hx::midi::describe(m), debug, hx::midi::isRealtime(m)};
        const auto &ports = midi_.ports();
        if (m.port >= 0 && std::size_t(m.port) < ports.size()) r.port = ports[std::size_t(m.port)].name;
        midiAll_.push_back(std::move(r));
        if (midiAll_.size() > kMidiLines) midiAll_.pop_front();
        midiDirty_ = true;
    }
    if (!visible) return;
    // The document is loaded after bind(): the guard goes on once the box exists.
    if (!guarded_) guarded_ = guardReadOnly(kUsbWrap);
    const double now = hx::midi::seconds();
    if (tab_ == "midi" && !midi_.listening() && now - lastPortScan_ >= 2.0) {  // ports plugged in or out
        scanPorts();
        lastPortScan_ = now;
    }
    // RmlUi lays out the whole list on each change: a few rebuilds a second at most.
    if (now - lastRebuild_ >= 0.25) {
        if (usbDirty_ && tab_ == "usb" && !usbFocused_ && !usbPaused_) rebuildUsb();
        if (midiDirty_) {
            rebuildMidi();
            rebuildWords();
        }
        lastRebuild_ = now;
    }
    if (now - lastWords_ >= 1.0) {  // the words' ages
        rebuildWords();
        lastWords_ = now;
    }
    if (scrollPending_ > 0) {
        --scrollPending_;
        if (Rml::Element *box = findElementById(kUsbBox)) box->SetScrollTop(box->GetScrollHeight());
    }
}

void MonitorView::rebuildUsb() {
    usbText_.clear();
    const std::size_t first = usb_.size() > kUsbShown ? usb_.size() - kUsbShown : 0;
    for (std::size_t i = first; i < usb_.size(); ++i) {
        usbText_ += usb_[i];
        if (i + 1 < usb_.size()) usbText_ += '\n';
    }
    usbCount_ = usb_.empty() ? "empty" : std::to_string(usb_.size()) + " lines";
    usbDirty_ = false;
    scrollPending_ = 3;
    dirty("mon_usb_text");
    dirty("mon_usb_count");
}

void MonitorView::rebuildMidi() {
    midiShown_.clear();
    for (auto it = midiAll_.rbegin(); it != midiAll_.rend() && midiShown_.size() < kShown; ++it) {
        if (debugOnly_ && !it->debug) continue;
        if (hideRealtime_ && it->realtime) continue;
        midiShown_.push_back(*it);
    }
    midiDirty_ = false;
    dirty("mon_midi");
}

void MonitorView::rebuildWords() {
    wordRows_.clear();
    const double now = hx::midi::seconds();
    for (int w = 0; w < hx::midi::DebugWords::kWords; ++w) {
        WordRow r;
        r.name = "W" + std::to_string(w + 1);
        r.seen = words_.seen(w);
        if (r.seen) {
            const std::uint32_t v = words_.value(w);
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%05x", unsigned(v));
            r.value = std::to_string(v);
            r.hex = hex;
            const int age = int(now - words_.updated(w));
            r.age = age < 2 ? "just now" : std::to_string(age) + " s ago";
        } else {
            r.value = "-";
            r.age = "not seen";
        }
        wordRows_.push_back(std::move(r));
    }
    dirty("mon_words");
}

} // namespace editor
