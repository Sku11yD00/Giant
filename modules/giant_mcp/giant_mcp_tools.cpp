/**************************************************************************/
/*  giant_mcp_tools.cpp                                                   */
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

#include "giant_mcp_tools.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/variant/variant_parser.h"
#include "core/version.h"
#include "editor/doc/doc_tools.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/main/viewport.h"
#include "scene/resources/packed_scene.h"

static constexpr int MAX_LOG_ENTRIES = 2000;
static constexpr int MAX_JSON_DEPTH = 5;
static constexpr int MAX_JSON_ITEMS = 256;

String GiantMCPTools::get_instructions() {
	return String(
			"You are connected to the Giant engine editor (a Godot fork). "
			"Node paths are relative to the open scene's root; use \".\" for the root itself. "
			"Values use JSON where possible: numbers, booleans and strings as-is, vectors and colours as arrays such as [1, 2, 3], "
			"or any Godot literal as a string such as \"Vector3(1, 2, 3)\" or \"Color(1, 0, 0)\". "
			"Resource properties accept a \"res://\" path to load, or an object such as {\"_class\": \"BoxMesh\", \"size\": [2, 2, 2]} to create one. "
			"Every scene change is recorded in the editor's undo history, and the undo tool reverts the last one. "
			"Use describe_class to learn any engine class before using it. "
			"After writing a script, read the result and call get_log to see compile errors. "
			"Use screenshot to check visual work.");
}

void GiantMCPTools::push_log(const String &p_type, const String &p_text) {
	MutexLock lock(log_mutex);
	LogEntry entry;
	entry.seq = log_next_seq++;
	entry.type = p_type;
	entry.text = p_text;
	log_entries.push_back(entry);
	if (log_entries.size() > MAX_LOG_ENTRIES) {
		log_entries.remove_at(0);
	}
}

void GiantMCPTools::_add_tool(const String &p_name, const String &p_title, const String &p_description, const String &p_schema_json, bool p_read_only, bool p_destructive, Handler p_handler) {
	Tool tool;
	tool.name = p_name;
	tool.title = p_title;
	tool.description = p_description;
	JSON json;
	Error err = json.parse(p_schema_json);
	ERR_FAIL_COND_MSG(err != OK, vformat("Invalid schema for MCP tool %s: %s", p_name, json.get_error_message()));
	tool.input_schema = json.get_data();
	tool.read_only = p_read_only;
	tool.destructive = p_destructive;
	tool.handler = p_handler;
	tools.push_back(tool);
}

Array GiantMCPTools::list_tools() const {
	// Tools are returned in a fixed order so clients can cache the list.
	Array result;
	for (const Tool &tool : tools) {
		Dictionary annotations;
		annotations["title"] = tool.title;
		annotations["readOnlyHint"] = tool.read_only;
		annotations["destructiveHint"] = tool.destructive;
		annotations["openWorldHint"] = false;
		Dictionary entry;
		entry["name"] = tool.name;
		entry["title"] = tool.title;
		entry["description"] = tool.description;
		entry["inputSchema"] = tool.input_schema;
		entry["annotations"] = annotations;
		result.push_back(entry);
	}
	return result;
}

Dictionary GiantMCPTools::call_tool(const String &p_name, const Dictionary &p_args, bool &r_found) {
	for (const Tool &tool : tools) {
		if (tool.name == p_name) {
			r_found = true;
			// Check required arguments before running the tool.
			Array required = tool.input_schema.get("required", Array());
			for (const Variant &key : required) {
				if (!p_args.has(key)) {
					return _error(vformat("Missing required argument \"%s\".", key));
				}
			}
			return (this->*tool.handler)(p_args);
		}
	}
	r_found = false;
	return Dictionary();
}

// Results.

Dictionary GiantMCPTools::_text_result(const String &p_text, bool p_is_error) {
	Dictionary content;
	content["type"] = "text";
	content["text"] = p_text;
	Array contents;
	contents.push_back(content);
	Dictionary result;
	result["content"] = contents;
	if (p_is_error) {
		result["isError"] = true;
	}
	return result;
}

Dictionary GiantMCPTools::_json_result(const Variant &p_data) {
	Dictionary result = _text_result(JSON::stringify(p_data, " ", false));
	if (p_data.get_type() == Variant::DICTIONARY) {
		result["structuredContent"] = p_data;
	}
	return result;
}

// Conversion between JSON and engine values.

Variant GiantMCPTools::_to_json(const Variant &p_value, int p_depth) {
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_value;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_value);
		case Variant::OBJECT: {
			Object *object = p_value.get_validated_object();
			if (!object) {
				return Variant();
			}
			if (Node *node = Object::cast_to<Node>(object)) {
				return "Node:" + _path_of(node);
			}
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				Dictionary d;
				d["_class"] = resource->get_class();
				if (!resource->get_path().is_empty() && !resource->get_path().contains("::")) {
					d["path"] = resource->get_path();
				}
				return d;
			}
			Dictionary d;
			d["_class"] = object->get_class();
			return d;
		}
		case Variant::DICTIONARY: {
			if (p_depth >= MAX_JSON_DEPTH) {
				return "{...}";
			}
			Dictionary in = p_value;
			Dictionary out;
			int count = 0;
			for (const KeyValue<Variant, Variant> &kv : in) {
				if (count++ >= MAX_JSON_ITEMS) {
					out["_truncated"] = in.size();
					break;
				}
				out[String(kv.key)] = _to_json(kv.value, p_depth + 1);
			}
			return out;
		}
		case Variant::ARRAY: {
			if (p_depth >= MAX_JSON_DEPTH) {
				return "[...]";
			}
			Array in = p_value;
			Array out;
			for (int i = 0; i < MIN(in.size(), MAX_JSON_ITEMS); i++) {
				out.push_back(_to_json(in[i], p_depth + 1));
			}
			if (in.size() > MAX_JSON_ITEMS) {
				out.push_back(vformat("... %d more", in.size() - MAX_JSON_ITEMS));
			}
			return out;
		}
		default: {
			// Vectors, colours, transforms and packed arrays use Godot's literal syntax, which the tools also accept as input.
			String text;
			VariantWriter::write_to_string(p_value, text, false);
			if (text.length() > 4096) {
				text = text.substr(0, 4096) + "...";
			}
			return text;
		}
	}
}

