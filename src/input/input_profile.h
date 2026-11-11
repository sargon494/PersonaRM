#pragma once

// Configurable input profiles: which physical key / mouse button / gamepad button
// triggers each VirtualAction. Profiles are serialized into personarm_config.json.
//
// Binding codes are SDL-independent, positional names so the config file stays
// portable ("South" is Cross on PlayStation pads and A on Xbox pads).

#include "core/json_lite.h"
#include "input/input_manager.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace personarm {

struct InputBinding {
    InputDeviceKind device = InputDeviceKind::Keyboard;
    std::string code;

    bool operator==(const InputBinding& o) const { return device == o.device && code == o.code; }
    bool operator!=(const InputBinding& o) const { return !(*this == o); }
};

const char* DeviceKindToString(InputDeviceKind kind);
std::optional<InputDeviceKind> DeviceKindFromString(const std::string& s);
const char* ActionToString(VirtualAction action);
std::optional<VirtualAction> ActionFromString(const std::string& s);

/// True if `code` is a known name for the given device (e.g. Keyboard:"F5", Mouse:"WheelUp", Gamepad:"South").
bool IsValidBindingCode(InputDeviceKind device, const std::string& code);

enum class ConflictPolicy {
    Reject, // leave everything untouched and report the conflicting action
    Steal,  // remove the code from the other action, bind it here
    Swap    // give the other action this action's previous binding(s) for that device
};

enum class RebindResult { Ok, Conflict, InvalidBinding };

struct RebindOutcome {
    RebindResult result = RebindResult::Ok;
    std::optional<VirtualAction> conflicting_action;
};

struct InputProfile {
    std::string name = "Default";
    ControllerType controller = ControllerType::KeyboardAndMouse;

    float stick_deadzone = 0.15f;
    float mouse_sensitivity = 1.0f;
    bool invert_camera_y = false;
    bool toggle_dash = true;
    bool enable_turbo_hold = true;
    uint32_t turbo_speed = 3;

    std::map<VirtualAction, std::vector<InputBinding>> bindings;

    static InputProfile DefaultKeyboardMouse();
    static InputProfile DefaultXbox();
    static InputProfile DefaultPlayStation();
    static InputProfile DefaultFor(ControllerType type);

    const std::vector<InputBinding>& BindingsFor(VirtualAction action) const;

    /// Actions triggered by a physical input (can be several if the profile shares a code).
    std::vector<VirtualAction> ActionsForCode(InputDeviceKind device, const std::string& code) const;

    /// Replace every binding of `binding.device` for `action` with `binding` (primary rebind).
    RebindOutcome Rebind(VirtualAction action, const InputBinding& binding,
                         ConflictPolicy policy = ConflictPolicy::Reject);

    /// Add an extra binding without removing existing ones (e.g. Confirm: Space + Enter).
    RebindOutcome AddBinding(VirtualAction action, const InputBinding& binding,
                             ConflictPolicy policy = ConflictPolicy::Reject);

    void ClearBindings(VirtualAction action, std::optional<InputDeviceKind> only_device = std::nullopt);

    /// Human-readable text for UI prompts, e.g. "Space / Enter / LMB".
    std::string GlyphFor(VirtualAction action) const;

    /// Add defaults for any action absent from `bindings` (used after loading older configs).
    /// An action explicitly present with an empty list stays unbound.
    void FillMissingFromDefaults();

    JsonValue ToJson() const;
    /// Returns nullopt only if `j` is not an object; unknown/invalid entries are skipped
    /// and counted in *skipped.
    static std::optional<InputProfile> FromJson(const JsonValue& j, size_t* skipped = nullptr);
};

class InputProfileManager {
public:
    InputProfileManager();

    const std::vector<InputProfile>& Profiles() const { return m_profiles; }
    InputProfile& Active();
    const InputProfile& Active() const;
    const std::string& ActiveName() const { return m_profiles[m_active].name; }

    bool SetActive(const std::string& name);
    InputProfile* Find(const std::string& name);

    /// Adds a copy of `source_name` under `new_name`. Fails if the name is empty/taken or source is missing.
    bool Clone(const std::string& source_name, const std::string& new_name);
    /// Removes a profile; the last remaining profile cannot be removed.
    bool Remove(const std::string& name);
    /// Restores a built-in profile to defaults (keeps its name and controller type).
    bool ResetProfile(const std::string& name);
    void ResetAll();

    JsonValue ToJson() const;
    /// Replaces the current profiles. Falls back to the built-ins if nothing valid is found.
    /// Returns the number of skipped (invalid) entries.
    size_t FromJson(const JsonValue& j);

private:
    void EnsureValid();

    std::vector<InputProfile> m_profiles;
    size_t m_active = 0;
};

} // namespace personarm
