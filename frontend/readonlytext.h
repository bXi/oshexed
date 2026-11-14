#pragma once

// Read-only text boxes (the USB log, the build report): RmlUi's textarea has no readonly, so a listener on the box's
// wrapper, in the capture phase, runs before the text widget's own listeners on the box. Typing and edit keys stop
// there; moving the cursor, selecting and Ctrl+A / Ctrl+C get through (the copy itself is App's, from the SDL key
// event).

#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>

namespace editor {

class ReadOnlyGuard : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event &e) override {
        if (e.GetId() == Rml::EventId::Textinput) return e.StopPropagation();
        if (e.GetId() != Rml::EventId::Keydown) return;
        using K = Rml::Input::KeyIdentifier;
        const auto key = K(e.GetParameter<int>("key_identifier", 0));
        const bool ctrl = e.GetParameter<int>("ctrl_key", 0) != 0;
        switch (key) {
        case K::KI_LEFT: case K::KI_RIGHT: case K::KI_UP: case K::KI_DOWN:
        case K::KI_HOME: case K::KI_END: case K::KI_PRIOR: case K::KI_NEXT:
        case K::KI_LSHIFT: case K::KI_RSHIFT: case K::KI_LCONTROL: case K::KI_RCONTROL:
            return;
        case K::KI_A: case K::KI_C:
            if (ctrl) return;
            break;
        default: break;
        }
        e.StopPropagation();
    }
};

inline ReadOnlyGuard g_readOnlyGuard;

// The element with that id in any loaded document; nullptr when none (yet).
inline Rml::Element *findElementById(const char *id) {
    Rml::Context *ctx = RmlUI::GetContext();
    if (!ctx) return nullptr;
    for (int i = 0; i < ctx->GetNumDocuments(); ++i)
        if (Rml::Element *e = ctx->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

// Puts the guard on the wrapper with that id once the document has it. Call every frame until it returns true.
inline bool guardReadOnly(const char *wrapId) {
    Rml::Element *wrap = findElementById(wrapId);
    if (!wrap) return false;
    wrap->AddEventListener(Rml::EventId::Keydown, &g_readOnlyGuard, true);
    wrap->AddEventListener(Rml::EventId::Textinput, &g_readOnlyGuard, true);
    return true;
}

} // namespace editor