bool GiantMCPTools::_convert(const Variant &p_value, int p_target_type, Variant &r_value, String &r_error) {
	Variant::Type target = Variant::Type(p_target_type);
	if (target == Variant::NIL || p_value.get_type() == target) {
		r_value = p_value;
		return true;
	}
	Callable::CallError call_error;
	const Variant *args[1] = { &p_value };
	Variant::construct(target, r_value, args, 1, call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		r_error = vformat("Cannot convert %s to %s.", Variant::get_type_name(p_value.get_type()), Variant::get_type_name(target));
		return false;
	}
	return true;
}

bool GiantMCPTools::_from_json(const Variant &p_json, int p_target_type, Variant &r_value, String &r_error) {
	Variant::Type target = Variant::Type(p_target_type);

	if (p_json.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_json;
		// {"_class": "BoxMesh", ...} creates a new resource and sets its properties.
		if (d.has("_class")) {
			String class_name = d["_class"];
			if (!ClassDB::class_exists(class_name) || !ClassDB::can_instantiate(class_name) || !ClassDB::is_parent_class(class_name, "Resource")) {
				r_error = vformat("\"%s\" is not a resource class that can be created.", class_name);
				return false;
			}
			Ref<Resource> resource = Object::cast_to<Resource>(ClassDB::instantiate(class_name));
			for (const KeyValue<Variant, Variant> &kv : d) {
				if (String(kv.key) == "_class") {
					continue;
				}
				Variant value;
				if (!_set_object_property(resource.ptr(), kv.key, kv.value, value, r_error)) {
					return false;
				}
				resource->set(StringName(String(kv.key)), value);
			}
			r_value = resource;
			return true;
		}
		// {"x": 1, "y": 2, "z": 3} for vectors and {"r": ..} for colours.
		if (target != Variant::DICTIONARY && target != Variant::NIL && target != Variant::OBJECT) {
			Array components;
			const char *keys[2][4] = { { "x", "y", "z", "w" }, { "r", "g", "b", "a" } };
			for (int set = 0; set < 2 && components.is_empty(); set++) {
				for (int i = 0; i < 4; i++) {
					if (d.has(keys[set][i])) {
						components.push_back(d[keys[set][i]]);
					}
				}
			}
			if (!components.is_empty()) {
				return _from_json(components, p_target_type, r_value, r_error);
			}
		}
		Dictionary out;
		for (const KeyValue<Variant, Variant> &kv : d) {
			Variant value;
			if (!_from_json(kv.value, Variant::NIL, value, r_error)) {
				return false;
			}
			out[kv.key] = value;
		}
		return _convert(out, p_target_type, r_value, r_error);
	}

	if (p_json.get_type() == Variant::ARRAY) {
		Array in = p_json;
		if (target == Variant::NIL || target == Variant::ARRAY || (target >= Variant::PACKED_BYTE_ARRAY && target <= Variant::PACKED_VECTOR4_ARRAY)) {
			Array out;
			for (int i = 0; i < in.size(); i++) {
				Variant value;
				if (!_from_json(in[i], Variant::NIL, value, r_error)) {
					return false;
				}
				out.push_back(value);
			}
			return _convert(out, p_target_type, r_value, r_error);
		}
		// [1, 2, 3] builds a Vector3, [1, 0, 0, 1] a Color, and so on.
		Vector<Variant> values;
		Vector<const Variant *> args;
		values.resize(in.size());
		for (int i = 0; i < in.size(); i++) {
			values.write[i] = in[i];
		}
		for (int i = 0; i < values.size(); i++) {
			args.push_back(&values[i]);
		}
		Callable::CallError call_error;
		Variant::construct(target, r_value, args.ptr(), args.size(), call_error);
		if (call_error.error != Callable::CallError::CALL_OK) {
			r_error = vformat("Cannot build %s from an array of %d values.", Variant::get_type_name(target), in.size());
			return false;
		}
		return true;
	}

	if (p_json.get_type() == Variant::STRING) {
		String text = p_json;
		switch (target) {
			case Variant::STRING:
				r_value = text;
				return true;
			case Variant::STRING_NAME:
				r_value = StringName(text);
				return true;
			case Variant::NODE_PATH:
				r_value = NodePath(text);
				return true;
			default:
				break;
		}
		if (text.begins_with("res://") || text.begins_with("uid://")) {
			Ref<Resource> resource = ResourceLoader::load(text);
			if (resource.is_null()) {
				r_error = "Could not load resource " + text + ".";
				return false;
			}
			r_value = resource;
			return true;
		}
		// Godot literal syntax, such as "Vector3(1, 2, 3)".
		VariantParser::StreamString stream;
		stream.s = text;
		Variant parsed;
		String parse_error;
		int line = 0;
		if (VariantParser::parse(&stream, parsed, parse_error, line) == OK) {
			return _convert(parsed, p_target_type, r_value, r_error);
		}
		if (target == Variant::NIL) {
			r_value = text;
			return true;
		}
		r_error = vformat("Could not read \"%s\" as %s: %s", text, Variant::get_type_name(target), parse_error);
		return false;
	}

	return _convert(p_json, p_target_type, r_value, r_error);
}

