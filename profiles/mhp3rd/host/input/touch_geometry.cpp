#include "input/touch_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
namespace mhp3rd::input::touch {
namespace {
const char *anchor_name(Anchor a) {
    switch (a) {
    case Anchor::Left:
        return "left";
    case Anchor::Centre:
        return "centre";
    case Anchor::Right:
        return "right";
    }
    return "left";
}

} // namespace
std::string format(const Placement &p) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s %.3f %.3f %.3f 0x%04X %d", anchor_name(p.anchor), static_cast<double>(p.x),
        static_cast<double>(p.y), static_cast<double>(p.size), p.buttons, p.shown ? 1 : 0);
    return text;
}

bool parse(std::string_view text, Placement &out) {
    std::istringstream in{std::string(text)};
    std::string anchor, buttons;
    Placement p;
    int shown = 1;
    if (!(in >> anchor >> p.x >> p.y >> p.size >> buttons >> shown)) return false;
    if (anchor == "left")
        p.anchor = Anchor::Left;
    else if (anchor == "centre")
        p.anchor = Anchor::Centre;
    else if (anchor == "right")
        p.anchor = Anchor::Right;
    else
        return false;
    char *end = nullptr;
    const unsigned long bits = std::strtoul(buttons.c_str(), &end, 0);
    if (end == buttons.c_str() || *end != '\0' || bits > 0xFFFFu) return false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.size)) return false;
    p.buttons = static_cast<std::uint16_t>(bits);
    p.x = std::clamp(p.x, -4.0f, 4.0f);
    p.y = std::clamp(p.y, 0.0f, 1.0f);
    p.size = std::clamp(p.size, kMinElementSize, kMaxElementSize);
    p.shown = shown != 0;
    out = p;
    return true;
}

Area safe_area(float width, float height, Insets insets) {
    Area a;
    a.left = insets.left;
    a.top = insets.top;
    a.width = std::max(1.0f, width - insets.left - insets.right);
    a.height = std::max(1.0f, height - insets.top - insets.bottom);
    return a;
}

Placed place(const Placement &p, const Area &area, float scale, float aspect) {
    const float unit = area.height;
    Placed placed;
    switch (p.anchor) {
    case Anchor::Left:
        placed.centre.x = area.left + p.x * unit;
        break;
    case Anchor::Centre:
        placed.centre.x = area.left + area.width * 0.5f + p.x * unit;
        break;
    case Anchor::Right:
        placed.centre.x = area.left + area.width - p.x * unit;
        break;
    }
    placed.centre.y = area.top + p.y * unit;
    placed.radius = p.size * std::clamp(scale, 0.5f, 2.0f) * unit;
    placed.half_width = placed.radius * aspect;
    return placed;
}

Placement move_to(const Placement &placement, Point centre, const Area &area, bool central) {
    Placement p = placement;
    const float unit = area.height;
    centre.x = std::clamp(centre.x, area.left, area.left + area.width);
    centre.y = std::clamp(centre.y, area.top, area.top + area.height);
    if (central)
        p.anchor = Anchor::Centre;
    else
        p.anchor = centre.x < area.left + area.width * 0.5f ? Anchor::Left : Anchor::Right;
    switch (p.anchor) {
    case Anchor::Left:
        p.x = (centre.x - area.left) / unit;
        break;
    case Anchor::Centre:
        p.x = (centre.x - area.left - area.width * 0.5f) / unit;
        break;
    case Anchor::Right:
        p.x = (area.left + area.width - centre.x) / unit;
        break;
    }
    p.y = (centre.y - area.top) / unit;
    return p;
}

} // namespace mhp3rd::input::touch
