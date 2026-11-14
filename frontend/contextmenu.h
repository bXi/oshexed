#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
class Event;
}

namespace editor {

// The right-click menu, one for the whole window (markup at the end of main.rml). A view opens it from a
// "mousedown" handler with the actions for what was clicked; clicking an item runs it, clicking anywhere else or
// Esc closes it.
class ContextMenu {
public:
    // A colour dot in an item's row of swatches (e.g. a snapshot's colour): its CSS class, what picking it does.
    struct Swatch {
        std::string cls;
        std::function<void()> action;
        bool selected = false;
    };
    struct Item {
        std::string label;              // "" = a separator line (unless it has swatches)
        std::function<void()> action;
        bool enabled = true;
        std::vector<Swatch> swatches;   // non-empty: the row shows the label and these dots, each its own action
        bool checked = false;           // the current choice (when the menu is a list of choices): highlighted
    };

    void bind(Rml::DataModelConstructor &model);
    // At the mouse position of a mousedown event, if it was the right button; false (and nothing) otherwise.
    static bool isRightClick(Rml::Event &ev);
    void openAt(Rml::Event &ev, std::vector<Item> items, std::string title = "");
    // Where the last menu was: a follow-up choice (an item's action opens the next step, e.g. block, then knob).
    void openAgain(std::vector<Item> items, std::string title = "");
    void close();
    bool isOpen() const { return open_; }

private:
    struct SwatchRow {
        int index = 0;
        std::string cls;
        bool selected = false;
    };
    struct Row {
        int index = 0;
        std::string label;
        bool enabled = true, separator = false, palette = false, checked = false;
        std::vector<SwatchRow> swatches;
    };
    void pick(int index);
    void fill(std::vector<Item> items, std::string title);
    void pickSwatch(int item, int swatch);
    void dirty();

    Rml::DataModelHandle handle_;
    std::vector<Item> items_;
    std::vector<Row> rows_;
    bool open_ = false;
    std::string left_ = "0px", top_ = "0px", title_;
};

} // namespace editor
