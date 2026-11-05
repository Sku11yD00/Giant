add_executable(giant_editor src/main.cpp)
target_link_libraries(giant_editor PRIVATE giant::core giant::platform giant::rhi)
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
endif()
