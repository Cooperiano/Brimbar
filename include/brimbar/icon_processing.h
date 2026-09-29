#pragma once

#include <algorithm>

namespace brimbar {

[[nodiscard]] constexpr int icon_alpha_from_difference(const int difference) noexcept
{
    constexpr int transparent_difference{20};
    constexpr int opaque_difference{40};
    if (difference <= transparent_difference) {
        return 0;
    }
    if (difference >= opaque_difference) {
        return 255;
    }
    return std::clamp(
        (difference - transparent_difference) * 255 /
            (opaque_difference - transparent_difference),
        0,
        255);
}

}  // namespace brimbar
