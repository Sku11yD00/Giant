/**************************************************************************/
/*  giant_mcp_plugin.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "giant_mcp_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/editor_log.h"
#include "editor/settings/editor_settings.h"

static void _on_editor_log_message(const String &p_message, EditorLog::MessageType p_type) {
	String type;
	switch (p_type) {
		case EditorLog::MSG_TYPE_ERROR:
			type = "error";
			break;
		case EditorLog::MSG_TYPE_WARNING:
			type = "warning";
			break;
		case EditorLog::MSG_TYPE_EDITOR:
			type = "editor";
			break;
		default:
			type = "output";
			break;
	}
	GiantMCPTools::push_log(type, p_message);
}

void GiantMCPPlugin::_start_server() {
	// Environment variables override the editor settings, which helps automated tests.
	bool enabled = EDITOR_GET("giant_mcp/enabled");
	String env_port = OS::get_singleton()->get_environment("GIANT_MCP_PORT");
	String env_token = OS::get_singleton()->get_environment("GIANT_MCP_TOKEN");
	if (!env_port.is_empty()) {
		enabled = true;
	}
	if (!enabled) {
		return;
	}

	int port = env_port.is_empty() ? int(EDITOR_GET("giant_mcp/port")) : env_port.to_int();
	String token = env_token.is_empty() ? String(EDITOR_GET("giant_mcp/token")) : env_token;
	if (token.is_empty()) {
		uint8_t bytes[24];
		if (OS::get_singleton()->get_entropy(bytes, sizeof(bytes)) == OK) {
			token = String::hex_encode_buffer(bytes, sizeof(bytes));
			EditorSettings::get_singleton()->set_setting("giant_mcp/token", token);
			EditorSettings::get_singleton()->save();
		}
	}

	Error err = server.start(port, token, &tools);
	if (err != OK) {
		ERR_PRINT(vformat("Giant MCP: could not listen on port %d (error %d). Change Editor Settings > Giant MCP > Port.", port, err));
		return;
	}
	_write_connection_file(server.get_port(), token);
	print_line(vformat("Giant MCP server listening on http://127.0.0.1:%d%s", server.get_port(), GiantMCPServer::ENDPOINT_PATH));
	// Show the ready-made command so connecting Claude Code is a single copy and paste.
	print_line(vformat("To connect Claude Code, run: claude mcp add --transport http giant http://127.0.0.1:%d%s --header \"Authorization: Bearer %s\"", server.get_port(), GiantMCPServer::ENDPOINT_PATH, token));
}

void GiantMCPPlugin::_write_connection_file(int p_port, const String &p_token) {
	// .godot is not committed to version control, so the token stays on this PC.
	// AI tools read this file to connect without the user copying the token by hand.
	Dictionary info;
	info["url"] = vformat("http://127.0.0.1:%d%s", p_port, GiantMCPServer::ENDPOINT_PATH);
	info["token"] = p_token;
	String path = ProjectSettings::get_singleton()->get_project_data_path().path_join("giant_mcp.json");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(info, "  "));
	}
}

void GiantMCPPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_start_server();
			set_process(true);
		} break;
		case NOTIFICATION_PROCESS: {
			server.poll();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			server.stop();
		} break;
	}
}

GiantMCPPlugin::GiantMCPPlugin() {
	EDITOR_DEF("giant_mcp/enabled", true);
	EDITOR_DEF("giant_mcp/port", 7400);
	EDITOR_DEF("giant_mcp/token", "");
	EditorLog::message_hook = _on_editor_log_message;
}

GiantMCPPlugin::~GiantMCPPlugin() {
	if (EditorLog::message_hook == _on_editor_log_message) {
		EditorLog::message_hook = nullptr;
	}
}
