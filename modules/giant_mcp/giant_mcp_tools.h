/**************************************************************************/
/*  giant_mcp_tools.h                                                     */
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

#pragma once

#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class Node;
class Object;

// The tools an AI can call to drive the editor.
// Generic tools are driven by ClassDB reflection, so every engine class, property
// and method is reachable without writing a tool for it. Every scene change goes
// through the editor's undo history, so one Ctrl+Z reverts an AI action.
class GiantMCPTools {
public:
	typedef Dictionary (GiantMCPTools::*Handler)(const Dictionary &p_args);

	struct Tool {
		String name;
		String title;
		String description;
		Dictionary input_schema;
		bool read_only = false;
		bool destructive = false;
		Handler handler = nullptr;
	};

	struct LogEntry {
		uint64_t seq = 0;
		String type;
		String text;
	};

private:
	LocalVector<Tool> tools;

	static inline Mutex log_mutex;
	static inline LocalVector<LogEntry> log_entries;
	static inline uint64_t log_next_seq = 1;

	void _add_tool(const String &p_name, const String &p_title, const String &p_description, const String &p_schema_json, bool p_read_only, bool p_destructive, Handler p_handler);

	// Results.
	static Dictionary _text_result(const String &p_text, bool p_is_error = false);
	static Dictionary _json_result(const Variant &p_data);
	static Dictionary _error(const String &p_message) { return _text_result(p_message, true); }

	// Conversion between JSON and engine values.
	static Variant _to_json(const Variant &p_value, int p_depth = 0);
	static bool _from_json(const Variant &p_json, int p_target_type, Variant &r_value, String &r_error);
	static bool _convert(const Variant &p_value, int p_target_type, Variant &r_value, String &r_error);
	static bool _set_object_property(Object *p_object, const String &p_property, const Variant &p_json, Variant &r_value, String &r_error);

	// Scene helpers.
	static Node *_get_scene_root();
	static Node *_find_node(const String &p_path, String &r_error);
	static String _path_of(Node *p_node);
	static Dictionary _describe_node(Node *p_node, int p_depth, int p_max_depth);
	static bool _is_safe_res_path(const String &p_path, bool p_allow_user);

	// Tool handlers.
	Dictionary _engine_info(const Dictionary &p_args);
	Dictionary _get_scene_tree(const Dictionary &p_args);
	Dictionary _list_classes(const Dictionary &p_args);
	Dictionary _describe_class(const Dictionary &p_args);
	Dictionary _get_node(const Dictionary &p_args);
	Dictionary _set_properties(const Dictionary &p_args);
	Dictionary _create_node(const Dictionary &p_args);
	Dictionary _delete_node(const Dictionary &p_args);
	Dictionary _call_method(const Dictionary &p_args);
	Dictionary _new_scene(const Dictionary &p_args);
	Dictionary _open_scene(const Dictionary &p_args);
	Dictionary _save_scene(const Dictionary &p_args);
	Dictionary _list_files(const Dictionary &p_args);
	Dictionary _read_file(const Dictionary &p_args);
	Dictionary _write_file(const Dictionary &p_args);
	Dictionary _attach_script(const Dictionary &p_args);
	Dictionary _get_log(const Dictionary &p_args);
	Dictionary _play(const Dictionary &p_args);
	Dictionary _stop(const Dictionary &p_args);
	Dictionary _undo(const Dictionary &p_args);
	Dictionary _redo(const Dictionary &p_args);
	Dictionary _screenshot(const Dictionary &p_args);

public:
	static String get_instructions();
	static void push_log(const String &p_type, const String &p_text);

	Array list_tools() const;
	Dictionary call_tool(const String &p_name, const Dictionary &p_args, bool &r_found);

	GiantMCPTools();
};
