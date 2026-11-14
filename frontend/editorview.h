#pragma once

#include "core/device/editorclient.h"
#include "core/firmware.h"
#include "core/globalsettings.h"
#include "core/unitprofile.h"
#include "frontend/contextmenu.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace editor {

// Rows bound to the "editor" RmlUi data model (ui/main.rml).
struct UiItem {
    int index = 0;
    std::string label;
    std::string extra;
    std::string cls;
    bool enabled = true;
    std::string icon;       // firmware image source (fwtex), "" = none
};
struct UiCell {
    int id = -1;            // dsp * 100 + lane * 10 + slot; slot 8 = input node, 9 = output node
    std::string kind;       // "io", "block", "empty"
    std::string label;
    std::string cls;        // category colour class
    bool selected = false;
    bool bypassed = false;
    bool hidden = false;    // lane B input/output node when B has none of its own
    bool dragIo = false;    // lane B's own input/output: drag it onto lane A to make a split / merge
    std::string icon;       // block icon from the firmware (fwtex source), "" = draw the colour box
    bool tallIcon = false;  // an I/O image taller than the round ones (Path 2A / 2B outputs: circle + arrow)
};
// One signal-flow row. Positions are percentages of the row width (10 equal columns:
// input node, 8 slots, output node).
//
// Lane B is its own row of 8 positions; the split only says where lane A's signal is tapped.
// So lane B's line always spans its whole row, and each connector "snakes": from the split
// point on lane A down to between the rows, sideways to lane B's start, then down (the merge
// mirrors this at lane B's end), as Helix Native draws it.
struct UiLane {
    std::string label;
    std::vector<UiCell> cells;
    bool line = true;          // lane B: false while the path is serial
    bool ghost = false;        // lane B of a serial path while dragging: the routing a drop would make, greyed
    std::string lineLeft = "5%", lineRight = "5%";  // CSS left/right of the line
    bool split = false, join = false;               // lane B: connectors up to lane A
    std::string splitX, joinX;                      // split / merge point on lane A
    std::string splitBX, joinBX;                    // where the connector meets lane B
    std::string splitHL, splitHW, joinHL, joinHW;   // sideways segment: left and width
    bool splitRight = false, joinRight = false;     // the lane A end lies right of the lane B end
    std::string splitHR, joinHR;                    // sideways run: CSS right (100% - its right end)
    bool splitStraight = false, joinStraight = false;  // both ends at one x: a plain vertical line
    int splitId = -1, joinId = -1;                  // pick_cell ids of the split / merge
    bool splitSel = false, joinSel = false;
};
// Bypass / Controller Assign as HX Edit has it: one row per assignment in the preset (a block's bypass on a switch, a
// knob on a pedal / switch / MIDI CC / snapshots).
struct UiAssignRow {
    int index = 0;  // into EditorView::caRows_
    std::string block, param, source, cls;
    bool selected = false;
};
// A field of the selected Command Center command.
struct UiField {
    int index = 0;
    std::string label, value;
    std::string text;   // the value as HX Edit shows it ("Base", "C3", "Play"), "" = the number itself
    bool list = false;  // a choice of names: a dropdown instead of a number
};
// A choice setting shown in the edit area (the input block's Guitar In-Z / Guitar Pad, stored in the preset).
struct UiChoice {
    int id = 0;               // global setting id
    std::string label, kind;  // kind: "segs" (<= 4 options) or "step"
    std::vector<UiItem> options;
    int value = 0;
    std::string current;      // the value's name
};

struct UiParam {
    int index = 0;          // into the model's rec 4 knob list
    std::string name;
    std::string value;
    bool slider = true;
    float pos = 0;          // 0..1 along the slider
    bool centered = false;  // pan / balance style: the fill grows from the middle
    std::string fillLeft = "0%", fillWidth = "0%";  // the coloured part of the bar (CSS left / width)
    bool irSlot = false;    // picks a user IR slot (IR Select, NAM Model): a dropdown of the unit's IR names
    bool open = false;      // its dropdown is open
};

