#pragma once

#include <cstdint>

namespace brimbar {

struct windows_version final {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t build{};
};

[[nodiscard]] windows_version current_windows_version() noexcept;
[[nodiscard]] constexpr bool is_candidate_build(const std::uint32_t build) noexcept
{
    return build >= 26100U && build <= 26200U;
}

}  // namespace brimbar
