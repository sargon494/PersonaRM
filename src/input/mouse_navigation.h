#pragma once

// Mouse support for menus, dialogues and inventories.
//   Left click  -> Confirm (on the hovered item; in dialogues also advances the text)
//   Right click -> Cancel
//   Wheel       -> vertical scroll of the active scroll region (wheel up in a dialogue opens the backlog)
//
// The manager is renderer-agnostic: the UI registers rectangles in *virtual* screen
// coordinates (e.g. the PS1's 320x240) and the manager maps window pixels through the
// letterbox viewport. It only produces events; the caller decides what they mean.
//
// Call order per frame: platform events -> MouseNavigationManager::Update(mouse) ->
// InputManager::Update() (which resets the per-frame wheel/delta values).

#include "input/input_manager.h"

#include <cstdint>
#include <vector>

namespace personarm {

enum class NavContext {
    None,       // gameplay: mouse navigation inactive
    Menu,
    Dialogue,
    Inventory,
    Battle
};

struct NavItem {
    int id = -1;
    int x = 0, y = 0, w = 0, h = 0; // virtual coordinates
    bool enabled = true;
};

enum class NavEventType { Hover, Confirm, Cancel, Scroll, OpenBacklog };

struct NavEvent {
    NavEventType type = NavEventType::Hover;
    int item_id = -1;        // Hover/Confirm: item under the cursor (-1 = none)
    int scroll_offset = 0;   // Scroll: new first visible row
    int scroll_delta = 0;    // Scroll: change applied (rows, signed)
};

struct NavSettings {
    int rows_per_notch = 1;
    bool invert_wheel = false;
    float cursor_hide_delay = 3.0f; // seconds of inactivity; <= 0 keeps the cursor always visible
};

class MouseNavigationManager {
public:
    void SetContext(NavContext context);
    NavContext GetContext() const { return m_context; }

    /// Maps window pixels to virtual coordinates. Pointers outside the viewport (letterbox bars)
    /// hit nothing. Default is identity (window == virtual space).
    void SetViewport(int view_x, int view_y, int view_w, int view_h, int virtual_w, int virtual_h);

    void SetItems(std::vector<NavItem> items);
    void ClearItems();

    /// Defines a scrollable list. The wheel only scrolls while the pointer is inside the region
    /// (w == 0 or h == 0 means the whole screen). Resets the offset to 0.
    void SetScrollRegion(int x, int y, int w, int h, int total_rows, int visible_rows);
    void ClearScrollRegion();
    void SetScrollOffset(int offset);
    int GetScrollOffset() const { return m_scroll_offset; }
    int GetMaxScrollOffset() const;

    NavSettings& Settings() { return m_settings; }

    /// Processes one frame of mouse state and returns the events it produced, in order.
    std::vector<NavEvent> Update(const MouseState& mouse, float delta_time = 0.016f);

    int GetHoveredItem() const { return m_hovered; }
    bool IsCursorVisible() const;

    /// The gameplay action a navigation event corresponds to (Confirm/Cancel/DialogueBacklog).
    static bool ToVirtualAction(const NavEvent& event, VirtualAction& out_action);

private:
    bool ToVirtual(int32_t mx, int32_t my, int& vx, int& vy) const;
    int HitTest(int vx, int vy) const;
    bool InScrollRegion(int vx, int vy) const;

    NavContext m_context = NavContext::None;
    NavSettings m_settings;
    std::vector<NavItem> m_items;

    // viewport mapping
    int m_view_x = 0, m_view_y = 0, m_view_w = 0, m_view_h = 0;
    int m_virtual_w = 0, m_virtual_h = 0;
    bool m_has_viewport = false;

    // scroll region
    bool m_has_scroll = false;
    int m_scroll_x = 0, m_scroll_y = 0, m_scroll_w = 0, m_scroll_h = 0;
    int m_total_rows = 0, m_visible_rows = 0, m_scroll_offset = 0;

    // per-frame tracking
    int m_hovered = -1;
    int32_t m_last_x = INT32_MIN, m_last_y = INT32_MIN;
    bool m_prev_left = false, m_prev_right = false;
    bool m_suppress_left = false, m_suppress_right = false; // buttons held while the context changed
    float m_idle_time = 0.0f;
    bool m_started = false;
};

} // namespace personarm
