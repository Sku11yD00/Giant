add_executable(giant_editor src/main.cpp src/editor_commands.cpp)
target_link_libraries(giant_editor PRIVATE giant::core giant::platform giant::rhi giant::ai)
giant_apply_warnings(giant_editor)
giant_apply_sanitizers(giant_editor)

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
    set_tests_properties(editor_mcp PROPERTIES TIMEOUT 120)
    if(NOT WIN32)
      set_tests_properties(editor_mcp PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
  else()
    message(WARNING "Python 3 not found: the editor_mcp test is skipped")
  endif()
endif()
