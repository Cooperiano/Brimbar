#include "brimbar/version_policy.h"

#include <cstdlib>
#include <iostream>

int main()
{
    if (brimbar::is_candidate_build(26099U)) {
        std::cerr << "Build 26099 must be rejected\n";
        return EXIT_FAILURE;
    }
    if (!brimbar::is_candidate_build(26100U) || !brimbar::is_candidate_build(26200U)) {
        std::cerr << "Candidate Windows 11 builds must be accepted\n";
        return EXIT_FAILURE;
    }
    if (brimbar::is_candidate_build(26201U)) {
        std::cerr << "Unknown future builds must fail closed\n";
        return EXIT_FAILURE;
    }

    const auto current = brimbar::current_windows_version();
    if (current.major == 0U || current.build == 0U) {
        std::cerr << "Windows version probe failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "Version policy checks passed for build " << current.build << '\n';
    return EXIT_SUCCESS;
}
