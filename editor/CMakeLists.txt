add_executable(giant_editor
  src/main.cpp
  src/animation_commands.cpp
  src/animation_state.cpp
  src/cloth_commands.cpp
  src/cloth_state.cpp
  src/swing_commands.cpp
  src/swing_state.cpp
  src/nav_commands.cpp
  src/nav_state.cpp
  src/walker_commands.cpp
  src/walker_state.cpp
  src/audio_commands.cpp
  src/audio_state.cpp
  src/game_saves.cpp
  src/save_commands.cpp
  src/hit_targets.cpp
  src/editor_commands.cpp
  src/frame_times.cpp
  src/mesh_library.cpp
  src/scene_commands.cpp
  src/scene_render.cpp
  src/terrain_commands.cpp
  src/scatter_commands.cpp
  src/terrain_meshes.cpp
  src/terrain_brush_tool.cpp
  src/far_ground.cpp
  src/scatter_placer.cpp
  src/weather_state.cpp
  src/weather_commands.cpp
  src/physics_state.cpp
  src/physics_commands.cpp
  src/play_state.cpp
  src/viewport_camera.cpp
  src/world_streaming.cpp
  src/ui/imgui_layer.cpp
  src/ui/panels.cpp
)
target_link_libraries(giant_editor PRIVATE giant::core giant::platform giant::rhi giant::geometry giant::render giant::world giant::sim giant::nav giant::audio giant::save giant::script
                      giant::ai third_party::imgui)
if(GIANT_DOTNET)
  add_dependencies(giant_editor giant_managed) # the C# API ships next to the editor
endif()
giant_apply_warnings(giant_editor)
giant_apply_sanitizers(giant_editor)
giant_d3d12_app(giant_editor) # Agility SDK runtime and WARP next to the exe

# Smoke test: open a hidden window, render 30 frames, exit cleanly.
if(GIANT_BUILD_TESTS)
  if(WIN32)
    # CI sets GIANT_REQUIRE_VALIDATION=ON after installing Graphics Tools, so
    # "no validation errors" cannot pass with the layer silently missing.
    if(GIANT_REQUIRE_VALIDATION)
      add_test(NAME editor_smoke COMMAND giant_editor --frames 30 --hidden --software --require-validation)
    else()
      add_test(NAME editor_smoke COMMAND giant_editor --frames 30 --hidden --software --debug-layer)
    endif()
  else()
    add_test(NAME editor_smoke COMMAND giant_editor --frames 30 --hidden --backend null)
    set_tests_properties(editor_smoke PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
  endif()
  set_tests_properties(editor_smoke PROPERTIES TIMEOUT 120)

  # Drive the editor over MCP the way Claude Code does.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    if(WIN32)
      set(mcp_backend --software --debug-layer)
    else()
      set(mcp_backend --backend null)
    endif()
    add_test(NAME editor_mcp
      COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/mcp_smoke.py
              $<TARGET_FILE:giant_editor> --hidden ${mcp_backend})
    set(mcp_env "")
    if(NOT WIN32)
      list(APPEND mcp_env "SDL_VIDEO_DRIVER=dummy")
    endif()
    # The C# sample runs too, except under ThreadSanitizer (which cannot
    # follow .NET's threads). .NET handles its own segfaults under ASan.
    if(GIANT_DOTNET AND NOT GIANT_SANITIZE_THREAD)
      list(APPEND mcp_env
        "GIANT_SAMPLE_SCRIPTS=${CMAKE_BINARY_DIR}/managed/samples/HelloGiant/HelloGiant.dll"
        "ASAN_OPTIONS=handle_segv=0:allow_user_segv_handler=1:detect_leaks=0:use_sigaltstack=0")
    endif()
    set_tests_properties(editor_mcp PROPERTIES TIMEOUT 2460 ENVIRONMENT "${mcp_env}")
    # The gates' summaries of a flight (no editor).
    add_test(NAME gate_common COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/test_gate_common.py)
    # The N2 gate's scene and flight, on the null device (the software
    # renderer would take hours): everything but the speed. Optimised builds
    # only: a debug or sanitizer build takes most of an hour over it.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      add_test(NAME editor_n2_gate
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/n2_gate.py
                $<TARGET_FILE:giant_editor> --hidden --backend null)
      set(gate_env "")
      if(NOT WIN32)
        list(APPEND gate_env "SDL_VIDEO_DRIVER=dummy")
      endif()
      set_tests_properties(editor_n2_gate PROPERTIES TIMEOUT 900 ENVIRONMENT "${gate_env}")
      # The N3 gate: the storm over the edited valley, streamed from disk.
      add_test(NAME editor_n3_gate
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/n3_gate.py
                $<TARGET_FILE:giant_editor> --hidden --backend null)
      set_tests_properties(editor_n3_gate PROPERTIES TIMEOUT 1500 ENVIRONMENT "${gate_env}")
    endif()
  else()
    message(WARNING "Python 3 not found: the editor_mcp test is skipped")
  endif()
endif()

# The editor's commands without a window or GPU.
if(GIANT_BUILD_TESTS)
  add_executable(giant_editor_tests tests/test_scene_commands.cpp tests/test_kits.cpp tests/test_scene_cells.cpp
                 tests/test_world_streaming.cpp tests/test_terrain.cpp tests/test_far_ground.cpp tests/test_scatter.cpp tests/test_weather.cpp tests/test_physics.cpp tests/test_animation.cpp tests/test_cloth.cpp tests/test_swing.cpp tests/test_nav.cpp tests/test_walkers.cpp tests/test_audio.cpp tests/test_game_saves.cpp
                 src/scene_commands.cpp src/terrain_commands.cpp src/terrain_meshes.cpp src/far_ground.cpp src/terrain_brush_tool.cpp src/viewport_camera.cpp
                 src/scatter_placer.cpp src/scatter_commands.cpp src/weather_state.cpp src/weather_commands.cpp src/physics_state.cpp src/physics_commands.cpp src/play_state.cpp
                 src/mesh_library.cpp src/scene_render.cpp src/world_streaming.cpp src/frame_times.cpp
                 src/animation_state.cpp src/animation_commands.cpp src/hit_targets.cpp src/cloth_state.cpp
                 src/cloth_commands.cpp src/swing_commands.cpp src/swing_state.cpp src/nav_commands.cpp src/nav_state.cpp src/walker_commands.cpp src/walker_state.cpp src/audio_commands.cpp src/audio_state.cpp
                 src/game_saves.cpp src/save_commands.cpp)
  target_link_libraries(giant_editor_tests PRIVATE giant::core giant::platform giant::rhi giant::geometry
                        giant::render giant::world giant::sim giant::nav giant::audio giant::save giant::ai giant::test_main)
  target_include_directories(giant_editor_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/geometry/tests) # gltf_test_files.h
  giant_apply_warnings(giant_editor_tests)
  giant_apply_sanitizers(giant_editor_tests)
  add_test(NAME editor_commands COMMAND giant_editor_tests)
  set_tests_properties(editor_commands PROPERTIES TIMEOUT 300) # ~70 s under ThreadSanitizer
endif()