bool GiantMCPTools::_set_object_property(Object *p_object, const String &p_property, const Variant &p_json, Variant &r_value, String &r_error) {
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (info.name == p_property) {
			return _from_json(p_json, info.type, r_value, r_error);
		}
	}
	bool valid = false;
	p_object->get(StringName(p_property), &valid);
	if (!valid) {
		r_error = vformat("%s has no property \"%s\". Use describe_class to list its properties.", p_object->get_class(), p_property);
		return false;
	}
	return _from_json(p_json, Variant::NIL, r_value, r_error);
}

// Scene helpers.

Node *GiantMCPTools::_get_scene_root() {
	return EditorInterface::get_singleton()->get_edited_scene_root();
}

Node *GiantMCPTools::_find_node(const String &p_path, String &r_error) {
	Node *root = _get_scene_root();
	if (!root) {
		r_error = "No scene is open. Use new_scene or open_scene first.";
		return nullptr;
	}
	String path = p_path.strip_edges();
	if (path.is_empty() || path == "." || path == String(root->get_name())) {
		return root;
	}
	Node *node = root->get_node_or_null(NodePath(path));
	if (!node && path.begins_with(String(root->get_name()) + "/")) {
		node = root->get_node_or_null(NodePath(path.substr(String(root->get_name()).length() + 1)));
	}
	if (!node) {
		r_error = vformat("No node at \"%s\". Use get_scene_tree to see the paths.", p_path);
	}
	return node;
}

String GiantMCPTools::_path_of(Node *p_node) {
	Node *root = _get_scene_root();
	if (!root || !p_node) {
		return String();
	}
	if (p_node == root) {
		return ".";
	}
	if (!root->is_ancestor_of(p_node)) {
		return String(p_node->get_path());
	}
	return String(root->get_path_to(p_node));
}

Dictionary GiantMCPTools::_describe_node(Node *p_node, int p_depth, int p_max_depth) {
	Dictionary d;
	d["name"] = p_node->get_name();
	d["type"] = p_node->get_class();
	d["path"] = _path_of(p_node);
	Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		d["script"] = script->get_path();
	}
	int child_count = p_node->get_child_count();
	if (child_count > 0) {
		if (p_depth < p_max_depth) {
			Array children;
			for (int i = 0; i < MIN(child_count, MAX_JSON_ITEMS); i++) {
				children.push_back(_describe_node(p_node->get_child(i), p_depth + 1, p_max_depth));
			}
			d["children"] = children;
		} else {
			d["child_count"] = child_count;
		}
	}
	return d;
}

bool GiantMCPTools::_is_safe_res_path(const String &p_path, bool p_allow_user) {
	bool scheme_ok = p_path.begins_with("res://") || (p_allow_user && p_path.begins_with("user://"));
	return scheme_ok && !p_path.contains("..") && !p_path.begins_with("res://.godot");
}

// Tool handlers.

Dictionary GiantMCPTools::_engine_info(const Dictionary &p_args) {
	Dictionary info;
	info["engine"] = "Giant engine (Godot fork)";
	info["version"] = GODOT_VERSION_FULL_BUILD;
#ifdef REAL_T_IS_DOUBLE
	info["precision"] = "double";
#else
	info["precision"] = "single";
#endif
	info["project_name"] = GLOBAL_GET("application/config/name");
	info["project_path"] = ProjectSettings::get_singleton()->globalize_path("res://");
	info["main_scene"] = GLOBAL_GET("application/run/main_scene");
	info["rendering_method"] = GLOBAL_GET("rendering/renderer/rendering_method");
	info["msaa_3d"] = GLOBAL_GET("rendering/anti_aliasing/quality/msaa_3d");
	info["screen_space_aa"] = GLOBAL_GET("rendering/anti_aliasing/quality/screen_space_aa");
	info["taa"] = GLOBAL_GET("rendering/anti_aliasing/quality/use_taa");
	Node *root = _get_scene_root();
	info["edited_scene"] = root ? Variant(root->get_scene_file_path()) : Variant();
	info["open_scenes"] = EditorInterface::get_singleton()->get_open_scenes();
	info["playing"] = EditorInterface::get_singleton()->is_playing_scene();
	return _json_result(info);
}

Dictionary GiantMCPTools::_get_scene_tree(const Dictionary &p_args) {
	String error;
	Node *node = _find_node(p_args.get("root_path", "."), error);
	if (!node) {
		return _error(error);
	}
	int max_depth = p_args.get("max_depth", 8);
	Dictionary result;
	result["scene_path"] = _get_scene_root()->get_scene_file_path();
	result["tree"] = _describe_node(node, 0, max_depth);
	return _json_result(result);
}

