#include "input_profile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace personarm {

namespace {

struct ActionName { VirtualAction action; const char* name; };

constexpr std::array<ActionName, static_cast<size_t>(VirtualAction::Count)> kActionNames = {{
    {VirtualAction::MoveUp, "MoveUp"},
    {VirtualAction::MoveDown, "MoveDown"},
    {VirtualAction::MoveLeft, "MoveLeft"},
    {VirtualAction::MoveRight, "MoveRight"},
    {VirtualAction::Confirm, "Confirm"},
    {VirtualAction::Cancel, "Cancel"},
    {VirtualAction::Dash, "Dash"},
    {VirtualAction::Menu, "Menu"},
    {VirtualAction::Map, "Map"},
    {VirtualAction::CameraRotateLeft, "CameraRotateLeft"},
    {VirtualAction::CameraRotateRight, "CameraRotateRight"},
    {VirtualAction::Turbo, "Turbo"},
    {VirtualAction::AutoBattle, "AutoBattle"},
    {VirtualAction::DialogueBacklog, "DialogueBacklog"},
    {VirtualAction::QuickSave, "QuickSave"},
    {VirtualAction::QuickLoad, "QuickLoad"},
    {VirtualAction::BattleAttack, "BattleAttack"},
    {VirtualAction::BattlePersona, "BattlePersona"},
    {VirtualAction::BattleFusion, "BattleFusion"},
    {VirtualAction::BattleContact, "BattleContact"},
    {VirtualAction::BattleItem, "BattleItem"},
    {VirtualAction::BattleStrategy, "BattleStrategy"},
}};

const std::set<std::string>& KeyboardCodes() {
    static const std::set<std::string> codes = [] {
        std::set<std::string> s;
        for (char c = 'A'; c <= 'Z'; ++c) s.insert(std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) s.insert(std::string(1, c));
        for (int i = 1; i <= 12; ++i) s.insert("F" + std::to_string(i));
        for (const char* k : {"Space", "Tab", "Esc", "Enter", "Backspace", "LShift", "RShift", "LCtrl", "RCtrl",
                              "LAlt", "RAlt", "Up", "Down", "Left", "Right", "Backquote", "Minus", "Equals",
                              "Comma", "Period", "Slash", "Home", "End", "PageUp", "PageDown", "Insert", "Delete"}) {
            s.insert(k);
        }
        return s;
    }();
    return codes;
}

const std::set<std::string>& MouseCodes() {
    static const std::set<std::string> codes = {"MouseLeft", "MouseRight", "MouseMiddle", "WheelUp", "WheelDown"};
    return codes;
}

const std::set<std::string>& GamepadCodes() {
    static const std::set<std::string> codes = {"South", "East", "West", "North", "LB", "RB", "LT", "RT", "L3", "R3",
                                                "Start", "Back", "DPadUp", "DPadDown", "DPadLeft", "DPadRight"};
    return codes;
}

const std::vector<InputBinding>& EmptyBindings() {
    static const std::vector<InputBinding> empty;
    return empty;
}

InputBinding Key(const char* c) { return {InputDeviceKind::Keyboard, c}; }
InputBinding Mouse(const char* c) { return {InputDeviceKind::Mouse, c}; }
InputBinding Pad(const char* c) { return {InputDeviceKind::Gamepad, c}; }

std::string PadLabel(const std::string& code, ControllerType type) {
    if (type == ControllerType::GamepadPlayStation) {
        if (code == "South") return "Cross";
        if (code == "East") return "Circle";
        if (code == "West") return "Square";
        if (code == "North") return "Triangle";
        if (code == "LB") return "L1";
        if (code == "RB") return "R1";
        if (code == "LT") return "L2";
        if (code == "RT") return "R2";
        if (code == "Back") return "Select";
    } else if (type == ControllerType::GamepadNintendo) {
        if (code == "South") return "B";
        if (code == "East") return "A";
        if (code == "West") return "Y";
        if (code == "North") return "X";
        if (code == "LB") return "L";
        if (code == "RB") return "R";
        if (code == "LT") return "ZL";
        if (code == "RT") return "ZR";
        if (code == "Back") return "-";
        if (code == "Start") return "+";
    } else { // Xbox / generic
        if (code == "South") return "A";
        if (code == "East") return "B";
        if (code == "West") return "X";
        if (code == "North") return "Y";
        if (code == "Back") return "View";
        if (code == "Start") return "Menu";
    }
    return code;
}

std::string BindingLabel(const InputBinding& b, ControllerType type) {
    if (b.device == InputDeviceKind::Gamepad) return PadLabel(b.code, type);
    if (b.device == InputDeviceKind::Mouse) {
        if (b.code == "MouseLeft") return "LMB";
        if (b.code == "MouseRight") return "RMB";
        if (b.code == "MouseMiddle") return "MMB";
        if (b.code == "WheelUp") return "Wheel Up";
        if (b.code == "WheelDown") return "Wheel Down";
    }
    return b.code;
}

} // namespace

