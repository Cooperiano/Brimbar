#include "brimbar/bridge_api.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>

int main()
{
    static_assert(std::is_standard_layout_v<brimbar::bridge_runtime_state>);
    static_assert(brimbar::bridge_protocol_magic == 0x4252494DU);
    static_assert(brimbar::bridge_protocol_version == 1U);

    brimbar::bridge_probe_result result{};
    result.structure_size = sizeof(result);

    if (!brimbar_bridge_probe(&result)) {
        std::cerr << "Bridge probe rejected a valid result structure\n";
        return EXIT_FAILURE;
    }
    if (result.windows_build == 0U || result.message[0] == L'\0') {
        std::cerr << "Bridge probe returned incomplete diagnostics\n";
        return EXIT_FAILURE;
    }
    if (result.implementation_ready) {
        std::cerr << "Explorer mutation must remain disabled in this milestone\n";
        return EXIT_FAILURE;
    }

    std::wcout << L"Bridge safety probe passed for build " << result.windows_build << L'\n';
    return EXIT_SUCCESS;
}