Dictionary GiantMCPTools::_list_classes(const Dictionary &p_args) {
	String filter = String(p_args.get("filter", "")).to_lower();
	String inherits = p_args.get("inherits", "");
	if (!inherits.is_empty() && !ClassDB::class_exists(inherits)) {
		return _error(vformat("Unknown class \"%s\".", inherits));
	}
	LocalVector<StringName> classes;
	ClassDB::get_class_list(classes);
	PackedStringArray names;
	for (const StringName &class_name : classes) {
		String name = class_name;
		if (name.begins_with("_") || !ClassDB::is_class_exposed(class_name)) {
			continue;
		}
		if (!filter.is_empty() && !name.to_lower().contains(filter)) {
			continue;
		}
		if (!inherits.is_empty() && !ClassDB::is_parent_class(class_name, inherits)) {
			continue;
		}
		names.push_back(name);
	}
	// Classes declared with class_name in scripts.
	LocalVector<StringName> global_classes;
	ScriptServer::get_global_class_list(global_classes);
	for (const StringName &class_name : global_classes) {
		String name = class_name;
		if (!filter.is_empty() && !name.to_lower().contains(filter)) {
			continue;
		}
		if (!inherits.is_empty() && !ClassDB::is_parent_class(ScriptServer::get_global_class_native_base(name), inherits)) {
			continue;
		}
		names.push_back(name + " (script)");
	}
	names.sort();
	int total = names.size();
	if (names.size() > 500) {
		names.resize(500);
	}
	Dictionary result;
	result["classes"] = names;
	result["total"] = total;
	return _json_result(result);
}

Dictionary GiantMCPTools::_describe_class(const Dictionary &p_args) {
	String class_name = p_args["class_name"];
	if (!ClassDB::class_exists(class_name)) {
		return _error(vformat("Unknown class \"%s\". Use list_classes to search.", class_name));
	}
	bool inherited = p_args.get("include_inherited", false);
	Dictionary result;
	result["class"] = class_name;

	PackedStringArray chain;
	for (StringName parent = ClassDB::get_parent_class(class_name); parent != StringName(); parent = ClassDB::get_parent_class(parent)) {
		chain.push_back(parent);
	}
	result["inherits"] = chain;
	result["can_create"] = ClassDB::can_instantiate(class_name);

	DocTools *docs = EditorHelp::get_doc_data();
	if (docs && docs->class_list.has(class_name)) {
		result["description"] = docs->class_list[class_name].brief_description.strip_edges();
	}

	Array properties;
	List<PropertyInfo> property_list;
	ClassDB::get_property_list(class_name, &property_list, !inherited);
	for (const PropertyInfo &info : property_list) {
		if (!(info.usage & PROPERTY_USAGE_EDITOR) || (info.usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP))) {
			continue;
		}
		Dictionary p;
		p["name"] = info.name;
		p["type"] = info.class_name != StringName() ? String(info.class_name) : Variant::get_type_name(info.type);
		if (info.hint == PROPERTY_HINT_ENUM || info.hint == PROPERTY_HINT_RANGE || info.hint == PROPERTY_HINT_FLAGS) {
			p["hint"] = info.hint_string;
		}
		properties.push_back(p);
	}
	result["properties"] = properties;

	Array methods;
	List<MethodInfo> method_list;
	ClassDB::get_method_list(class_name, &method_list, !inherited);
	for (const MethodInfo &info : method_list) {
		if (String(info.name).begins_with("_")) {
			continue;
		}
		PackedStringArray args;
		for (const PropertyInfo &arg : info.arguments) {
			args.push_back(arg.name + ": " + (arg.class_name != StringName() ? String(arg.class_name) : Variant::get_type_name(arg.type)));
		}
		String return_type = info.return_val.class_name != StringName() ? String(info.return_val.class_name) : Variant::get_type_name(info.return_val.type);
		methods.push_back(vformat("%s(%s) -> %s", info.name, String(", ").join(args), return_type));
	}
	result["methods"] = methods;

	Array signals;
	List<MethodInfo> signal_list;
	ClassDB::get_signal_list(class_name, &signal_list, !inherited);
	for (const MethodInfo &info : signal_list) {
		signals.push_back(String(info.name));
	}
	result["signals"] = signals;
	return _json_result(result);
}

Dictionary GiantMCPTools::_get_node(const Dictionary &p_args) {
	String error;
	Node *node = _find_node(p_args["node_path"], error);
	if (!node) {
		return _error(error);
	}
	String filter = String(p_args.get("filter", "")).to_lower();
	Dictionary result = _describe_node(node, 0, 0);
	Dictionary values;
	List<PropertyInfo> properties;
	node->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (!(info.usage & PROPERTY_USAGE_EDITOR) || (info.usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP))) {
			continue;
		}
		if (!filter.is_empty() && !info.name.to_lower().contains(filter)) {
			continue;
		}
		values[info.name] = _to_json(node->get(info.name));
	}
	result["properties"] = values;
	return _json_result(result);
}

Dictionary GiantMCPTools::_set_properties(const Dictionary &p_args) {
	String error;
	Node *node = _find_node(p_args["node_path"], error);
	if (!node) {
		return _error(error);
	}
	Dictionary properties = p_args["properties"];
	if (properties.is_empty()) {
		return _error("No properties given.");
	}
	// Convert everything first so a bad value changes nothing.
	Vector<Pair<StringName, Variant>> changes;
	for (const KeyValue<Variant, Variant> &kv : properties) {
		Variant value;
		if (!_set_object_property(node, kv.key, kv.value, value, error)) {
			return _error(error);
		}
		changes.push_back(Pair<StringName, Variant>(StringName(String(kv.key)), value));
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat("AI: Set %d properties on %s", changes.size(), node->get_name()), UndoRedo::MERGE_DISABLE, _get_scene_root());
	for (const Pair<StringName, Variant> &change : changes) {
		undo_redo->add_do_property(node, change.first, change.second);
		undo_redo->add_undo_property(node, change.first, node->get(change.first));
	}
	undo_redo->commit_action();

	Dictionary applied;
	for (const Pair<StringName, Variant> &change : changes) {
		applied[String(change.first)] = _to_json(node->get(change.first));
	}
	Dictionary result;
	result["node_path"] = _path_of(node);
	result["properties"] = applied;
	return _json_result(result);
}

