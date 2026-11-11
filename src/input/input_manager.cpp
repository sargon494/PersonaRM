#include "input_manager.h"
#include "input_profile.h"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace personarm {

InputManager& InputManager::Instance() {
    static InputManager instance;
    return instance;
}

bool InputManager::Initialize() {
    size_t count = static_cast<size_t>(VirtualAction::Count);
    m_action_states.assign(count, false);
    m_previous_action_states.assign(count, false);
    m_last_update_states.assign(count, false);

    m_axes = AnalogAxes{};
    m_mouse = MouseState{};
    m_active_device = ControllerType::KeyboardAndMouse;
    m_profile.reset();
    m_pulse_actions.clear();

    m_initialized = true;
    return true;
}

void InputManager::Shutdown() {
    m_initialized = false;
}

void InputManager::Update(float delta_time) {
    (void)delta_time;
    if (!m_initialized) return;

    // One-frame pulses (mouse wheel): a pulse fired between two Update() calls is visible as
    // JustPressed during the next Update and released at the one after that.
    for (VirtualAction a : m_pulse_release) SimulateActionRelease(a);
    m_pulse_release = std::move(m_pulse_actions);
    m_pulse_actions.clear();

    // Edge detection: "previous" is the state captured at the prior Update();
    // events that arrived since then are already in m_action_states.
    m_previous_action_states = m_last_update_states;
    m_last_update_states = m_action_states;

    // Reset per-frame deltas
    m_mouse.delta_x = 0;
    m_mouse.delta_y = 0;
    m_mouse.wheel_delta = 0;

    // Compute analog movement from digital actions if axes are zero
    if (std::abs(m_axes.move_x) < 0.01f && std::abs(m_axes.move_y) < 0.01f) {
        float x = 0.0f;
        float y = 0.0f;
        if (IsActionPressed(VirtualAction::MoveRight)) x += 1.0f;
        if (IsActionPressed(VirtualAction::MoveLeft))  x -= 1.0f;
        if (IsActionPressed(VirtualAction::MoveDown))  y += 1.0f;
        if (IsActionPressed(VirtualAction::MoveUp))    y -= 1.0f;

        // Normalize diagonal vector to avoid running faster diagonally
        if (x != 0.0f && y != 0.0f) {
            float len = std::sqrt(x * x + y * y);
            x /= len;
            y /= len;
        }
        m_axes.move_x = x;
        m_axes.move_y = y;
    }
}

bool InputManager::IsActionPressed(VirtualAction action) const {
    size_t idx = static_cast<size_t>(action);
    return idx < m_action_states.size() ? m_action_states[idx] : false;
}

bool InputManager::IsActionJustPressed(VirtualAction action) const {
    size_t idx = static_cast<size_t>(action);
    if (idx >= m_action_states.size()) return false;
    return m_action_states[idx] && !m_previous_action_states[idx];
}

bool InputManager::IsActionJustReleased(VirtualAction action) const {
    size_t idx = static_cast<size_t>(action);
    if (idx >= m_action_states.size()) return false;
    return !m_action_states[idx] && m_previous_action_states[idx];
}

std::string InputManager::GetActionGlyphDescription(VirtualAction action) const {
    if (m_profile) return m_profile->GlyphFor(action);
    switch (m_active_device) {
        case ControllerType::KeyboardAndMouse:
            switch (action) {
                case VirtualAction::MoveUp:             return "W";
                case VirtualAction::MoveDown:           return "S";
                case VirtualAction::MoveLeft:           return "A";
                case VirtualAction::MoveRight:          return "D";
                case VirtualAction::Confirm:            return "Space / LMB";
                case VirtualAction::Cancel:             return "Esc / RMB";
                case VirtualAction::Dash:               return "L-Shift";
                case VirtualAction::Menu:               return "Tab";
                case VirtualAction::Map:                return "M";
                case VirtualAction::CameraRotateLeft:   return "Q";
                case VirtualAction::CameraRotateRight:  return "E";
                case VirtualAction::Turbo:              return "Hold Tab";
                case VirtualAction::AutoBattle:         return "R";
                case VirtualAction::DialogueBacklog:    return "Wheel Up";
                case VirtualAction::QuickSave:          return "F5";
                case VirtualAction::QuickLoad:          return "F9";
                case VirtualAction::BattleAttack:       return "[1] Attack";
                case VirtualAction::BattlePersona:      return "[2] Persona";
                case VirtualAction::BattleFusion:       return "[3] Fusion";
                case VirtualAction::BattleContact:      return "[4] Contact";
                case VirtualAction::BattleItem:         return "[5] Item";
                case VirtualAction::BattleStrategy:     return "[6] Strategy";
                default:                                return "";
            }

        case ControllerType::GamepadPlayStation:
            switch (action) {
                case VirtualAction::MoveUp:
                case VirtualAction::MoveDown:
                case VirtualAction::MoveLeft:
                case VirtualAction::MoveRight:          return "Left Stick";
                case VirtualAction::Confirm:            return "✕ Cross";
                case VirtualAction::Cancel:             return "○ Circle";
                case VirtualAction::Dash:               return "Hold ○ Circle";
                case VirtualAction::Menu:               return "△ Triangle / Options";
                case VirtualAction::Map:                return "□ Square / Touchpad";
                case VirtualAction::CameraRotateLeft:   return "L1 / Right Stick Left";
                case VirtualAction::CameraRotateRight:  return "R1 / Right Stick Right";
                case VirtualAction::Turbo:              return "Hold R2";
                case VirtualAction::AutoBattle:         return "R1";
                case VirtualAction::DialogueBacklog:    return "L2";
                case VirtualAction::QuickSave:          return "L3 + R3";
                case VirtualAction::QuickLoad:          return "L3 + L1";
                default:                                return "";
            }

        case ControllerType::GamepadXbox:
        case ControllerType::GamepadNintendo:
        case ControllerType::GenericGamepad:
        default:
            switch (action) {
                case VirtualAction::MoveUp:
                case VirtualAction::MoveDown:
                case VirtualAction::MoveLeft:
                case VirtualAction::MoveRight:          return "Left Stick";
                case VirtualAction::Confirm:            return "A Button";
                case VirtualAction::Cancel:             return "B Button";
                case VirtualAction::Dash:               return "Hold B";
                case VirtualAction::Menu:               return "Y / Menu";
                case VirtualAction::Map:                return "X / View";
                case VirtualAction::CameraRotateLeft:   return "LB / Right Stick Left";
                case VirtualAction::CameraRotateRight:  return "RB / Right Stick Right";
                case VirtualAction::Turbo:              return "Hold RT";
                case VirtualAction::AutoBattle:         return "RB";
                case VirtualAction::DialogueBacklog:    return "LT";
                case VirtualAction::QuickSave:          return "LS + RS";
                case VirtualAction::QuickLoad:          return "LS + LB";
                default:                                return "";
            }
    }
}

