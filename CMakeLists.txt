cmake_minimum_required(VERSION 3.24)
project(Brimbar VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(brimbar_common STATIC
    src/common/version_policy.cpp
    src/common/tray_target.cpp
    src/common/process_icon_match.cpp
)
target_include_directories(brimbar_common PUBLIC include)
target_compile_definitions(brimbar_common PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(brimbar_tray_access STATIC
    src/tray_access/tray_access.cpp
    src/tray_access/native_tray.cpp
)
target_include_directories(brimbar_tray_access PUBLIC include)
target_compile_definitions(brimbar_tray_access PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brimbar_tray_access PUBLIC brimbar_common ole32 oleaut32 uiautomationcore)

add_executable(brimbar_topbar WIN32
    src/host/appbar.cpp
    src/host/main.cpp
    src/host/topbar.manifest
)
target_include_directories(brimbar_topbar PRIVATE include src/host)
target_compile_definitions(brimbar_topbar PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brimbar_topbar PRIVATE brimbar_common brimbar_tray_access shell32 comctl32 dwmapi gdiplus ole32)

add_library(brimbar_explorer_bridge SHARED
    src/bridge/bridge.cpp
)
target_include_directories(brimbar_explorer_bridge PRIVATE include)
target_compile_definitions(brimbar_explorer_bridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN BRIMBAR_BRIDGE_EXPORTS)
target_link_libraries(brimbar_explorer_bridge PRIVATE brimbar_common)

add_executable(brimbar_bridge_controller
    src/bridge_controller/main.cpp
)
target_include_directories(brimbar_bridge_controller PRIVATE include)
target_compile_definitions(brimbar_bridge_controller PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brimbar_bridge_controller PRIVATE brimbar_common)

add_executable(brimbar_tray_probe
    src/probe/main.cpp
    src/host/topbar.manifest
)
target_include_directories(brimbar_tray_probe PRIVATE include)
target_compile_definitions(brimbar_tray_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brimbar_tray_probe PRIVATE brimbar_common brimbar_tray_access ole32)

add_executable(brimbar_service
    src/service/main.cpp
)
target_compile_definitions(brimbar_service PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brimbar_service PRIVATE advapi32 userenv wtsapi32)

enable_testing()
add_executable(version_policy_tests tests/version_policy_tests.cpp)
target_include_directories(version_policy_tests PRIVATE include)
target_link_libraries(version_policy_tests PRIVATE brimbar_common)
add_test(NAME version_policy_tests COMMAND version_policy_tests)

add_executable(bridge_probe_tests tests/bridge_probe_tests.cpp)
target_include_directories(bridge_probe_tests PRIVATE include)
target_link_libraries(bridge_probe_tests PRIVATE brimbar_explorer_bridge)
add_test(NAME bridge_probe_tests COMMAND bridge_probe_tests)

add_executable(tray_target_tests tests/tray_target_tests.cpp)
target_include_directories(tray_target_tests PRIVATE include)
target_link_libraries(tray_target_tests PRIVATE brimbar_common)
add_test(NAME tray_target_tests COMMAND tray_target_tests)

add_executable(icon_processing_tests tests/icon_processing_tests.cpp)
target_include_directories(icon_processing_tests PRIVATE include)
target_link_libraries(icon_processing_tests PRIVATE brimbar_common)
add_test(NAME icon_processing_tests COMMAND icon_processing_tests)

add_executable(process_icon_match_tests tests/process_icon_match_tests.cpp)
target_include_directories(process_icon_match_tests PRIVATE include)
target_link_libraries(process_icon_match_tests PRIVATE brimbar_common)
add_test(NAME process_icon_match_tests COMMAND process_icon_match_tests)

install(TARGETS brimbar_topbar brimbar_service brimbar_explorer_bridge brimbar_bridge_controller brimbar_tray_probe
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)
install(FILES README.md LICENSE CONTRIBUTING.md SECURITY.md DESTINATION .)

if(MSVC)
    foreach(target brimbar_common brimbar_tray_access brimbar_topbar brimbar_service brimbar_explorer_bridge brimbar_bridge_controller brimbar_tray_probe version_policy_tests bridge_probe_tests tray_target_tests icon_processing_tests process_icon_match_tests)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    endforeach()
endif()