Dictionary GiantMCPTools::_create_node(const Dictionary &p_args) {
	Node *root = _get_scene_root();
	if (!root) {
		return _error("No scene is open. Use new_scene or open_scene first.");
	}
	String error;
	Node *parent = _find_node(p_args.get("parent_path", "."), error);
	if (!parent) {
		return _error(error);
	}

	String type = p_args["type"];
	Node *node = nullptr;
	if (ScriptServer::is_global_class(type)) {
		StringName base = ScriptServer::get_global_class_native_base(type);
		Ref<Script> script = ResourceLoader::load(ScriptServer::get_global_class_path(type));
		node = Object::cast_to<Node>(ClassDB::instantiate(base));
		if (node && script.is_valid()) {
			node->set_script(script);
		}
	} else if (ClassDB::class_exists(type) && ClassDB::can_instantiate(type) && ClassDB::is_parent_class(type, "Node")) {
		node = Object::cast_to<Node>(ClassDB::instantiate(type));
	}
	if (!node) {
		return _error(vformat("\"%s\" is not a node type that can be created. Use list_classes with inherits \"Node\".", type));
	}
	node->set_name(p_args.get("name", type));

	Dictionary properties = p_args.get("properties", Dictionary());
	for (const KeyValue<Variant, Variant> &kv : properties) {
		Variant value;
		if (!_set_object_property(node, kv.key, kv.value, value, error)) {
			memdelete(node);
			return _error(error);
		}
		node->set(StringName(String(kv.key)), value);
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat("AI: Create %s", node->get_name()), UndoRedo::MERGE_DISABLE, root);
	undo_redo->add_do_method(parent, "add_child", node, true);
	undo_redo->add_do_method(node, "set_owner", root);
	undo_redo->add_do_reference(node);
	undo_redo->add_undo_method(parent, "remove_child", node);
	undo_redo->commit_action();

	return _json_result(_describe_node(node, 0, 0));
}

Dictionary GiantMCPTools::_delete_node(const Dictionary &p_args) {
	Node *root = _get_scene_root();
	String error;
	Node *node = _find_node(p_args["node_path"], error);
	if (!node) {
		return _error(error);
	}
	if (node == root) {
		return _error("The scene root cannot be deleted. Create a new scene instead.");
	}
	Node *parent = node->get_parent();
	String path = _path_of(node);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat("AI: Delete %s", node->get_name()), UndoRedo::MERGE_DISABLE, root);
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node, true);
	undo_redo->add_undo_method(parent, "move_child", node, node->get_index());
	// Restore ownership so the node and its children are saved with the scene again.
	List<Node *> owned;
	owned.push_back(node);
	for (List<Node *>::Element *E = owned.front(); E; E = E->next()) {
		Node *current = E->get();
		if (current->get_owner() == root) {
			undo_redo->add_undo_method(current, "set_owner", root);
		}
		for (int i = 0; i < current->get_child_count(); i++) {
			owned.push_back(current->get_child(i));
		}
	}
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	return _text_result("Deleted " + path + ". Use undo to bring it back.");
}

Dictionary GiantMCPTools::_call_method(const Dictionary &p_args) {
	String error;
	Node *node = _find_node(p_args["node_path"], error);
	if (!node) {
		return _error(error);
	}
	StringName method = String(p_args["method"]);
	if (!node->has_method(method)) {
		return _error(vformat("%s has no method \"%s\".", node->get_class(), method));
	}
	Array json_args = p_args.get("args", Array());

	// Use the method's declared argument types when the class reports them.
	MethodInfo info;
	bool has_info = ClassDB::get_method_info(node->get_class_name(), method, &info);
	Vector<Variant> values;
	for (int i = 0; i < json_args.size(); i++) {
		int target = (has_info && i < info.arguments.size()) ? int(info.arguments[i].type) : int(Variant::NIL);
		Variant value;
		if (!_from_json(json_args[i], target, value, error)) {
			return _error(vformat("Argument %d: %s", i + 1, error));
		}
		values.push_back(value);
	}
	Vector<const Variant *> args;
	for (int i = 0; i < values.size(); i++) {
		args.push_back(&values[i]);
	}
	Callable::CallError call_error;
	Variant ret = node->callp(method, args.ptr(), args.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		return _error(Variant::get_callable_error_text(Callable(node, method), args.ptr(), args.size(), call_error));
	}
	Dictionary result;
	result["return"] = _to_json(ret);
	result["note"] = "Method calls are not recorded in the undo history.";
	return _json_result(result);
}