// The HX-style main screen: preset sidebar, signal flow, and the edit area (category, model,
// params). Works on a copy of the selected factory preset; nothing goes to the device.
class EditorView {
public:
    // Before any document that uses the model is loaded.
    void bind(Rml::DataModelConstructor &model);
    // Shows `fw` (may be null), opening the given setlist and preset when they exist.
    void setFirmware(const hx::Firmware *fw, int setlist = 0, int preset = 0);

    // Called whenever the user (or setFirmware) opens a preset, so the app can remember it.
    std::function<void(int setlist, int preset)> onPresetOpened;
    void setStatus(std::string status);
    // The unit's user IR slot names (128, "" = empty), for IR Select / NAM Model dropdowns; empty vector = unknown.
    // kinds (per slot, same order): 0 empty / not read, 1 IR, 2 NAM capture (read from the slots' data at connect).
    void setIrNames(std::vector<std::string> names, std::vector<int> kinds = {});
    // Edits of the open preset (Ctrl+Z / Ctrl+Y). Opening another preset starts a new history.
    void undo();
    // Save to the Helix (Ctrl+S): asks first, naming the slot and what's stored there; only "Save" sends cmd 71.
    void askSave();
    void cancelPicking();  // Esc: leaves Save as's slot picking, closes the save question
    void setMenu(ContextMenu *menu) { menu_ = menu; }  // the window's right-click menu
    std::function<void()> onExport;                     // "Export .hlx" (the app's save dialog)
    // Stored presets on the unit (library channel; DeviceLink::sendLibraryWrite / sendLibraryRead).
    std::function<void(int command, hx::mp::Value args)> onLibraryWrite;
    std::function<void(int command, hx::mp::Value args, std::string tag)> onLibraryRead;
    // Backups: the app asks for the file / folder, reads the presets and writes them (setlist = the unit's).
    std::function<void(int setlist, int preset, std::string name)> onBackupPreset;
    std::function<void(int setlist)> onBackupSetlist;
    std::function<void(int setlist, int preset)> onRestorePreset;
    void applyStoredCopy(const std::vector<std::uint8_t> &blob);  // a stored preset read for Copy
    // A backup file into a slot, named `name` (the bytes hold no name).
    void restoreStored(int setlist, int index, const std::vector<std::uint8_t> &blob, const std::string &name);
    std::string storedName(int setlist, int preset) const;  // the unit's name for a slot ("New Preset" if empty)
    const std::vector<UiItem> &setlistItems() const { return setlists_; }
    // Setting "Ask before saving to the Helix" (off by default: Save writes at once, as in HX Edit).
    void setConfirmSave(bool on);
    std::function<void(bool on)> onConfirmSaveChanged;
    void selectSnapshot(int index);  // 0-based; recalls it, and on the unit when connected (Ctrl+1..8)
    void copyBlock();   // the selected block (Ctrl+C)
    void pasteBlock();  // onto the selected slot, replacing what is there (Ctrl+V)
    void setClipBlob(std::vector<std::uint8_t> blob);  // the copied preset as the unit sent it
    void redo();
    // Shows a preset that isn't one of the firmware's (an imported .hlx). Needs a firmware.
    void openPreset(hx::Preset preset, std::string title);
    bool hasFirmware() const { return fw_ != nullptr; }
    // ---- the unit (App owns the DeviceLink and forwards its session events)
    // Edits for the unit's current preset; set by the App. Called only while connected.
    std::function<void(int command, hx::mp::Value args)> onLiveEdit;
    std::function<void(const std::string &status)> onStatus;  // every status line (for the device log)
    std::function<void(const std::string &line)> onLog;       // details for the device log only (Monitor > USB)
    // The Online / Offline switch and the unit picker (offline).
    std::function<void(bool online)> onOnlineChanged;
    std::function<void(const std::string &unit)> onUnitChanged;
    void setOnline(bool online, const std::string &unit);
    // The model the editor works for (App::updateUnitModel): its hardware filters the I/O port lists.
    void setUnitModel(hx::UnitModel model);
    void setConnection(bool connected, const std::string &text);
    void applyDeviceLibrary(const std::vector<std::string> &setlists, const std::vector<std::pair<int, std::string>> &names);
    void applyDevicePreset(const hx::editor::CurrentInfo &info, const std::vector<std::uint8_t> &blob);
    void applyDeviceNotice(const hx::EditorNotice &notice);
    void applyDeviceInfo(const hx::editor::CurrentInfo &info);  // the unit's modified flag for the open preset
    // Rebuilds once for all notices applied this frame (a snapshot change is one event per block).
    void flushDeviceNotices();
    // Every frame: gives a just-opened rename field the keyboard focus.
    void update();

