giant_module(ui
  SOURCES
    src/canvas.cpp
    src/font.cpp
    src/hud.cpp
    src/screen.cpp
  DEPENDS core rhi
)
target_link_libraries(giant_ui PRIVATE third_party::stb_truetype nlohmann_json::nlohmann_json)

# The default font, built in (giant::ui::embedded::default_font()).
set(font_cpp ${CMAKE_CURRENT_BINARY_DIR}/default_font.cpp)
add_custom_command(
  OUTPUT ${font_cpp}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${GIANT_DEFAULT_FONT} -DOUTPUT=${font_cpp} -DNAME=default_font
          -DNAMESPACE=ui::embedded -P ${PROJECT_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS ${GIANT_DEFAULT_FONT} ${PROJECT_SOURCE_DIR}/cmake/embed_file.cmake
  VERBATIM)
target_sources(giant_ui PRIVATE ${font_cpp})

giant_module_tests(ui
  SOURCES
    tests/test_ui.cpp
    tests/test_screen.cpp
    tests/test_hud.cpp
)