Dictionary GiantMCPTools::_new_scene(const Dictionary &p_args) {
	String path = p_args["path"];
	if (!_is_safe_res_path(path, false) || !(path.ends_with(".tscn") || path.ends_with(".scn"))) {
		return _error("The path must be inside res:// and end with .tscn.");
	}
	if (FileAccess::exists(path) && !bool(p_args.get("overwrite", false))) {
		return _error(path + " already exists. Pass overwrite: true to replace it, or use open_scene.");
	}
	String type = p_args.get("root_type", "Node3D");
	if (!ClassDB::class_exists(type) || !ClassDB::can_instantiate(type) || !ClassDB::is_parent_class(type, "Node")) {
		return _error(vformat("\"%s\" is not a node type that can be created.", type));
	}
	Node *root = Object::cast_to<Node>(ClassDB::instantiate(type));
	root->set_name(p_args.get("root_name", path.get_file().get_basename().to_pascal_case()));

	Ref<PackedScene> scene;
	scene.instantiate();
	Error err = scene->pack(root);
	memdelete(root);
	if (err != OK) {
		return _error("Could not pack the new scene.");
	}
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	err = ResourceSaver::save(scene, path);
	if (err != OK) {
		return _error(vformat("Could not save %s (error %d).", path, err));
	}
	EditorFileSystem::get_singleton()->update_file(path);
	EditorInterface::get_singleton()->open_scene_from_path(path);
	return _text_result("Created and opened " + path + ".");
}

Dictionary GiantMCPTools::_open_scene(const Dictionary &p_args) {
	String path = p_args["path"];
	if (!_is_safe_res_path(path, false) || !FileAccess::exists(path)) {
		return _error("No scene at " + path + ".");
	}
	EditorInterface::get_singleton()->open_scene_from_path(path);
	Node *root = _get_scene_root();
	if (!root || root->get_scene_file_path() != path) {
		return _error("Could not open " + path + ". Call get_log for details.");
	}
	return _text_result("Opened " + path + ".");
}

Dictionary GiantMCPTools::_save_scene(const Dictionary &p_args) {
	Node *root = _get_scene_root();
	if (!root) {
		return _error("No scene is open.");
	}
	String path = p_args.get("path", "");
	if (!path.is_empty()) {
		if (!_is_safe_res_path(path, false) || !path.ends_with(".tscn")) {
			return _error("The path must be inside res:// and end with .tscn.");
		}
		EditorInterface::get_singleton()->save_scene_as(path);
		return _text_result("Saved the scene as " + path + ".");
	}
	if (root->get_scene_file_path().is_empty()) {
		return _error("This scene has never been saved. Pass a path.");
	}
	Error err = EditorInterface::get_singleton()->save_scene();
	if (err != OK) {
		return _error(vformat("Saving failed (error %d).", err));
	}
	return _text_result("Saved " + root->get_scene_file_path() + ".");
}

Dictionary GiantMCPTools::_list_files(const Dictionary &p_args) {
	String dir = p_args.get("dir", "res://");
	if (!_is_safe_res_path(dir, true)) {
		return _error("The folder must be inside res:// or user://.");
	}
	bool recursive = p_args.get("recursive", false);
	PackedStringArray files;
	List<String> pending;
	pending.push_back(dir);
	while (!pending.is_empty() && files.size() < 2000) {
		String current = pending.front()->get();
		pending.pop_front();
		Ref<DirAccess> da = DirAccess::open(current);
		if (da.is_null()) {
			continue;
		}
		da->list_dir_begin();
		for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
			if (name.begins_with(".")) {
				continue;
			}
			String full = current.path_join(name);
			if (da->current_is_dir()) {
				files.push_back(full + "/");
				if (recursive) {
					pending.push_back(full);
				}
			} else if (!name.ends_with(".import") && !name.ends_with(".uid")) {
				files.push_back(full);
			}
		}
		da->list_dir_end();
	}
	files.sort();
	Dictionary result;
	result["files"] = files;
	return _json_result(result);
}

Dictionary GiantMCPTools::_read_file(const Dictionary &p_args) {
	String path = p_args["path"];
	if (!_is_safe_res_path(path, true) && path != "res://project.godot") {
		return _error("The file must be inside res:// or user://.");
	}
	Error err = OK;
	String text = FileAccess::get_file_as_string(path, &err);
	if (err != OK) {
		return _error(vformat("Could not read %s (error %d).", path, err));
	}
	return _text_result(text);
}

Dictionary GiantMCPTools::_write_file(const Dictionary &p_args) {
	String path = p_args["path"];
	if (!_is_safe_res_path(path, false)) {
		return _error("The file must be inside res:// and outside .godot.");
	}
	bool existed = FileAccess::exists(path);
	if (existed && !bool(p_args.get("overwrite", true))) {
		return _error(path + " already exists.");
	}
	uint64_t log_start;
	{
		MutexLock lock(log_mutex);
		log_start = log_next_seq;
	}

	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	String content = p_args["content"];
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			return _error("Could not open " + path + " for writing.");
		}
		file->store_string(content);
	}
	EditorFileSystem::get_singleton()->update_file(path);

	Dictionary result;
	result["path"] = path;
	result["created"] = !existed;

	// Recompile scripts right away so errors come back with this call.
	String extension = path.get_extension().to_lower();
	if (extension == "gd" || extension == "gdshader") {
		Ref<Resource> resource = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REPLACE);
		Ref<Script> script = resource;
		if (script.is_valid()) {
			script->set_source_code(content);
			Error err = script->reload(true);
			result["compiled"] = err == OK;
		}
		PackedStringArray errors;
		MutexLock lock(log_mutex);
		for (const LogEntry &entry : log_entries) {
			if (entry.seq >= log_start && (entry.type == "error" || entry.type == "warning")) {
				errors.push_back(entry.text);
			}
		}
		result["errors"] = errors;
	} else if (extension == "cs") {
		result["note"] = "C# scripts compile when the project is built. Use play, then get_log, to see build errors.";
	}
	return _json_result(result);
}