    // The open preset as an HX Edit file (.hlx text); "" without a firmware.
    std::string exportHlx(std::vector<std::string> *warnings) const;
    std::string presetName() const { return preset_.name; }
    // Reads an HX Edit preset file (.hlx text) against the open firmware; see hx::importHlx.
    std::optional<hx::Preset> importHlx(std::string_view text, std::string *error,
                                        std::vector<std::string> *warnings) const;

private:
    // A block slot, a path's split or merge, or a lane's input / output node (the "special" items; Input / Output use
    // Selection::lane).
    enum class Special { None, Split, Join, Input, Output };
    struct Selection {
        int dsp = -1, lane = 0, slot = 0;
        Special special = Special::None;
        bool valid() const { return dsp >= 0; }
    };
    // What the params pane edits: a block's or a split/merge's knob values.
    struct EditTarget {
        const hx::MetaEntry *meta = nullptr;
        std::vector<hx::PresetValue> *values = nullptr;
        bool *enabled = nullptr;
        hx::PresetBlock *block = nullptr;  // when the target is a block
        hx::PresetTarget target;           // for controller / footswitch assignments; none for inputs / outputs
        std::string title;
        int category = -1;
        std::size_t knobCount = SIZE_MAX;  // only the first N knobs are shown (inputs / outputs: rec 4 displayParamCount)
    };
    bool isIo() const { return sel_.special == Special::Input || sel_.special == Special::Output; }
    // The input / output node a cell shows: label and firmware icon (Sel variant when selected).
    void ioCell(int dsp, int lane, bool input, UiCell &cell) const;
    EditTarget editTarget();
    void showSpecialInBrowser();
    UiParam *findParam(int knobIndex);

    // Snapshots: recall, rename (the bar above the signal flow).
    void pickSnapshot(int index);
    void renameSnapshot(const std::string &name);
    void setSnapshotColour(int colour);  // the snapshot in use
    void setSnapshotColourAt(int snapshot, int colour);
    void setSnapshotTempo(const std::string &text);
    void copySnapshot();   // remembers the selected snapshot
    void pasteSnapshot();  // onto the selected one
    void rebuildSnapshots();
    // Edit area tabs: "edit", "assign" (bypass / controller assign), "commands" (Command Center).
    void pickEditTab(const std::string &tab);
    // Controller Assign: the knob's controller (hx::controller::*, 0 = none); a footswitch for the block's bypass.
    void setController(int knobIndex, int source);
    // A pedal / footswitch assignment's value at the controller's minimum (`max` false) or maximum; pos 0..1.
    void setControllerRange(int knobIndex, bool max, float pos);
    void setControllerCc(int knobIndex, const std::string &text);  // MIDI CC source: its CC number (0..127)
    void toggleBypassSwitch(int fsIndex);
    // The switch fsSel_ as a whole (the unit keeps label, LED and latching per switch).
    void setSwitchLabel(const std::string &label);
    void setSwitchColour(int colour);  // LED colour index (0 = Auto Color)
    void setSwitchMomentary(bool momentary);
    void setCommandType(int slot, int type);
    void setCommandField(int slot, int field, const std::string &text);
    // A new block for the working preset: a fresh uid, so no old assignment points at it.
    hx::PresetBlock newBlock(const hx::ModelRef &m);
    // The cell id of a block, -1 if it isn't in the preset.
    int cellOf(int uid) const;
    // The label a footswitch shows for a block (its model name).
    std::string blockLabel(const hx::PresetBlock &b) const;