const char* DeviceKindToString(InputDeviceKind kind) {
    switch (kind) {
        case InputDeviceKind::Keyboard: return "keyboard";
        case InputDeviceKind::Mouse: return "mouse";
        case InputDeviceKind::Gamepad: return "gamepad";
    }
    return "keyboard";
}

std::optional<InputDeviceKind> DeviceKindFromString(const std::string& s) {
    if (s == "keyboard") return InputDeviceKind::Keyboard;
    if (s == "mouse") return InputDeviceKind::Mouse;
    if (s == "gamepad") return InputDeviceKind::Gamepad;
    return std::nullopt;
}

const char* ActionToString(VirtualAction action) {
    for (const auto& e : kActionNames) if (e.action == action) return e.name;
    return "Unknown";
}

std::optional<VirtualAction> ActionFromString(const std::string& s) {
    for (const auto& e : kActionNames) if (s == e.name) return e.action;
    return std::nullopt;
}

bool IsValidBindingCode(InputDeviceKind device, const std::string& code) {
    switch (device) {
        case InputDeviceKind::Keyboard: return KeyboardCodes().count(code) != 0;
        case InputDeviceKind::Mouse: return MouseCodes().count(code) != 0;
        case InputDeviceKind::Gamepad: return GamepadCodes().count(code) != 0;
    }
    return false;
}

// ── Defaults ───────────────────────────────────────────────────────────

InputProfile InputProfile::DefaultKeyboardMouse() {
    InputProfile p;
    p.name = "Keyboard & Mouse";
    p.controller = ControllerType::KeyboardAndMouse;
    auto& b = p.bindings;
    b[VirtualAction::MoveUp] = {Key("W"), Key("Up")};
    b[VirtualAction::MoveDown] = {Key("S"), Key("Down")};
    b[VirtualAction::MoveLeft] = {Key("A"), Key("Left")};
    b[VirtualAction::MoveRight] = {Key("D"), Key("Right")};
    b[VirtualAction::Confirm] = {Key("Space"), Key("Enter"), Mouse("MouseLeft")};
    b[VirtualAction::Cancel] = {Key("Esc"), Mouse("MouseRight")};
    b[VirtualAction::Dash] = {Key("LShift")};
    b[VirtualAction::Menu] = {Key("Tab")};
    b[VirtualAction::Map] = {Key("M")};
    b[VirtualAction::CameraRotateLeft] = {Key("Q")};
    b[VirtualAction::CameraRotateRight] = {Key("E")};
    b[VirtualAction::Turbo] = {Key("LCtrl")};
    b[VirtualAction::AutoBattle] = {Key("R")};
    b[VirtualAction::DialogueBacklog] = {Mouse("WheelUp"), Key("B")};
    b[VirtualAction::QuickSave] = {Key("F5")};
    b[VirtualAction::QuickLoad] = {Key("F9")};
    b[VirtualAction::BattleAttack] = {Key("1")};
    b[VirtualAction::BattlePersona] = {Key("2")};
    b[VirtualAction::BattleFusion] = {Key("3")};
    b[VirtualAction::BattleContact] = {Key("4")};
    b[VirtualAction::BattleItem] = {Key("5")};
    b[VirtualAction::BattleStrategy] = {Key("6")};
    return p;
}

static InputProfile DefaultGamepad(ControllerType type, const char* name) {
    InputProfile p;
    p.name = name;
    p.controller = type;
    auto& b = p.bindings;
    b[VirtualAction::MoveUp] = {Pad("DPadUp")};
    b[VirtualAction::MoveDown] = {Pad("DPadDown")};
    b[VirtualAction::MoveLeft] = {Pad("DPadLeft")};
    b[VirtualAction::MoveRight] = {Pad("DPadRight")};
    b[VirtualAction::Confirm] = {Pad("South")};
    b[VirtualAction::Cancel] = {Pad("East")};
    b[VirtualAction::Dash] = {Pad("West")};
    b[VirtualAction::Menu] = {Pad("North")};
    b[VirtualAction::Map] = {Pad("Back")};
    b[VirtualAction::CameraRotateLeft] = {Pad("LB")};
    b[VirtualAction::CameraRotateRight] = {Pad("RB")};
    b[VirtualAction::Turbo] = {Pad("RT")};
    b[VirtualAction::AutoBattle] = {Pad("R3")};
    b[VirtualAction::DialogueBacklog] = {Pad("LT")};
    b[VirtualAction::QuickSave] = {};
    b[VirtualAction::QuickLoad] = {};
    b[VirtualAction::BattleAttack] = {};
    b[VirtualAction::BattlePersona] = {};
    b[VirtualAction::BattleFusion] = {};
    b[VirtualAction::BattleContact] = {};
    b[VirtualAction::BattleItem] = {};
    b[VirtualAction::BattleStrategy] = {};
    return p;
}