Dictionary GiantMCPTools::_attach_script(const Dictionary &p_args) {
	String error;
	Node *node = _find_node(p_args["node_path"], error);
	if (!node) {
		return _error(error);
	}
	String path = p_args["script_path"];
	Ref<Script> script = ResourceLoader::load(path);
	if (script.is_null()) {
		return _error("Could not load a script from " + path + ".");
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat("AI: Attach script to %s", node->get_name()), UndoRedo::MERGE_DISABLE, _get_scene_root());
	undo_redo->add_do_property(node, "script", script);
	undo_redo->add_undo_property(node, "script", node->get_script());
	undo_redo->commit_action();
	return _text_result("Attached " + path + " to " + _path_of(node) + ".");
}

Dictionary GiantMCPTools::_get_log(const Dictionary &p_args) {
	uint64_t since = int64_t(p_args.get("since", 0));
	int max = CLAMP(int(p_args.get("max", 100)), 1, 1000);
	bool errors_only = p_args.get("errors_only", false);
	Array entries;
	uint64_t next;
	{
		MutexLock lock(log_mutex);
		next = log_next_seq;
		for (int64_t i = int64_t(log_entries.size()) - 1; i >= 0 && entries.size() < max; i--) {
			const LogEntry &entry = log_entries[i];
			if (entry.seq <= since) {
				break;
			}
			if (errors_only && entry.type != "error" && entry.type != "warning") {
				continue;
			}
			Dictionary d;
			d["seq"] = entry.seq;
			d["type"] = entry.type;
			d["text"] = entry.text;
			entries.push_front(d);
		}
	}
	Dictionary result;
	result["entries"] = entries;
	result["next_since"] = next - 1;
	return _json_result(result);
}

Dictionary GiantMCPTools::_play(const Dictionary &p_args) {
	String which = p_args.get("scene", "current");
	if (which == "main") {
		if (String(GLOBAL_GET("application/run/main_scene")).is_empty()) {
			return _error("The project has no main scene. Set application/run/main_scene or play the current scene.");
		}
		EditorInterface::get_singleton()->play_main_scene();
	} else {
		if (!_get_scene_root()) {
			return _error("No scene is open.");
		}
		EditorInterface::get_singleton()->play_current_scene();
	}
	return _text_result("The game is starting. Its output appears in get_log; call stop when done.");
}

Dictionary GiantMCPTools::_stop(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()->is_playing_scene()) {
		return _text_result("Nothing is playing.");
	}
	EditorInterface::get_singleton()->stop_playing_scene();
	return _text_result("Stopped the game.");
}

Dictionary GiantMCPTools::_undo(const Dictionary &p_args) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *root = _get_scene_root();
	int history = root ? undo_redo->get_history_id_for_object(root) : EditorUndoRedoManager::GLOBAL_HISTORY;
	String action = undo_redo->get_history_undo_redo(history)->get_current_action_name();
	if (!undo_redo->undo_history(history)) {
		return _error("Nothing to undo.");
	}
	return _text_result("Undid: " + action);
}

Dictionary GiantMCPTools::_redo(const Dictionary &p_args) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *root = _get_scene_root();
	int history = root ? undo_redo->get_history_id_for_object(root) : EditorUndoRedoManager::GLOBAL_HISTORY;
	if (!undo_redo->redo_history(history)) {
		return _error("Nothing to redo.");
	}
	return _text_result("Redid: " + undo_redo->get_history_undo_redo(history)->get_current_action_name());
}

Dictionary GiantMCPTools::_screenshot(const Dictionary &p_args) {
	String view = p_args.get("view", "3d");
	Viewport *viewport = nullptr;
	if (view == "3d") {
		viewport = EditorInterface::get_singleton()->get_editor_viewport_3d(p_args.get("index", 0));
	} else if (view == "2d") {
		viewport = EditorInterface::get_singleton()->get_editor_viewport_2d();
	} else if (view == "editor") {
		viewport = EditorInterface::get_singleton()->get_base_control()->get_viewport();
	} else {
		return _error("view must be \"3d\", \"2d\" or \"editor\".");
	}
	if (!viewport || viewport->get_texture().is_null()) {
		return _error("That view is not available.");
	}
	Ref<Image> image = viewport->get_texture()->get_image();
	if (image.is_null() || image->is_empty()) {
		return _error("The view returned no image. The editor may be running without a GPU (headless).");
	}
	int max_width = p_args.get("max_width", 1280);
	if (max_width > 0 && image->get_width() > max_width) {
		image->resize(max_width, MAX(1, image->get_height() * max_width / image->get_width()), Image::INTERPOLATE_BILINEAR);
	}
	Vector<uint8_t> png = image->save_png_to_buffer();

	Dictionary content;
	content["type"] = "image";
	content["mimeType"] = "image/png";
	content["data"] = CryptoCore::b64_encode_str(png.ptr(), png.size());
	Array contents;
	contents.push_back(content);
	Dictionary result;
	result["content"] = contents;
	return result;
}

