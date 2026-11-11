#include "mouse_navigation.h"

#include <algorithm>

namespace personarm {

void MouseNavigationManager::SetContext(NavContext context) {
    if (context == m_context) return;
    m_context = context;
    m_hovered = -1;
    m_items.clear();
    ClearScrollRegion();
    // A button still held from the previous screen must not click the new one.
    m_suppress_left = m_prev_left;
    m_suppress_right = m_prev_right;
}

void MouseNavigationManager::SetViewport(int view_x, int view_y, int view_w, int view_h, int virtual_w, int virtual_h) {
    m_has_viewport = view_w > 0 && view_h > 0 && virtual_w > 0 && virtual_h > 0;
    m_view_x = view_x;
    m_view_y = view_y;
    m_view_w = view_w;
    m_view_h = view_h;
    m_virtual_w = virtual_w;
    m_virtual_h = virtual_h;
}

void MouseNavigationManager::SetItems(std::vector<NavItem> items) {
    m_items = std::move(items);
    // Force the next Update to re-evaluate hover even if the pointer is stationary.
    m_last_x = INT32_MIN;
    m_last_y = INT32_MIN;
}

void MouseNavigationManager::ClearItems() {
    m_items.clear();
    m_hovered = -1;
}

void MouseNavigationManager::SetScrollRegion(int x, int y, int w, int h, int total_rows, int visible_rows) {
    m_has_scroll = true;
    m_scroll_x = x;
    m_scroll_y = y;
    m_scroll_w = w;
    m_scroll_h = h;
    m_total_rows = std::max(0, total_rows);
    m_visible_rows = std::max(1, visible_rows);
    m_scroll_offset = 0;
}

void MouseNavigationManager::ClearScrollRegion() {
    m_has_scroll = false;
    m_total_rows = m_visible_rows = m_scroll_offset = 0;
}

int MouseNavigationManager::GetMaxScrollOffset() const {
    return m_has_scroll ? std::max(0, m_total_rows - m_visible_rows) : 0;
}

void MouseNavigationManager::SetScrollOffset(int offset) {
    m_scroll_offset = std::clamp(offset, 0, GetMaxScrollOffset());
}

bool MouseNavigationManager::ToVirtual(int32_t mx, int32_t my, int& vx, int& vy) const {
    if (!m_has_viewport) { vx = mx; vy = my; return true; }
    if (mx < m_view_x || my < m_view_y || mx >= m_view_x + m_view_w || my >= m_view_y + m_view_h) return false;
    vx = static_cast<int>((static_cast<int64_t>(mx - m_view_x) * m_virtual_w) / m_view_w);
    vy = static_cast<int>((static_cast<int64_t>(my - m_view_y) * m_virtual_h) / m_view_h);
    return true;
}

int MouseNavigationManager::HitTest(int vx, int vy) const {
    // Later items are drawn on top, so scan from the back.
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it) {
        if (!it->enabled) continue;
        if (vx >= it->x && vx < it->x + it->w && vy >= it->y && vy < it->y + it->h) return it->id;
    }
    return -1;
}

bool MouseNavigationManager::InScrollRegion(int vx, int vy) const {
    if (!m_has_scroll) return false;
    if (m_scroll_w <= 0 || m_scroll_h <= 0) return true; // whole screen
    return vx >= m_scroll_x && vx < m_scroll_x + m_scroll_w && vy >= m_scroll_y && vy < m_scroll_y + m_scroll_h;
}

bool MouseNavigationManager::IsCursorVisible() const {
    if (m_context == NavContext::None) return false;
    if (m_settings.cursor_hide_delay <= 0.0f) return true;
    return m_idle_time < m_settings.cursor_hide_delay;
}

std::vector<NavEvent> MouseNavigationManager::Update(const MouseState& mouse, float delta_time) {
    std::vector<NavEvent> events;

    const bool moved = !m_started || mouse.x != m_last_x || mouse.y != m_last_y;
    const bool left_edge = mouse.left_button_down && !m_prev_left;
    const bool right_edge = mouse.right_button_down && !m_prev_right;
    const int wheel = std::clamp(mouse.wheel_delta, -64, 64);

    // Held-through-context-change buttons become clickable again once released.
    if (!mouse.left_button_down) m_suppress_left = false;
    if (!mouse.right_button_down) m_suppress_right = false;

    m_prev_left = mouse.left_button_down;
    m_prev_right = mouse.right_button_down;
    m_started = true;

    if (moved || left_edge || right_edge || wheel != 0) m_idle_time = 0.0f;
    else m_idle_time += delta_time;

    if (m_context == NavContext::None) {
        m_last_x = mouse.x;
        m_last_y = mouse.y;
        return events;
    }

    int vx = 0, vy = 0;
    const bool inside = ToVirtual(mouse.x, mouse.y, vx, vy);
    const int under_cursor = inside ? HitTest(vx, vy) : -1;

    // Hover follows the pointer only when it moves (or the item set changed), so keyboard/pad
    // focus is not stolen by a stationary mouse.
    if (moved && under_cursor != m_hovered) {
        m_hovered = under_cursor;
        NavEvent e;
        e.type = NavEventType::Hover;
        e.item_id = m_hovered;
        events.push_back(e);
    }
    m_last_x = mouse.x;
    m_last_y = mouse.y;

    if (left_edge && !m_suppress_left && inside) {
        if (under_cursor >= 0 || m_context == NavContext::Dialogue) {
            NavEvent e;
            e.type = NavEventType::Confirm;
            e.item_id = under_cursor;
            events.push_back(e);
        }
    }

    if (right_edge && !m_suppress_right) {
        NavEvent e;
        e.type = NavEventType::Cancel;
        events.push_back(e);
    }

    if (wheel != 0) {
        const bool up = (wheel > 0) != m_settings.invert_wheel;
        if (m_has_scroll && inside && InScrollRegion(vx, vy)) {
            // wheel up scrolls toward the top of the list (smaller offset)
            const int before = m_scroll_offset;
            const int delta = (up ? -1 : 1) * std::abs(wheel) * std::max(1, m_settings.rows_per_notch);
            SetScrollOffset(m_scroll_offset + delta);
            if (m_scroll_offset != before) {
                NavEvent e;
                e.type = NavEventType::Scroll;
                e.scroll_offset = m_scroll_offset;
                e.scroll_delta = m_scroll_offset - before;
                events.push_back(e);
            }
        } else if (m_context == NavContext::Dialogue && up) {
            NavEvent e;
            e.type = NavEventType::OpenBacklog;
            events.push_back(e);
        }
    }

    return events;
}

bool MouseNavigationManager::ToVirtualAction(const NavEvent& event, VirtualAction& out_action) {
    switch (event.type) {
        case NavEventType::Confirm: out_action = VirtualAction::Confirm; return true;
        case NavEventType::Cancel: out_action = VirtualAction::Cancel; return true;
        case NavEventType::OpenBacklog: out_action = VirtualAction::DialogueBacklog; return true;
        default: return false;
    }
}

} // namespace personarm