    void pickSetlist(int index, int preset = 0);
    void pickPreset(int index);
    // The library slot of a preset in the current setlist.
    std::size_t slotOf(int index) const { return std::size_t(setlist_ * hx::kPresetsPerSetlist + index); }
    // Puts the working copy back into the library (before another preset is opened).
    void storeWorking();
    void renamePreset(const std::string &name);
    // Moves preset `from` to `to` in the setlist; the ones between shift by one, as in HX Edit.
    void movePreset(int from, int to);
    void copyPreset();
    void pastePreset();  // over the selected one
    void markEdited();   // the working preset changed
    // Live editing: true when connected (the edit is sent). `what` names an edit the unit has no known command for:
    // it stays in the editor and the status says so.
    bool live() const { return connected_ && showingDevice_ && onLiveEdit; }
    bool deviceLists() const { return connected_ && onLiveEdit; }  // the lists are the unit's; picking loads there
    void sendLive(int command, hx::mp::Value args);
    void notSent(const std::string &what);
    bool sendSelectedModel();
    bool sendModelAt(int slot, const hx::PresetBlock &b);
    bool sendBlockAt(int slot, const hx::PresetBlock &b);  // the whole block (model, knobs, on / off) at a slot
    bool undoIfDspFull(int dsp);
    std::uint32_t entryCost(const hx::BrowserEntry &e) const;
    std::uint32_t dspRoom();        // DSP units left for the selected slot (its block counted as gone)
    void updateFits();              // greys out models / categories that don't fit there  // takes back the last edit if it put the path over the unit's DSP budget  // live: the selected block's model (into an empty slot = adding it)
    int selectedSlotNumber() const;  // the selection's global slot (path * 20 + slot list index), -1 if none
    int blockTypeOf(const hx::PresetBlock &b) const;  // preset key 9 for the block (model + linked cab)
    int factoryBlockType(std::uint16_t model) const;   // key 9 as the firmware's factory presets have it; -1
    void rebuildUnitChoices();
    // Call before changing preset_: saves the state for undo. Repeated edits with the same non-empty key in quick
    // succession (a slider drag, typing) are one step. `before` = the state to save when preset_ already changed.
    void beginEdit(const std::string &key = "", const hx::Preset *before = nullptr);
    void clearHistory();
    void reselect();     // after undo / redo: the same selection on the restored preset
    void showPreset();  // after preset_ changed: select the first block, rebuild everything
    void pickCell(int id);
    void pickCategory(int category);
    void pickSub(int index);
    void pickModel(int index);
    void toggleBypass();
    void paramChanged(int index, float pos);
    static void setFill(UiParam &p);
    void paramWheel(int index, float delta, bool fine);

    // Drag and drop: a block from the signal flow, or a model from the browser list, onto a cell.
    void dragCell(int id);
    void dragModel(int index);
    void dropOnCell(int id);
    std::map<int, int> blockSlots(int dsp) const;
    static std::optional<std::vector<std::pair<int, int>>> planMoves(std::map<int, int> before,
                                                                     const std::map<int, int> &after, int dsp);
    void setDropTarget(int id);
    // Dragging a split / merge (its dot on lane A, or lane B's own input / output node). The
    // mouse position and lane B's track box are in context pixels. `id` is the dragged item's
    // cell id; junctionOf() maps it to the split / merge id, or -1 for anything else.
    void moveJunction(int id, float mouseX, float mouseY, float trackLeft, float trackTop, float trackWidth,
                      float trackHeight);
    static int junctionOf(int id);