InputProfile InputProfile::DefaultXbox() { return DefaultGamepad(ControllerType::GamepadXbox, "Xbox Controller"); }
InputProfile InputProfile::DefaultPlayStation() { return DefaultGamepad(ControllerType::GamepadPlayStation, "PlayStation Controller"); }

InputProfile InputProfile::DefaultFor(ControllerType type) {
    switch (type) {
        case ControllerType::KeyboardAndMouse: return DefaultKeyboardMouse();
        case ControllerType::GamepadPlayStation: return DefaultPlayStation();
        case ControllerType::GamepadNintendo: {
            InputProfile p = DefaultGamepad(ControllerType::GamepadNintendo, "Nintendo Controller");
            return p;
        }
        case ControllerType::GamepadXbox:
        case ControllerType::GenericGamepad:
        default: {
            InputProfile p = DefaultXbox();
            if (type == ControllerType::GenericGamepad) { p.name = "Generic Gamepad"; p.controller = type; }
            return p;
        }
    }
}

// ── Queries / mutation ─────────────────────────────────────────────────

const std::vector<InputBinding>& InputProfile::BindingsFor(VirtualAction action) const {
    auto it = bindings.find(action);
    return it == bindings.end() ? EmptyBindings() : it->second;
}

std::vector<VirtualAction> InputProfile::ActionsForCode(InputDeviceKind device, const std::string& code) const {
    std::vector<VirtualAction> result;
    for (const auto& kv : bindings) {
        for (const auto& b : kv.second) {
            if (b.device == device && b.code == code) { result.push_back(kv.first); break; }
        }
    }
    return result;
}

namespace {

// Returns the first action other than `self` that already uses `binding`.
std::optional<VirtualAction> FindConflict(const InputProfile& p, VirtualAction self, const InputBinding& binding) {
    for (const auto& kv : p.bindings) {
        if (kv.first == self) continue;
        for (const auto& b : kv.second) if (b == binding) return kv.first;
    }
    return std::nullopt;
}

} // namespace

RebindOutcome InputProfile::Rebind(VirtualAction action, const InputBinding& binding, ConflictPolicy policy) {
    RebindOutcome out;
    if (static_cast<size_t>(action) >= static_cast<size_t>(VirtualAction::Count) ||
        !IsValidBindingCode(binding.device, binding.code)) {
        out.result = RebindResult::InvalidBinding;
        return out;
    }

    // Previous bindings of this action on the same device (used by Swap).
    std::vector<InputBinding> previous;
    for (const auto& b : BindingsFor(action)) if (b.device == binding.device) previous.push_back(b);

    // Collect every action that currently uses the code (profiles may share codes by design).
    std::vector<VirtualAction> holders;
    for (const auto& kv : bindings) {
        if (kv.first == action) continue;
        for (const auto& b : kv.second) if (b == binding) { holders.push_back(kv.first); break; }
    }

    if (!holders.empty()) {
        out.conflicting_action = holders.front();
        if (policy == ConflictPolicy::Reject) { out.result = RebindResult::Conflict; return out; }
        for (VirtualAction other : holders) {
            auto& list = bindings[other];
            list.erase(std::remove(list.begin(), list.end(), binding), list.end());
            if (policy == ConflictPolicy::Swap) {
                for (const auto& prev : previous) {
                    if (std::find(list.begin(), list.end(), prev) == list.end()) list.push_back(prev);
                }
            }
        }
    }

    auto& mine = bindings[action];
    mine.erase(std::remove_if(mine.begin(), mine.end(),
                              [&](const InputBinding& b) { return b.device == binding.device; }),
               mine.end());
    mine.push_back(binding);
    return out;
}