void InputManager::TriggerRumble(float low_freq, float high_freq, uint32_t duration_ms) {
    // When SDL2 is integrated, calls SDL_GameControllerRumble
    // For now, logged in diagnostics
    (void)low_freq;
    (void)high_freq;
    (void)duration_ms;
}

void InputManager::SimulateActionPress(VirtualAction action) {
    size_t idx = static_cast<size_t>(action);
    if (idx < m_action_states.size()) {
        m_action_states[idx] = true;
    }
}

void InputManager::SimulateActionRelease(VirtualAction action) {
    size_t idx = static_cast<size_t>(action);
    if (idx < m_action_states.size()) {
        m_action_states[idx] = false;
    }
}

void InputManager::ApplyProfile(const InputProfile& profile) {
    m_profile = std::make_shared<InputProfile>(profile);
    m_active_device = profile.controller;
    m_config.stick_deadzone = profile.stick_deadzone;
    m_config.mouse_sensitivity = profile.mouse_sensitivity;
    m_config.invert_camera_y = profile.invert_camera_y;
    m_config.toggle_dash = profile.toggle_dash;
    m_config.enable_turbo_hold = profile.enable_turbo_hold;
    m_config.turbo_speed = profile.turbo_speed;
    // Drop held state: bindings may have changed under the player's fingers.
    std::fill(m_action_states.begin(), m_action_states.end(), false);
}

bool InputManager::ProcessRawInput(InputDeviceKind kind, const std::string& code, bool pressed) {
    if (!m_initialized || !m_profile) return false;

    bool affected = false;
    const bool is_wheel = kind == InputDeviceKind::Mouse && (code == "WheelUp" || code == "WheelDown");
    if (kind == InputDeviceKind::Mouse) {
        if (code == "MouseLeft") { m_mouse.left_button_down = pressed; affected = true; }
        else if (code == "MouseRight") { m_mouse.right_button_down = pressed; affected = true; }
        else if (code == "MouseMiddle") { m_mouse.middle_button_down = pressed; affected = true; }
        else if (is_wheel && pressed) { m_mouse.wheel_delta += code == "WheelUp" ? 1 : -1; affected = true; }
        else if (is_wheel) return false; // wheels have no release event
    }

    for (VirtualAction action : m_profile->ActionsForCode(kind, code)) {
        if (pressed) {
            SimulateActionPress(action);
            if (is_wheel) m_pulse_actions.push_back(action);
        } else {
            SimulateActionRelease(action);
        }
        affected = true;
    }
    return affected;
}

void InputManager::SimulateMouseButton(bool left, bool right, bool middle) {
    m_mouse.left_button_down = left;
    m_mouse.right_button_down = right;
    m_mouse.middle_button_down = middle;
}

void InputManager::SimulateMouseWheel(int32_t notches) {
    m_mouse.wheel_delta += notches;
}

void InputManager::SimulateMouseMove(int32_t x, int32_t y) {
    m_mouse.delta_x = x - m_mouse.x;
    m_mouse.delta_y = y - m_mouse.y;
    m_mouse.x = x;
    m_mouse.y = y;
}

} // namespace personarm