    std::optional<hx::PresetBlock> *selectedSlot();
    hx::PresetBlock *selectedBlock();
    void showModelInBrowser(const hx::ModelRef *m);
    void setBrowserList(int list);

    void rebuildPresetList();
    void rebuildLanes();
    void rebuildCategories();
    void rebuildModels();
    void rebuildParams();
    void rebuildDsp();
    void refresh();        // everything (preset, setlist or firmware changed)
    void refreshEditor();  // flow + edit area only (clicks, drops, bypass, junction moves)

    const hx::Firmware *fw_ = nullptr;
    hx::PresetLibrary library_;   // the firmware's presets with this session's edits
    std::vector<bool> edited_;    // per library slot
    std::optional<hx::Preset> presetClip_;
    std::vector<std::uint8_t> clipBlob_;  // presetClip_ as the unit's bytes (copied while connected), else empty
    int dragPreset_ = -1;
    hx::Preset preset_;  // working copy
    // The unit.
    bool online_ = true, connected_ = false;
    bool showingDevice_ = false;  // the open preset is the unit's current one (not an imported file)
    std::string unit_ = "helix-lt", deviceText_ = "Offline";
    hx::UnitModel unitModel_ = hx::UnitModel::Unknown;
    std::vector<UiItem> units_;              // offline unit picker: label, enabled = supported
    std::vector<std::string> deviceSetlists_;
    std::vector<std::string> deviceNames_;   // by library slot, from the unit
    std::optional<std::pair<hx::editor::CurrentInfo, std::vector<std::uint8_t>>> devicePreset_;  // the latest one
    struct LocalKnob {
        int slot = -1, set = 0, knob = -1;
        double at = 0;
    } lastKnob_;                              // our last knob send: its echo from the unit is ignored for a moment
    bool noticeFlow_ = false, noticeParams_ = false;  // what the frame's unit events changed (flushDeviceNotices)
    mutable std::map<std::uint16_t, int> blockTypes_;  // model -> preset key 9, filled on first use
    std::vector<hx::Preset> undo_, redo_;
    std::string lastEditKey_;
    std::chrono::steady_clock::time_point lastEditAt_;
    Selection sel_;
    struct Drag {
        enum class Kind { None, Block, Model } kind = Kind::None;
        int id = -1;  // cell id (Block) or entry index in list_ (Model)
    } drag_;
    bool dragging_ = false;  // a block or model is being dragged: serial paths show a grey lane B to drop on
    void setDragging(bool on);
    // The grey lane B: its path (-1 = none) and the split position. Kept while the pointer crosses the gaps between
    // cells (no drop target there); cleared over another lane's cell or when the drag ends.
    int ghostDsp_ = -1, ghostSplit_ = 0;
    int setlist_ = 0;
    std::vector<int> subLists_;  // list indices of the current category
    std::vector<std::uint16_t> splitTypes_;  // model list while a split is selected
    std::vector<int> ioChoices_;             // model list while an input / output is selected: port values
    const hx::MetaEntry *inputAMeta_ = nullptr, *inputBMeta_ = nullptr, *outputMeta_ = nullptr;  // rec 4, per firmware
    int list_ = -1;              // current browser list

