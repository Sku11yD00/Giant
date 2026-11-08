giant_module(save
  SOURCES
    src/slot.cpp
  DEPENDS core
)
# JSON stays private: the slot API is plain C++ types and text.
target_link_libraries(giant_save PRIVATE nlohmann_json::nlohmann_json)

giant_module_tests(save
  SOURCES
    tests/test_slot.cpp
)
