cmake_minimum_required(VERSION 3.25)

project(obdlink_j2534 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(obdlink_core STATIC
    src/api/passthru_api.cpp
    src/channel/bus_presets.cpp
    src/channel/channel.cpp
    src/channel/message_filters.cpp
    src/channel/rx_queue.cpp
    src/device/device.cpp
    src/isotp/isotp_receiver.cpp
    src/j2534/message_codec.cpp
    src/log/trace_log.cpp
    src/stn/stn_adapter.cpp
    src/stn/stn_link.cpp
    src/stn/stn_response.cpp
)
target_include_directories(obdlink_core PUBLIC src)
target_compile_options(obdlink_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(DLL_NAME obdstn32)
    else()
        set(DLL_NAME obdstn64)
    endif()

    add_library(obdlink_j2534 SHARED
        src/api/passthru_exports.cpp
        src/api/passthru.def
        src/config/registry_settings.cpp
        src/transport/win_serial_port.cpp
    )
    target_link_libraries(obdlink_j2534 PRIVATE obdlink_core)
    target_compile_definitions(obdlink_j2534 PRIVATE OBDLINK_J2534_VERSION="${PROJECT_VERSION}")
    target_compile_options(obdlink_j2534 PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(obdlink_j2534 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
    set_target_properties(obdlink_j2534 PROPERTIES OUTPUT_NAME ${DLL_NAME} PREFIX "")
    install(TARGETS obdlink_j2534 RUNTIME DESTINATION .)
    return()
endif()

enable_testing()
find_package(GTest REQUIRED)
add_executable(obdlink_tests
    tests/fake_stn.cpp
    tests/isotp_receiver_test.cpp
    tests/message_filters_test.cpp
    tests/passthru_api_test.cpp
    tests/stn_link_test.cpp
    tests/stn_response_test.cpp
)
target_link_libraries(obdlink_tests PRIVATE obdlink_core GTest::gtest_main)
target_compile_options(obdlink_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
include(GoogleTest)
gtest_discover_tests(obdlink_tests)