    Rml::DataModelHandle handle_;
    std::string status_ = "No firmware loaded";
    bool loaded_ = false;
    std::vector<UiItem> setlists_, presets_, categories_, subs_, models_;
    std::vector<std::string> irNames_;  // the unit's IR slots 1..128 (index 0..127)
    std::vector<int> irKinds_;          // per slot: 0 empty / not read, 1 IR, 2 NAM capture
    int irKind(int slot) const;         // slot 1-based
    // A block's IR slot knobs against what the slots hold: "" when fine, else a warning (an IR block on a NAM
    // capture plays it as a loud burst; a NAM block on an IR doesn't work).
    std::string irMismatch(const hx::MetaEntry *meta, const std::vector<hx::PresetValue> *values) const;
    bool wantsNam(const hx::MetaEntry *meta) const;  // a block whose IR slot knob picks a NAM capture (not an IR block)
    std::vector<UiItem> irChoices_;     // the dropdown: index = slot number (1-based), label "60  name"
    int irOpen_ = -1, irOpenSlot_ = -1; // the param whose IR dropdown is open, and the block slot it belongs to
    bool irShown_ = false;              // the popup list (window level, under the param's dropdown head)
    std::string irLeft_, irTop_, irHeight_;
    void closeIr();
    int irScrollTries_ = 0;             // frames left to scroll the popup to the current slot
    void scrollIrList();
    std::string irLabel(int slot) const;
    void rebuildIrChoices();
    void pickIr(int slot);
    std::string setlistLabel_, subLabel_;
    bool setlistOpen_ = false, subOpen_ = false;
    int presetSel_ = -1, catSel_ = -1, modelSel_ = -1;
    int dropId_ = -1;  // cell under a drag, highlighted
    std::string presetName_ = "No preset";
    bool hasClip_ = false;
    std::string dsp1_, dsp2_, dsp1W_ = "0%", dsp2W_ = "0%";
    std::vector<UiLane> lanes_;
    bool hasBlock_ = false, blockOn_ = false;
    bool canCopyBlock_ = false, canPasteBlock_ = false;
    bool confirmSave_ = false;   // the setting
    bool presetModified_ = false;  // the open preset differs from what's stored ("*" in the bar and the list)
    void setModified(bool on);
    bool saveAsking_ = false;    // the save confirmation is shown
    std::string saveQuestion_;
    std::string saveName_;       // the name the save sends (cleaned for the unit)
    std::function<void()> pendingWrite_;  // what the strip's "Save" does (Ask first)
    void askOrDo(std::string question, std::function<void()> write);
    bool saveAsPicking_ = false; // Save as: the next click in the preset list picks the slot
    ContextMenu *menu_ = nullptr;
    void clickCell(int id);      // a click on a flow cell (selects it here and on the unit)
    void clickPreset(int index); // a click on a preset row (Save as target, load, or open)
    void cellMenu(Rml::Event &ev, int id);
    void paramMenu(Rml::Event &ev, int index);
    void presetMenu(Rml::Event &ev, int index);
    void snapshotMenu(Rml::Event &ev, int index);
    void resetParam(int index);  // back to the model's default
    std::optional<std::vector<std::uint8_t>> presetBlob(const hx::Preset &p, std::string *error) const;
    bool sendWholePreset(const std::string &what);  // cmd 21 with the open preset; false (and a status) if it can't
    void setRowLabel(const std::string &name);  // the open preset's list row (offline only)
    // The input block's preset settings (Guitar In-Z 30, Guitar Pad 134; names from the firmware).
    std::vector<hx::GlobalSetting> inputDefs_;
    std::vector<UiChoice> inputSettings_;
    void rebuildInputSettings();
    void setInputSetting(int id, int index);
    std::string slotLabel(int setlist, int preset) const;  // "Setlist 4 08B"
    void copyStored(int index);
    void pasteStored(int index);
    void writeStored(int setlist, int index, const std::vector<std::uint8_t> &blob, const std::string &name,
                     const std::string &what);
    std::string pendingCopyName_;  // the name of the stored preset being copied (its bytes don't hold it)
    int historySlot_ = -1;         // the unit's preset the undo history belongs to (setlist * 128 + preset)
    void openRename(int index);
    void confirmRename();
    void cancelRename();
    bool renaming_ = false;
    bool focusRename_ = false;  // the row's rename field still needs the keyboard focus (update())
    int renameIndex_ = -1, renameSetlist_ = 0;
    std::string renameText_;
    int deviceCurrentSlot() const;
    void askSaveTo(int setlist, int preset);
    void toggleSaveAs();
    void confirmSave();
    void cancelSave();
    std::optional<hx::PresetBlock> blockClip_;  // Ctrl+C on a block
    std::string blockTitle_, blockCls_, editHint_;
    bool editCab_ = false;           // the params pane shows the block's linked cab (PresetBlock::cabValues)
    bool hasCab_ = false;
    std::string mainLabel_, cabLabel_;  // "Amp" / "Cab", or "Cab 1" / "Cab 2" for a dual cab
    std::vector<UiParam> params_;

