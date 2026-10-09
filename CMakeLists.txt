cmake_minimum_required(VERSION 3.25)
project(moorhuhn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(MOORHUHN_BUILD_SDL_ADAPTER "Build the pinned SDL adapter dependencies" ON)
option(MOORHUHN_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(MOORHUHN_SANITIZERS "Enable ASan and UBSan on project targets" OFF)
option(MOORHUHN_DISTRIBUTION "Build relocatable desktop release packages" OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()
if(MOORHUHN_DISTRIBUTION)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|Darwin)$" OR NOT MOORHUHN_BUILD_SDL_ADAPTER)
        message(FATAL_ERROR "MOORHUHN_DISTRIBUTION requires the desktop application")
    endif()
    if(MSVC)
        add_compile_options("/pathmap:${PROJECT_SOURCE_DIR}=." "/pathmap:${CMAKE_BINARY_DIR}=build")
    else()
        add_compile_options("-ffile-prefix-map=${PROJECT_SOURCE_DIR}=."
            "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
            -ffunction-sections -fdata-sections)
        if(APPLE)
            add_link_options("LINKER:-dead_strip" "LINKER:-headerpad_max_install_names")
        else()
            add_link_options("LINKER:--gc-sections")
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # PipeWire inline functions also embed system header paths in SDL.
        add_compile_options("-ffile-prefix-map=/usr/include=include")
        set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    endif()
endif()
include(CTest)
include(cmake/Dependencies.cmake)
if(MOORHUHN_DISTRIBUTION)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES CXX)
    if(NOT ipo_supported)
        message(FATAL_ERROR "Release link-time optimization is unavailable: ${ipo_error}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

add_library(moorhuhn_project_options INTERFACE)
target_compile_features(moorhuhn_project_options INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(moorhuhn_project_options INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if(MOORHUHN_WARNINGS_AS_ERRORS)
        target_compile_options(moorhuhn_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(moorhuhn_project_options INTERFACE -Wall -Wextra -Wpedantic)
    if(MOORHUHN_WARNINGS_AS_ERRORS)
        target_compile_options(moorhuhn_project_options INTERFACE -Werror)
    endif()
endif()
if(MOORHUHN_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(moorhuhn_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(moorhuhn_project_options INTERFACE -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "MOORHUHN_SANITIZERS requires GCC or Clang with ASan and UBSan")
    endif()
endif()

add_library(moorhuhn_core src/game/simulation.cpp src/game/combat.cpp)
add_library(moorhuhn::core ALIAS moorhuhn_core)
target_include_directories(moorhuhn_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(moorhuhn_core PUBLIC moorhuhn_project_options)
set_target_properties(moorhuhn_core PROPERTIES CXX_EXTENSIONS OFF)

add_library(moorhuhn_render src/render/framebuffer.cpp src/render/renderer.cpp src/render/lookup.cpp)
add_library(moorhuhn::render ALIAS moorhuhn_render)
target_include_directories(moorhuhn_render PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(moorhuhn_render PUBLIC moorhuhn::core)
set_target_properties(moorhuhn_render PROPERTIES CXX_EXTENSIONS OFF)

add_library(moorhuhn_world src/game/world.cpp)
add_library(moorhuhn::world ALIAS moorhuhn_world)
target_link_libraries(moorhuhn_world PUBLIC moorhuhn::core moorhuhn::render)
set_target_properties(moorhuhn_world PROPERTIES CXX_EXTENSIONS OFF)

add_library(moorhuhn_specials src/game/specials.cpp)
add_library(moorhuhn::specials ALIAS moorhuhn_specials)
target_link_libraries(moorhuhn_specials PUBLIC moorhuhn::core moorhuhn::world)
set_target_properties(moorhuhn_specials PROPERTIES CXX_EXTENSIONS OFF)

add_library(moorhuhn_ui src/ui/hud.cpp src/ui/menu.cpp src/storage/highscores.cpp)
add_library(moorhuhn::ui ALIAS moorhuhn_ui)
target_include_directories(moorhuhn_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(moorhuhn_ui PUBLIC moorhuhn_project_options)
set_target_properties(moorhuhn_ui PROPERTIES CXX_EXTENSIONS OFF)

add_library(moorhuhn_session src/app/session.cpp)
add_library(moorhuhn::session ALIAS moorhuhn_session)
target_link_libraries(moorhuhn_session PUBLIC moorhuhn::specials moorhuhn::ui)
set_target_properties(moorhuhn_session PROPERTIES CXX_EXTENSIONS OFF)

if(MOORHUHN_BUILD_SDL_ADAPTER)
    add_library(moorhuhn_sdl_adapter INTERFACE)
    add_library(moorhuhn::sdl_adapter ALIAS moorhuhn_sdl_adapter)
    target_link_libraries(moorhuhn_sdl_adapter INTERFACE moorhuhn_core
        SDL3::SDL3 SDL3_mixer::SDL3_mixer)
    add_library(moorhuhn_app INTERFACE)
    add_library(moorhuhn::app ALIAS moorhuhn_app)
    target_link_libraries(moorhuhn_app INTERFACE moorhuhn_sdl_adapter)

    add_library(moorhuhn_platform
        src/platform/clock.cpp
        src/platform/viewport.cpp
        src/platform/input.cpp
        src/platform/paths.cpp
        src/platform/display.cpp)
    add_library(moorhuhn::platform ALIAS moorhuhn_platform)
    target_include_directories(moorhuhn_platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moorhuhn_platform PUBLIC moorhuhn::sdl_adapter)
    set_target_properties(moorhuhn_platform PROPERTIES CXX_EXTENSIONS OFF)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(image_package "${CMAKE_CURRENT_BINARY_DIR}/images.pak")
    set(audio_package "${CMAKE_CURRENT_BINARY_DIR}/audio.pak")
    file(GLOB_RECURSE source_assets CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/assets/*")
    add_custom_command(OUTPUT "${image_package}" "${audio_package}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/pack_assets.py"
            --manifest "${PROJECT_SOURCE_DIR}/assets/manifest.txt"
            --output-dir "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/pack_assets.py" ${source_assets}
        COMMENT "Pack game assets" VERBATIM)
    add_custom_target(moorhuhn_asset_package ALL DEPENDS "${image_package}" "${audio_package}")
    add_library(moorhuhn_assets src/assets/asset_store.cpp src/assets/asset_package.cpp)
    add_library(moorhuhn::assets ALIAS moorhuhn_assets)
    target_include_directories(moorhuhn_assets PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moorhuhn_assets PUBLIC moorhuhn::core
        PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer)
    set_target_properties(moorhuhn_assets PROPERTIES CXX_EXTENSIONS OFF)

    add_library(moorhuhn_audio src/audio/audio_engine.cpp)
    add_library(moorhuhn::audio ALIAS moorhuhn_audio)
    target_include_directories(moorhuhn_audio PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(moorhuhn_audio PUBLIC moorhuhn::core
        PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer moorhuhn::assets)
    set_target_properties(moorhuhn_audio PROPERTIES CXX_EXTENSIONS OFF)

    add_library(moorhuhn_render_presentation src/render/presentation.cpp)
    add_library(moorhuhn::render_presentation ALIAS moorhuhn_render_presentation)
    target_link_libraries(moorhuhn_render_presentation PUBLIC moorhuhn::render moorhuhn::platform)
    set_target_properties(moorhuhn_render_presentation PROPERTIES CXX_EXTENSIONS OFF)

    add_executable(moorhuhn src/app/main.cpp src/app/application.cpp)
    if(WIN32)
        enable_language(RC)
        configure_file(src/platform/windows.rc.in "${CMAKE_CURRENT_BINARY_DIR}/moorhuhn.rc" @ONLY)
        target_sources(moorhuhn PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/moorhuhn.rc")
    endif()
    add_dependencies(moorhuhn moorhuhn_asset_package)
    target_link_libraries(moorhuhn PRIVATE moorhuhn::app moorhuhn::platform
        moorhuhn::session moorhuhn::assets moorhuhn::audio moorhuhn::render_presentation)
    set_target_properties(moorhuhn PROPERTIES CXX_EXTENSIONS OFF)
    if(MOORHUHN_DISTRIBUTION)
        include(cmake/Install.cmake)
    endif()
    add_custom_target(run COMMAND "$<TARGET_FILE:moorhuhn>" DEPENDS moorhuhn
        VERBATIM USES_TERMINAL)
else()
    add_custom_target(run
        COMMAND "${CMAKE_COMMAND}" -E echo "Application requires MOORHUHN_BUILD_SDL_ADAPTER=ON."
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM USES_TERMINAL)
endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    add_subdirectory(tests/contracts)
    add_executable(moorhuhn_simulation_tests tests/game/simulation.cpp)
    target_link_libraries(moorhuhn_simulation_tests PRIVATE moorhuhn::core)
    set_target_properties(moorhuhn_simulation_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME game.simulation COMMAND moorhuhn_simulation_tests
        --fixtures "${PROJECT_SOURCE_DIR}/tests/game/fixtures/simulation")
    add_executable(moorhuhn_combat_tests tests/game/combat.cpp)
    target_link_libraries(moorhuhn_combat_tests PRIVATE moorhuhn::core moorhuhn::render)
    set_target_properties(moorhuhn_combat_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME game.combat COMMAND moorhuhn_combat_tests
        --fixtures "${PROJECT_SOURCE_DIR}/tests/game/fixtures/combat")
    add_executable(moorhuhn_world_tests tests/game/world.cpp)
    target_link_libraries(moorhuhn_world_tests PRIVATE moorhuhn::world)
    set_target_properties(moorhuhn_world_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME game.world COMMAND moorhuhn_world_tests)
    add_executable(moorhuhn_specials_tests tests/game/specials.cpp)
    target_link_libraries(moorhuhn_specials_tests PRIVATE moorhuhn::specials)
    set_target_properties(moorhuhn_specials_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME game.specials COMMAND moorhuhn_specials_tests)
    foreach(unit IN ITEMS menu hud)
        add_executable(moorhuhn_${unit}_tests "tests/ui/${unit}.cpp")
        target_link_libraries(moorhuhn_${unit}_tests PRIVATE moorhuhn::ui)
        set_target_properties(moorhuhn_${unit}_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME ui.${unit} COMMAND moorhuhn_${unit}_tests)
    endforeach()
    add_executable(moorhuhn_highscores_tests tests/storage/highscores.cpp)
    target_link_libraries(moorhuhn_highscores_tests PRIVATE moorhuhn::ui)
    set_target_properties(moorhuhn_highscores_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME storage.highscores COMMAND moorhuhn_highscores_tests)
    add_executable(moorhuhn_audio_clock_tests tests/app/audio_clock.cpp)
    target_link_libraries(moorhuhn_audio_clock_tests PRIVATE moorhuhn::core)
    set_target_properties(moorhuhn_audio_clock_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME app.audio_clock COMMAND moorhuhn_audio_clock_tests)
    add_executable(moorhuhn_renderer_tests tests/render/renderer.cpp)
    target_link_libraries(moorhuhn_renderer_tests PRIVATE moorhuhn::render)
    set_target_properties(moorhuhn_renderer_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME render.pixels COMMAND moorhuhn_renderer_tests)
    find_program(CLANG_FORMAT_EXECUTABLE clang-format)
    if(CLANG_FORMAT_EXECUTABLE)
        add_test(NAME lint.format COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/FormatCheck.cmake")
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME tools.python COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${PROJECT_SOURCE_DIR}/tests/tools" -p "*.py" -v)
        add_test(NAME tools.native COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${PROJECT_SOURCE_DIR}/tests/integration" -p "test_*.py" -v)
    endif()
    if(MOORHUHN_BUILD_SDL_ADAPTER)
        set(asset_root "${PROJECT_SOURCE_DIR}/assets")
        set(fixture_root "${PROJECT_SOURCE_DIR}/tests/fixtures")

        add_executable(moorhuhn_round_tests tests/app/session.cpp)
        target_link_libraries(moorhuhn_round_tests PRIVATE moorhuhn::session moorhuhn::assets)
        set_target_properties(moorhuhn_round_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME app.session COMMAND moorhuhn_round_tests
            "${asset_root}/manifest.txt")
        set_tests_properties(app.session PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=moorhuhn-disabled")
        target_link_libraries(moorhuhn_world_tests PRIVATE moorhuhn::assets)
        target_compile_definitions(moorhuhn_world_tests PRIVATE MOORHUHN_WORLD_ASSET_TESTS=1)
        add_test(NAME game.world.assets COMMAND moorhuhn_world_tests
            --assets "${asset_root}/manifest.txt")

        add_executable(moorhuhn_audio_tests tests/audio/audio_engine.cpp)
        target_link_libraries(moorhuhn_audio_tests PRIVATE moorhuhn::audio moorhuhn::assets
            SDL3::SDL3 SDL3_mixer::SDL3_mixer)
        set_target_properties(moorhuhn_audio_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME audio.adapter COMMAND moorhuhn_audio_tests
            --fixtures "${fixture_root}/audio")
        set_tests_properties(audio.adapter PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
        add_test(NAME audio.device COMMAND moorhuhn_audio_tests
            --fixtures "${fixture_root}/audio" --device)
        set_tests_properties(audio.device PROPERTIES
            ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
        add_test(NAME audio.assets COMMAND moorhuhn_audio_tests
            --fixtures "${fixture_root}/audio"
            --assets "${asset_root}"
            --asset-fixtures "${fixture_root}/assets/loader")
        set_tests_properties(audio.assets PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")

        add_executable(moorhuhn_render_presentation_tests tests/render/presentation.cpp)
        target_link_libraries(moorhuhn_render_presentation_tests PRIVATE moorhuhn::render_presentation)
        set_target_properties(moorhuhn_render_presentation_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME render.presentation COMMAND moorhuhn_render_presentation_tests)
        set_tests_properties(render.presentation PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")

        add_executable(moorhuhn_platform_viewport_tests tests/platform/viewport.cpp)
        target_link_libraries(moorhuhn_platform_viewport_tests PRIVATE moorhuhn::platform)
        set_target_properties(moorhuhn_platform_viewport_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME platform.viewport COMMAND moorhuhn_platform_viewport_tests)

        add_executable(moorhuhn_platform_tests tests/platform/input.cpp)
        target_link_libraries(moorhuhn_platform_tests PRIVATE moorhuhn::platform)
        set_target_properties(moorhuhn_platform_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME platform.adapters COMMAND moorhuhn_platform_tests
            --resource-root "${PROJECT_SOURCE_DIR}/tests/platform/resources")
        set_tests_properties(platform.adapters PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
        if(UNIX AND NOT APPLE)
            set_property(TEST platform.adapters APPEND PROPERTY ENVIRONMENT
                "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-user")
        endif()

        add_executable(moorhuhn_asset_loader_tests tests/assets/asset_store.cpp)
        add_dependencies(moorhuhn_asset_loader_tests moorhuhn_asset_package)
        target_link_libraries(moorhuhn_asset_loader_tests PRIVATE moorhuhn::assets
            SDL3::SDL3)
        set_target_properties(moorhuhn_asset_loader_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME assets.loader COMMAND moorhuhn_asset_loader_tests
            --fixtures "${fixture_root}/assets/loader")
        add_test(NAME assets.loader.game COMMAND moorhuhn_asset_loader_tests
            --fixtures "${fixture_root}/assets/loader"
            --assets "${asset_root}")
        add_test(NAME assets.loader.package COMMAND moorhuhn_asset_loader_tests
            --fixtures "${fixture_root}/assets/loader"
            --assets "${asset_root}" --package "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

if(BUILD_TESTING AND MOORHUHN_DISTRIBUTION)
    # Source-path mapping makes fixture paths derived from __FILE__ relative.
    foreach(test IN ITEMS game.world game.world.assets game.specials app.session)
        if(TEST ${test})
            set_tests_properties(${test} PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        endif()
    endforeach()
endif()
