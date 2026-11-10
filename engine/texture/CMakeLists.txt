giant_module(texture
  SOURCES
    src/cook.cpp
    src/mips.cpp
    src/summary.cpp
  DEPENDS core rhi
)
# The decoder and encoders stay private: the API is rhi descriptions and bytes.
target_link_libraries(giant_texture PRIVATE third_party::stb_image third_party::bc7enc)

giant_module_tests(texture
  SOURCES
    tests/test_texture.cpp
)