    std::vector<UiItem> snapshots_;  // label = name, cls = colour class
    int snapSel_ = 0;
    std::string snapName_;           // the rename field (the selected snapshot's name)
    std::string snapTempo_;          // the tempo field
    int snapColour_ = 0;
    std::vector<UiItem> snapColours_;  // 0 = auto, 1..10
    int snapClip_ = -1;              // copied snapshot, -1 = none
    bool hasSnaps_ = false;
    std::string editTab_ = "edit";
    int fsSel_ = -1;  // the switch setSwitchLabel / Colour / Momentary work on

    // Bypass / Controller Assign (HX Edit's layout): the table and the editor for its selected row.
    struct CaRow {
        hx::PresetTarget target;
        int param = -1;   // knob (preset param index); -1 = the block's bypass
        int source = 0;   // hx::controller source; 0 = none (a new row being set up)
    };
    std::vector<CaRow> caRows_;
    std::vector<UiAssignRow> caTable_;
    std::optional<CaRow> caSel_;  // the row being edited (may be a new one: source 0)
    int caSortCol_ = 0;           // 0 block, 1 parameter, 2 source
    bool caSortAsc_ = true;
    bool caHasSel_ = false, caCanBypass_ = false;
    std::string caBlock_, caBlockCls_, caParam_, caSource_, caKind_;  // kind: "", "range", "midi", "switch", "bypass"
    std::string caMinName_, caMaxName_, caMinText_, caMaxText_, caCc_, caLabel_, caLedName_;
    float caMinPos_ = 0, caMaxPos_ = 1;
    bool caMomentary_ = false;
    void rebuildAssignTable();

    // Command Center (HX Edit's window): the slots as a grid (UiItem: index = slot, label = short name, extra = its
    // command, cls = classes) and the selected slot's editor.
    std::vector<UiItem> cmdTop_, cmdRow1_, cmdRow2_, cmdPedals_;
    int cmdSel_ = -1;
    std::string cmdSelName_, cmdSelType_, cmdSnapName_, cmdLabel_, cmdLed_;
    std::vector<UiField> cmdFields_;
    bool cmdHasSel_ = false, cmdIsSwitch_ = false, cmdMomentary_ = false;
    void rebuildCommandGrid();
    void commandTypeMenu(Rml::Event &ev);
    void commandLedMenu(Rml::Event &ev);
    void stepCommandField(int field, int delta);
    static std::string commandSlotName(int slot, bool shortName);
    void pickAssignRow(int index);
    void showAssignRow(int knobIndex, int source);
    void assignParamMenu(Rml::Event &ev);
    void assignSourceMenu(Rml::Event &ev);
    void assignLedMenu(Rml::Event &ev);
    void setAssignSource(int source);
    void setAssignParam(int param);
    int caKnobIndex();                 // the selected row's knob in the edit target's rec 4 list, -1 = none
    std::string caSourceName(int source) const;  // "Footswitch 3", "EXP Pedal 1", ...
    std::string caParamName(const hx::PresetTarget &t, int param);
    std::string caBlockName(const hx::PresetTarget &t, int *category = nullptr);
    std::vector<hx::FootswitchAssign> *selectedSwitchList();
    int cellOfTarget(const hx::PresetTarget &t);
    // The unit's LED colours: index, label = name from the firmware, extra = our swatch "#rrggbb".
    std::vector<UiItem> fsColours_;
};

} // namespace editor
