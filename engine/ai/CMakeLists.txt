giant_module(ai
  SOURCES
    src/commands.cpp
    src/json.cpp
    src/mcp_server.cpp
  DEPENDS core
)
# Commands and the MCP server speak JSON in their public headers.
target_link_libraries(giant_ai PUBLIC nlohmann_json::nlohmann_json)

giant_module_tests(ai
  SOURCES
    tests/test_commands.cpp
    tests/test_json.cpp
    tests/test_mcp_server.cpp
)
