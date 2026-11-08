giant_module(gameplay
  SOURCES
    src/items.cpp
    src/inventory.cpp
    src/weapons.cpp
  DEPENDS core
)
# JSON stays private: the gameplay API is plain C++ types.
target_link_libraries(giant_gameplay PRIVATE nlohmann_json::nlohmann_json)

giant_module_tests(gameplay
  SOURCES
    tests/test_items.cpp
    tests/test_inventory.cpp
    tests/test_weapons.cpp
)
if(GIANT_BUILD_TESTS)
  # The game's own data files must load.
  target_compile_definitions(giant_gameplay_tests PRIVATE
    GIANT_GAME_DATA_DIR="${PROJECT_SOURCE_DIR}/game/fold/data")
endif()
# Rolls must come out the same on every platform: no fused multiply-add
# (Clang contracts a*b+c by default where the CPU has FMA).
if(NOT MSVC)
  target_compile_options(giant_gameplay PRIVATE -ffp-contract=off)
endif()