RebindOutcome InputProfile::AddBinding(VirtualAction action, const InputBinding& binding, ConflictPolicy policy) {
    RebindOutcome out;
    if (static_cast<size_t>(action) >= static_cast<size_t>(VirtualAction::Count) ||
        !IsValidBindingCode(binding.device, binding.code)) {
        out.result = RebindResult::InvalidBinding;
        return out;
    }
    auto conflict = FindConflict(*this, action, binding);
    if (conflict) {
        out.conflicting_action = conflict;
        if (policy == ConflictPolicy::Reject) { out.result = RebindResult::Conflict; return out; }
        // Steal and Swap behave the same here: there is no "previous" binding being replaced.
        for (auto& kv : bindings) {
            if (kv.first == action) continue;
            kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), binding), kv.second.end());
        }
    }
    auto& mine = bindings[action];
    if (std::find(mine.begin(), mine.end(), binding) == mine.end()) mine.push_back(binding);
    return out;
}

void InputProfile::ClearBindings(VirtualAction action, std::optional<InputDeviceKind> only_device) {
    auto& list = bindings[action];
    if (!only_device) { list.clear(); return; }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const InputBinding& b) { return b.device == *only_device; }),
               list.end());
}

std::string InputProfile::GlyphFor(VirtualAction action) const {
    const auto& list = BindingsFor(action);
    if (list.empty()) return "Unbound";
    std::string out;
    for (const auto& b : list) {
        if (!out.empty()) out += " / ";
        out += BindingLabel(b, controller);
    }
    return out;
}

void InputProfile::FillMissingFromDefaults() {
    InputProfile defaults = DefaultFor(controller);
    for (const auto& kv : defaults.bindings) {
        if (bindings.find(kv.first) == bindings.end()) bindings[kv.first] = kv.second;
    }
}

// ── JSON ───────────────────────────────────────────────────────────────

namespace {

const char* ControllerToString(ControllerType t) {
    switch (t) {
        case ControllerType::KeyboardAndMouse: return "keyboard_mouse";
        case ControllerType::GamepadXbox: return "xbox";
        case ControllerType::GamepadPlayStation: return "playstation";
        case ControllerType::GamepadNintendo: return "nintendo";
        case ControllerType::GenericGamepad: return "generic";
    }
    return "keyboard_mouse";
}

std::optional<ControllerType> ControllerFromString(const std::string& s) {
    if (s == "keyboard_mouse") return ControllerType::KeyboardAndMouse;
    if (s == "xbox") return ControllerType::GamepadXbox;
    if (s == "playstation") return ControllerType::GamepadPlayStation;
    if (s == "nintendo") return ControllerType::GamepadNintendo;
    if (s == "generic") return ControllerType::GenericGamepad;
    return std::nullopt;
}

float ClampF(double v, float lo, float hi) {
    return static_cast<float>(std::min<double>(hi, std::max<double>(lo, v)));
}

} // namespace

JsonValue InputProfile::ToJson() const {
    JsonValue j = JsonValue::MakeObject();
    j.Set("name", name);
    j.Set("controller", ControllerToString(controller));
    j.Set("stick_deadzone", static_cast<double>(stick_deadzone));
    j.Set("mouse_sensitivity", static_cast<double>(mouse_sensitivity));
    j.Set("invert_camera_y", invert_camera_y);
    j.Set("toggle_dash", toggle_dash);
    j.Set("enable_turbo_hold", enable_turbo_hold);
    j.Set("turbo_speed", turbo_speed);

    JsonValue map = JsonValue::MakeObject();
    for (const auto& kv : bindings) {
        JsonValue arr = JsonValue::MakeArray();
        for (const auto& b : kv.second) {
            JsonValue e = JsonValue::MakeObject();
            e.Set("device", DeviceKindToString(b.device));
            e.Set("code", b.code);
            arr.Push(std::move(e));
        }
        map.Set(ActionToString(kv.first), std::move(arr));
    }
    j.Set("bindings", std::move(map));
    return j;
}

