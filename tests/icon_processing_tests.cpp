#include "brimbar/icon_processing.h"

#include <cstdlib>
#include <iostream>

int main()
{
    if (brimbar::icon_alpha_from_difference(20) != 0 ||
        brimbar::icon_alpha_from_difference(40) != 255 ||
        brimbar::icon_alpha_from_difference(30) < 120 ||
        brimbar::icon_alpha_from_difference(30) > 135) {
        std::cerr << "Icon edge coverage curve is not crisp and deterministic\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
