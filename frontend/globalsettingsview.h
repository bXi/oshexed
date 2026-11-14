#pragma once

#include "core/device/editorclient.h"
#include "core/globalsettings.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
class Event;
}

namespace editor {

class ContextMenu;

// Helix tab > Global Settings: the unit's Global Settings menu pages (core/globalsettings.h), read when the
// editor connects (cmd 24 per setting) and changed live (cmd 25), like on the unit; its own changes come back as
// notices. Laid out like the unit's menu: the pages in a column, one page at a time, a compact row per setting
// (up to 4 choices as buttons, longer lists as a dropdown, numbers as a field).
class GlobalSettingsView {
public:
    void bind(Rml::DataModelConstructor &model);
    void setMenu(ContextMenu *menu) { menu_ = menu; }  // the window's menu, used for the dropdowns
    void setDefinitions(std::vector<hx::GlobalSetting> defs);  // from the firmware (labels, options)
    std::vector<int> ids() const;
    void setConnection(bool connected);
    void setValues(const std::vector<double> &idValuePairs);
    void applyNotice(const hx::EditorNotice &notice);

    std::function<void(int command, hx::mp::Value args)> onSend;

private:
    struct Option {
        int index = 0;
        std::string label;
    };
    struct Row {
        int id = 0;
        std::string label, kind;  // kind: "segs" (<= 4 choices), "list" (longer lists: a dropdown), "number"
        std::vector<Option> options;
        int value = 0;             // the choice (an index into options; -1 = a value with no option)
        std::string current;       // its name (list) or the number's text
        bool known = false;
        std::vector<int> stored;   // the value each option stands for (empty: its index); not bound
    };
    struct Group {
        std::string name;
        std::vector<Row> rows;
    };
    Row *row(int id);
    static int choiceOf(const Row &r, int stored);
    static int storedOf(const Row &r, int index);
    void set(int id, double value);
    void pick(int id, int index);
    void openList(Rml::Event &ev, int id);
    void number(int id, const std::string &text);
    void show(Row &r);
    void dirty();

    Rml::DataModelHandle handle_;
    ContextMenu *menu_ = nullptr;
    std::vector<hx::GlobalSetting> defs_;
    std::vector<Group> groups_;
    std::map<int, double> values_;
    bool connected_ = false, ready_ = false;
    // the page shown: its index, the page list (index = position), and a copy of its rows for the data binding
    int page_ = 0;
    std::vector<Option> pages_;
    std::vector<Row> shown_;
    std::string pageName_;
};

} // namespace editor
