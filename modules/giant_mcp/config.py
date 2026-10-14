def can_build(env, platform):
    # The MCP server drives the editor, so it is only built into editor binaries.
    return env.editor_build


def configure(env):
    pass