std::optional<InputProfile> InputProfile::FromJson(const JsonValue& j, size_t* skipped) {
    if (!j.IsObject()) return std::nullopt;
    size_t bad = 0;

    InputProfile p;
    p.name = j.StringOr("name", "Profile");
    if (p.name.empty()) p.name = "Profile";
    if (const JsonValue* c = j.Find("controller"); c && c->IsString()) {
        if (auto t = ControllerFromString(c->AsString())) p.controller = *t; else ++bad;
    }
    p.stick_deadzone = ClampF(j.NumberOr("stick_deadzone", p.stick_deadzone), 0.0f, 0.9f);
    p.mouse_sensitivity = ClampF(j.NumberOr("mouse_sensitivity", p.mouse_sensitivity), 0.1f, 10.0f);
    p.invert_camera_y = j.BoolOr("invert_camera_y", p.invert_camera_y);
    p.toggle_dash = j.BoolOr("toggle_dash", p.toggle_dash);
    p.enable_turbo_hold = j.BoolOr("enable_turbo_hold", p.enable_turbo_hold);
    p.turbo_speed = static_cast<uint32_t>(std::min(4.0, std::max(2.0, j.NumberOr("turbo_speed", p.turbo_speed))));

    if (const JsonValue* map = j.Find("bindings"); map && map->IsObject()) {
        for (const auto& kv : map->Members()) {
            auto action = ActionFromString(kv.first);
            if (!action || !kv.second.IsArray()) { ++bad; continue; }
            auto& list = p.bindings[*action]; // present even if empty => explicitly unbound
            for (const auto& e : kv.second.Items()) {
                auto dev = e.IsObject() ? DeviceKindFromString(e.StringOr("device", "")) : std::nullopt;
                std::string code = e.IsObject() ? e.StringOr("code", "") : "";
                if (!dev || !IsValidBindingCode(*dev, code)) { ++bad; continue; }
                InputBinding b{*dev, code};
                if (std::find(list.begin(), list.end(), b) == list.end()) list.push_back(b);
            }
            // A non-empty list where every entry was invalid must not leave the action unbound
            // (e.g. no way to confirm): fall back to the defaults instead.
            if (list.empty() && !kv.second.Items().empty()) p.bindings.erase(*action);
        }
    }
    p.FillMissingFromDefaults();
    if (skipped) *skipped += bad;
    return p;
}

// ── InputProfileManager ────────────────────────────────────────────────

InputProfileManager::InputProfileManager() { ResetAll(); }

void InputProfileManager::ResetAll() {
    m_profiles = {InputProfile::DefaultKeyboardMouse(), InputProfile::DefaultXbox(), InputProfile::DefaultPlayStation()};
    m_active = 0;
}

InputProfile& InputProfileManager::Active() { return m_profiles[m_active]; }
const InputProfile& InputProfileManager::Active() const { return m_profiles[m_active]; }

InputProfile* InputProfileManager::Find(const std::string& name) {
    for (auto& p : m_profiles) if (p.name == name) return &p;
    return nullptr;
}

bool InputProfileManager::SetActive(const std::string& name) {
    for (size_t i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name == name) { m_active = i; return true; }
    }
    return false;
}

bool InputProfileManager::Clone(const std::string& source_name, const std::string& new_name) {
    if (new_name.empty() || Find(new_name)) return false;
    InputProfile* src = Find(source_name);
    if (!src) return false;
    InputProfile copy = *src;
    copy.name = new_name;
    m_profiles.push_back(std::move(copy));
    return true;
}

bool InputProfileManager::Remove(const std::string& name) {
    if (m_profiles.size() <= 1) return false;
    for (size_t i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name != name) continue;
        const std::string active_name = m_profiles[m_active].name;
        m_profiles.erase(m_profiles.begin() + static_cast<std::ptrdiff_t>(i));
        m_active = 0;
        for (size_t k = 0; k < m_profiles.size(); ++k) if (m_profiles[k].name == active_name) m_active = k;
        return true;
    }
    return false;
}

bool InputProfileManager::ResetProfile(const std::string& name) {
    InputProfile* p = Find(name);
    if (!p) return false;
    InputProfile defaults = InputProfile::DefaultFor(p->controller);
    defaults.name = p->name;
    *p = std::move(defaults);
    return true;
}

JsonValue InputProfileManager::ToJson() const {
    JsonValue j = JsonValue::MakeObject();
    j.Set("active_profile", m_profiles[m_active].name);
    JsonValue arr = JsonValue::MakeArray();
    for (const auto& p : m_profiles) arr.Push(p.ToJson());
    j.Set("profiles", std::move(arr));
    return j;
}

void InputProfileManager::EnsureValid() {
    if (m_profiles.empty()) ResetAll();
    if (m_active >= m_profiles.size()) m_active = 0;
}

size_t InputProfileManager::FromJson(const JsonValue& j) {
    size_t skipped = 0;
    std::vector<InputProfile> loaded;
    std::set<std::string> names;
    if (const JsonValue* arr = j.Find("profiles"); arr && arr->IsArray()) {
        for (const auto& e : arr->Items()) {
            auto p = InputProfile::FromJson(e, &skipped);
            if (!p || !names.insert(p->name).second) { ++skipped; continue; } // drop duplicates by name
            loaded.push_back(std::move(*p));
        }
    }
    m_profiles = std::move(loaded);
    m_active = 0;
    EnsureValid();
    std::string wanted = j.StringOr("active_profile", "");
    if (!wanted.empty() && !SetActive(wanted)) ++skipped;
    return skipped;
}

} // namespace personarm
