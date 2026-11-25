#include "spell_ring.hpp"

#include <algorithm>
#include <cmath>

namespace ring {
namespace {
constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kSectorWidth = 360.0f / kSectors;

// The angle between two directions, 0 to 180.
float angle_between(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}
}  // namespace

float sector_angle(int sector, Orientation orientation) {
    return float(sector) * kSectorWidth + (orientation == Orientation::Top ? kSectorWidth / 2 : 0.0f);
}

int element_of(int sector, Orientation orientation) {
    // Sides: right, upper right, upper left, left, lower left, lower right.
    // Top: upper right, top, upper left, lower left, bottom, lower right.
    static constexpr int kSides[kSectors] = {1, 0, 5, 4, 3, 2};
    static constexpr int kTop[kSectors] = {2, 1, 0, 5, 4, 3};
    const int s = (sector % kSectors + kSectors) % kSectors;
    return orientation == Orientation::Top ? kTop[s] : kSides[s];
}

Layout layout(float x, float y, float width, float height, int size_percent, int x_percent, int y_percent,
              int player, bool multiplayer) {
    Layout result;
    result.size = height * size_percent / 100.0f * (multiplayer ? 0.5f : 1.0f);
    if (multiplayer) {
        const float qx = x + (player % 2) * width / 2;
        const float qy = y + (player / 2) * height / 2;
        result.x = qx + width / 4;
        result.y = qy + height / 4;
        result.indicator_x = result.x;
        result.indicator_y = qy + height * 0.07f;
        result.indicator_size = height * 0.07f;
    } else {
        const float reach = kDiscRadius * result.size;
        result.x = width > 2 * reach ? x + reach + (width - 2 * reach) * x_percent / 100.0f : x + width / 2;
        result.y = height > 2 * reach ? y + reach + (height - 2 * reach) * y_percent / 100.0f : y + height / 2;
    }
    return result;
}

HudIcon hud_icon(float x, float y, float width, float height, int wide_extra, bool edges, int hud_scale, int slot) {
    const float pixel = width / (256.0f + 2.0f * wide_extra);
    const float scale = hud_scale / 100.0f;
    const bool right = slot == 1 || slot == 3;
    const bool top = slot >= 2;
    const float anchor_x = right ? 256.0f : 0.0f;
    const float anchor_y = top ? 0.0f : 192.0f;
    const float ds_x[] = {135.0f, 190.0f, 66.0f, 190.0f};
    const float ds_y[] = {170.0f, 183.0f, 10.0f, 10.0f};
    const float shift = edges ? (right ? float(wide_extra) : -float(wide_extra)) : 0.0f;
    return {x + (wide_extra + anchor_x + shift + (ds_x[slot] - anchor_x) * scale) * pixel,
            y + (anchor_y + (ds_y[slot] - anchor_y) * scale) * height / 192.0f,
            11.0f * scale * height / 192.0f};
}

State pick(float x, float y, const State& previous, Orientation orientation, float open_at, float close_at, float margin) {
    const float length = std::hypot(x, y);
    State next;
    if (length < (previous.open ? close_at : open_at)) return next;
    next.open = true;
    float angle = std::atan2(-y, x) * kRadToDeg;  // (y is down on the screen)
    if (angle < 0) angle += 360.0f;
    const float first = sector_angle(0, orientation);
    float from_first = angle - first;
    if (from_first < 0) from_first += 360.0f;
    int nearest = int(std::lround(from_first / kSectorWidth)) % kSectors;
    if (previous.open && previous.sector >= 0 &&
        angle_between(angle, sector_angle(previous.sector, orientation)) <= kSectorWidth / 2 + margin)
        nearest = previous.sector;
    next.sector = nearest;
    return next;
}

}  // namespace ring
