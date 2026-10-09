cmake_minimum_required(VERSION 3.25)
project(EFMLNative VERSION 1.0.0 LANGUAGES C CXX RC)

# Same layout as Better-Endfield's native build: MSVC, C++20, static CRT so the runtime never
# shares the game's MSVCP140. build.cmd builds the same targets with cl only (no CMake needed).
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EFML targets Windows x64 with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(EFML_STAGE ${CMAKE_BINARY_DIR}/stage/$<CONFIG>)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8 $<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->)

add_library(minhook STATIC
    vendor/minhook/src/buffer.c
    vendor/minhook/src/hook.c
    vendor/minhook/src/trampoline.c
    vendor/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC vendor/minhook/include)

add_library(EFML.Host SHARED
    host/bootstrap.cpp
    host/dynamic_resolver.cpp
    host/game_thread.cpp
    host/hook_broker.cpp
    host/hook_diagnostics.cpp
    host/host_runtime.cpp
    host/logging.cpp
    host/module_host.cpp
    host/paths.cpp)
target_include_directories(EFML.Host PRIVATE include)
target_link_libraries(EFML.Host PRIVATE minhook ws2_32 bcrypt user32)
set_target_properties(EFML.Host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/runtime)

add_library(EFML.Bootstrap SHARED loader/input_proxy.cpp)
target_compile_definitions(EFML.Bootstrap PRIVATE EFML_BOOTSTRAP)
set_target_properties(EFML.Bootstrap PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/runtime)

add_library(EFML.XInputProxy SHARED loader/input_proxy.cpp)
target_compile_definitions(EFML.XInputProxy PRIVATE EFML_XINPUT_PROXY)
set_target_properties(EFML.XInputProxy PROPERTIES OUTPUT_NAME xinput1_4 PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/runtime)

add_executable(EFML.Injector loader/injector.cpp loader/injector.rc)
target_link_options(EFML.Injector PRIVATE /MANIFEST:NO)
set_target_properties(EFML.Injector PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/runtime)

# SDK example and tests.
add_library(example.echo SHARED ../sdk/examples/echo/native/echo.cpp)
target_include_directories(example.echo PRIVATE include)
set_target_properties(example.echo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/sdk)

add_library(test.legacy SHARED tests/legacy_module.cpp)
target_include_directories(test.legacy PRIVATE include)
set_target_properties(test.legacy PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/tests)

add_executable(efml-tests
    tests/module_host_test.cpp
    host/game_thread.cpp
    host/logging.cpp
    host/module_host.cpp
    host/paths.cpp)
target_include_directories(efml-tests PRIVATE include)
target_link_libraries(efml-tests PRIVATE ws2_32 bcrypt)
set_target_properties(efml-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EFML_STAGE}/tests)
add_dependencies(efml-tests example.echo test.legacy)

enable_testing()
add_test(NAME efml-module-host COMMAND efml-tests $<TARGET_FILE:example.echo> $<TARGET_FILE:test.legacy>)
