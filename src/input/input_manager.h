#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

namespace personarm {

struct InputProfile;

// ───────────────────────────────────────────────────────────────────────
// Virtual Actions Enumeration
// ───────────────────────────────────────────────────────────────────────

enum class VirtualAction {
    // Navigation / General
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    Confirm,            // Talk, interact, select (Cross / A / Space / LMB)
    Cancel,             // Back, cancel (Circle / B / Esc / RMB)
    Dash,               // Run toggle / hold (L-Shift / B / Square)
    Menu,               // Main menu, party, personas (Triangle / Y / Tab)
    Map,                // Minimap / Full dungeon map (Square / X / M)
    
    // Camera
    CameraRotateLeft,   // Q / L1 / LB
    CameraRotateRight,  // E / R1 / RB
    
    // Modern Quality of Life
    Turbo,              // Speed up game 2x-4x (Tab / R2 / RT)
    AutoBattle,         // Toggle auto combat (R / R1 / RB)
    DialogueBacklog,    // Open message history (Mouse Wheel Up / L2 / LT)
    QuickSave,          // F5
    QuickLoad,          // F9
    
    // Battle Direct Shortcuts (1-6 keys)
    BattleAttack,       // 1
    BattlePersona,      // 2
    BattleFusion,       // 3
    BattleContact,      // 4 (Demon negotiation)
    BattleItem,         // 5
    BattleStrategy,     // 6
    
    Count
};

// ───────────────────────────────────────────────────────────────────────
// Device & Controller Types
// ───────────────────────────────────────────────────────────────────────

enum class ControllerType {
    KeyboardAndMouse,
    GamepadXbox,
    GamepadPlayStation,
    GamepadNintendo,
    GenericGamepad
};

/// Physical device family a binding code belongs to (see input_profile.h).
enum class InputDeviceKind { Keyboard, Mouse, Gamepad };

// ───────────────────────────────────────────────────────────────────────
// Mouse Cursor State for In-Game Menus & Point-and-Click
// ───────────────────────────────────────────────────────────────────────

struct MouseState {
    int32_t x = 0;
    int32_t y = 0;
    int32_t delta_x = 0;
    int32_t delta_y = 0;
    int32_t wheel_delta = 0;
    bool left_button_down = false;
    bool right_button_down = false;
    bool middle_button_down = false;
    bool visible_in_menus = true;
};

// ───────────────────────────────────────────────────────────────────────
// Analog Axes State
// ───────────────────────────────────────────────────────────────────────

struct AnalogAxes {
    float move_x = 0.0f;     // -1.0 (Left) to +1.0 (Right)
    float move_y = 0.0f;     // -1.0 (Up) to +1.0 (Down)
    float camera_x = 0.0f;   // Camera orbit horizontal
    float camera_y = 0.0f;   // Camera orbit vertical
};

// ───────────────────────────────────────────────────────────────────────
// Input Configuration & Rebinding
// ───────────────────────────────────────────────────────────────────────

struct InputConfig {
    float stick_deadzone = 0.15f;
    float mouse_sensitivity = 1.0f;
    bool invert_camera_y = false;
    bool toggle_dash = true;
    bool enable_turbo_hold = true;
    uint32_t turbo_speed = 3;
};

// ───────────────────────────────────────────────────────────────────────
// InputManager Class
// ───────────────────────────────────────────────────────────────────────

class InputManager {
public:
    static InputManager& Instance();

    bool Initialize();
    void Shutdown();

    // Call once per frame
    void Update(float delta_time = 0.016f);

    // Action Query API
    bool IsActionPressed(VirtualAction action) const;
    bool IsActionJustPressed(VirtualAction action) const;
    bool IsActionJustReleased(VirtualAction action) const;

    // Analog Movement & Camera
    const AnalogAxes& GetAnalogAxes() const { return m_axes; }
    const MouseState& GetMouseState() const { return m_mouse; }

    // Controller & Dynamic Glyph API
    ControllerType GetActiveControllerType() const { return m_active_device; }
    std::string GetActionGlyphDescription(VirtualAction action) const;

    // Haptics & Vibration
    void TriggerRumble(float low_freq, float high_freq, uint32_t duration_ms);

    // Settings
    InputConfig& GetConfig() { return m_config; }
    const InputConfig& GetConfig() const { return m_config; }

    // ── Input profiles (remappable bindings) ──────────────────────────
    /// Activates a profile: copies its tuning values into InputConfig, selects the device
    /// used for glyph prompts and routes ProcessRawInput through its bindings.
    void ApplyProfile(const InputProfile& profile);
    bool HasProfile() const { return m_profile != nullptr; }
    const InputProfile* GetProfile() const { return m_profile.get(); }

    /// Feeds a physical input event (SDL-independent). Returns true if any VirtualAction or
    /// mouse state was affected.
    /// Mouse wheel codes ("WheelUp"/"WheelDown") are one-frame pulses: they release on the next Update().
    bool ProcessRawInput(InputDeviceKind device, const std::string& code, bool pressed);

    // Internal simulation & testing triggers
    void SimulateMouseButton(bool left, bool right, bool middle);
    void SimulateMouseWheel(int32_t notches);
    void SimulateActionPress(VirtualAction action);
    void SimulateActionRelease(VirtualAction action);
    void SimulateMouseMove(int32_t x, int32_t y);

private:
    InputManager() = default;
    ~InputManager() = default;

    InputConfig m_config;
    ControllerType m_active_device = ControllerType::KeyboardAndMouse;

    std::vector<bool> m_action_states;
    std::vector<bool> m_previous_action_states;
    std::vector<bool> m_last_update_states; // snapshot taken at the last Update()

    AnalogAxes m_axes;
    MouseState m_mouse;

    std::shared_ptr<InputProfile> m_profile;
    std::vector<VirtualAction> m_pulse_actions;
    std::vector<VirtualAction> m_pulse_release; // pulses to release on the next Update() // actions released at the start of the next Update()

    bool m_initialized = false;
};

} // namespace personarm