GiantMCPTools::GiantMCPTools() {
	// Schemas are JSON Schema objects. Tools are listed in this order.
	_add_tool("engine_info", "Engine info",
			"Engine version, project, open scenes, renderer and anti-aliasing settings.",
			R"({"type":"object","properties":{}})",
			true, false, &GiantMCPTools::_engine_info);
	_add_tool("get_scene_tree", "Get scene tree",
			"The open scene's node tree: names, types, paths and scripts.",
			R"({"type":"object","properties":{"root_path":{"type":"string","description":"Start from this node. Default is the scene root."},"max_depth":{"type":"integer","description":"How many levels to include. Default 8."}}})",
			true, false, &GiantMCPTools::_get_scene_tree);
	_add_tool("list_classes", "List classes",
			"Search the engine's classes (and script classes) by name or base class.",
			R"({"type":"object","properties":{"filter":{"type":"string","description":"Case-insensitive part of the name."},"inherits":{"type":"string","description":"Only classes that inherit this one, such as Node3D or Resource."}}})",
			true, false, &GiantMCPTools::_list_classes);
	_add_tool("describe_class", "Describe class",
			"A class's description, base classes, editable properties, methods and signals, from the engine's reflection data.",
			R"({"type":"object","properties":{"class_name":{"type":"string"},"include_inherited":{"type":"boolean","description":"Also list members from base classes. Default false."}},"required":["class_name"]})",
			true, false, &GiantMCPTools::_describe_class);
	_add_tool("get_node", "Get node",
			"A node's type, script and current property values.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"filter":{"type":"string","description":"Only properties whose name contains this."}},"required":["node_path"]})",
			true, false, &GiantMCPTools::_get_node);
	_add_tool("set_properties", "Set properties",
			"Set one or more properties on a node as a single undoable action.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"properties":{"type":"object","description":"Property names to values, e.g. {\"position\": [0, 1, 0], \"mesh\": {\"_class\": \"SphereMesh\"}}."}},"required":["node_path","properties"]})",
			false, false, &GiantMCPTools::_set_properties);
	_add_tool("create_node", "Create node",
			"Add a node of any engine or script class to the open scene, with optional starting properties. Undoable.",
			R"({"type":"object","properties":{"type":{"type":"string","description":"Class name, such as MeshInstance3D, GPUParticles3D or a script class_name."},"parent_path":{"type":"string","description":"Default is the scene root."},"name":{"type":"string"},"properties":{"type":"object"}},"required":["type"]})",
			false, false, &GiantMCPTools::_create_node);
	_add_tool("delete_node", "Delete node",
			"Remove a node and its children from the open scene. Undoable.",
			R"({"type":"object","properties":{"node_path":{"type":"string"}},"required":["node_path"]})",
			false, true, &GiantMCPTools::_delete_node);
	_add_tool("call_method", "Call method",
			"Call any method on a node and return the result. Not undoable, so prefer set_properties for edits.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"method":{"type":"string"},"args":{"type":"array"}},"required":["node_path","method"]})",
			false, true, &GiantMCPTools::_call_method);
	_add_tool("new_scene", "New scene",
			"Create a scene file with a root node, save it and open it.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path ending in .tscn."},"root_type":{"type":"string","description":"Default Node3D."},"root_name":{"type":"string"},"overwrite":{"type":"boolean"}},"required":["path"]})",
			false, false, &GiantMCPTools::_new_scene);
	_add_tool("open_scene", "Open scene",
			"Open a scene file in the editor.",
			R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
			false, false, &GiantMCPTools::_open_scene);
	_add_tool("save_scene", "Save scene",
			"Save the open scene, or save it under a new path.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"Optional new res:// path."}}})",
			false, false, &GiantMCPTools::_save_scene);
	_add_tool("list_files", "List files",
			"List project files and folders.",
			R"({"type":"object","properties":{"dir":{"type":"string","description":"Default res://"},"recursive":{"type":"boolean"}}})",
			true, false, &GiantMCPTools::_list_files);
	_add_tool("read_file", "Read file",
			"Read a text file from the project, such as a script, scene or shader.",
			R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
			true, false, &GiantMCPTools::_read_file);
	_add_tool("write_file", "Write file",
			"Create or replace a text file in the project. GDScript and shaders are recompiled at once and any errors are returned.",
			R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"},"overwrite":{"type":"boolean","description":"Default true."}},"required":["path","content"]})",
			false, true, &GiantMCPTools::_write_file);
	_add_tool("attach_script", "Attach script",
			"Attach a script file to a node. Undoable.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"script_path":{"type":"string"}},"required":["node_path","script_path"]})",
			false, false, &GiantMCPTools::_attach_script);
	_add_tool("get_log", "Get log",
			"Recent editor and game output, including errors. Pass since from a previous call to get only new lines.",
			R"({"type":"object","properties":{"since":{"type":"integer"},"max":{"type":"integer","description":"Default 100."},"errors_only":{"type":"boolean"}}})",
			true, false, &GiantMCPTools::_get_log);
	_add_tool("play", "Play",
			"Run the game from the open scene or the main scene.",
			R"({"type":"object","properties":{"scene":{"type":"string","enum":["current","main"]}}})",
			false, false, &GiantMCPTools::_play);
	_add_tool("stop", "Stop",
			"Stop the running game.",
			R"({"type":"object","properties":{}})",
			false, false, &GiantMCPTools::_stop);
	_add_tool("undo", "Undo",
			"Undo the last change in the open scene, whether an AI or a person made it.",
			R"({"type":"object","properties":{}})",
			false, false, &GiantMCPTools::_undo);
	_add_tool("redo", "Redo",
			"Redo the last undone change in the open scene.",
			R"({"type":"object","properties":{}})",
			false, false, &GiantMCPTools::_redo);
	_add_tool("screenshot", "Screenshot",
			"A PNG of the editor's 3D or 2D viewport, or of the whole editor window.",
			R"({"type":"object","properties":{"view":{"type":"string","enum":["3d","2d","editor"],"description":"Default 3d."},"index":{"type":"integer","description":"Which 3D viewport when split. Default 0."},"max_width":{"type":"integer","description":"Downscale wider images. Default 1280."}}})",
			true, false, &GiantMCPTools::_screenshot);
}
