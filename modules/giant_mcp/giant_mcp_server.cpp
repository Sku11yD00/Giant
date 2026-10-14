/**************************************************************************/
/*  giant_mcp_server.cpp                                                  */
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

#include "giant_mcp_server.h"

#include "giant_mcp_tools.h"

#include "core/crypto/crypto_core.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"

// Requests larger than this are refused, which bounds memory use per client.
static constexpr int MAX_REQUEST_BYTES = 32 * 1024 * 1024;
static constexpr uint64_t CLIENT_TIMEOUT_MSEC = 30000;

// JSON-RPC and MCP error codes.
static constexpr int ERROR_PARSE = -32700;
static constexpr int ERROR_INVALID_REQUEST = -32600;
static constexpr int ERROR_METHOD_NOT_FOUND = -32601;
static constexpr int ERROR_INVALID_PARAMS = -32602;
static constexpr int ERROR_HEADER_MISMATCH = -32020;
static constexpr int ERROR_UNSUPPORTED_PROTOCOL_VERSION = -32022;

PackedStringArray GiantMCPServer::_get_supported_versions() {
	PackedStringArray versions;
	versions.push_back("2026-07-28");
	versions.push_back("2025-11-25");
	versions.push_back("2025-06-18");
	versions.push_back("2025-03-26");
	return versions;
}

bool GiantMCPServer::_is_modern_version(const String &p_version) {
	// Revisions from 2026-07-28 on are stateless and validate request headers.
	return p_version >= String(LATEST_PROTOCOL_VERSION);
}

Dictionary GiantMCPServer::_server_info() {
	Dictionary info;
	info["name"] = "giant-engine";
	info["title"] = "Giant Engine Editor";
	info["version"] = GODOT_VERSION_FULL_CONFIG;
	return info;
}

String GiantMCPServer::_decode_header_value(const String &p_value) {
	// Values that are not plain ASCII arrive as =?base64?...?= (Streamable HTTP, "Value Encoding").
	if (p_value.begins_with("=?base64?") && p_value.ends_with("?=")) {
		String encoded = p_value.substr(9, p_value.length() - 11);
		CharString ascii = encoded.ascii();
		Vector<uint8_t> decoded;
		decoded.resize(ascii.length());
		size_t written = 0;
		if (CryptoCore::b64_decode(decoded.ptrw(), decoded.size(), &written, (const uint8_t *)ascii.get_data(), ascii.length()) != OK) {
			return String();
		}
		return String::utf8((const char *)decoded.ptr(), written);
	}
	return p_value;
}

Error GiantMCPServer::start(int p_port, const String &p_token, GiantMCPTools *p_tools) {
	stop();
	token = p_token;
	tools = p_tools;
	server.instantiate();
	// Bind to the loopback interface only, so nothing outside this PC can connect.
	return server->listen(p_port, IPAddress("127.0.0.1"));
}

void GiantMCPServer::stop() {
	for (Client &client : clients) {
		if (client.peer.is_valid()) {
			client.peer->disconnect_from_host();
		}
	}
	clients.clear();
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
}

bool GiantMCPServer::is_listening() const {
	return server.is_valid() && server->is_listening();
}

int GiantMCPServer::get_port() const {
	return is_listening() ? server->get_local_port() : 0;
}

void GiantMCPServer::poll() {
	if (!is_listening()) {
		return;
	}
	while (server->is_connection_available()) {
		Client client;
		client.peer = server->take_connection();
		client.started_msec = OS::get_singleton()->get_ticks_msec();
		clients.push_back(client);
	}
	for (uint32_t i = 0; i < clients.size(); i++) {
		_process_client(clients[i]);
	}
	for (int64_t i = int64_t(clients.size()) - 1; i >= 0; i--) {
		if (clients[i].done) {
			clients.remove_at_unordered(i);
		}
	}
}

void GiantMCPServer::_process_client(Client &p_client) {
	Ref<StreamPeerTCP> peer = p_client.peer;
	peer->poll();
	if (peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		p_client.done = true;
		return;
	}
	if (OS::get_singleton()->get_ticks_msec() - p_client.started_msec > CLIENT_TIMEOUT_MSEC) {
		peer->disconnect_from_host();
		p_client.done = true;
		return;
	}

	int available = peer->get_available_bytes();
	if (available > 0) {
		int offset = p_client.buffer.size();
		p_client.buffer.resize(offset + available);
		int received = 0;
		peer->get_partial_data(p_client.buffer.ptrw() + offset, available, received);
		p_client.buffer.resize(offset + received);
	}
	if (p_client.buffer.size() > MAX_REQUEST_BYTES) {
		HTTPResponse response;
		response.status = 413;
		_send(p_client, response);
		return;
	}

	// Wait for the full header block.
	const uint8_t *data = p_client.buffer.ptr();
	int size = p_client.buffer.size();
	int header_end = -1;
	for (int i = 0; i + 3 < size; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
			header_end = i;
			break;
		}
	}
	if (header_end < 0) {
		return;
	}

	String header_text = String::utf8((const char *)data, header_end);
	Vector<String> lines = header_text.split("\r\n");
	Vector<String> request_line = lines[0].split(" ");
	if (request_line.size() < 2) {
		HTTPResponse response;
		response.status = 400;
		_send(p_client, response);
		return;
	}

	HashMap<String, String> headers;
	for (int i = 1; i < lines.size(); i++) {
		int colon = lines[i].find(":");
		if (colon > 0) {
			// Header names are case-insensitive, so they are stored lowercase.
			headers[lines[i].substr(0, colon).strip_edges().to_lower()] = lines[i].substr(colon + 1).strip_edges();
		}
	}

	int content_length = headers.has("content-length") ? headers["content-length"].to_int() : 0;
	int body_start = header_end + 4;
	if (size - body_start < content_length) {
		return; // Body still arriving.
	}

	String body = String::utf8((const char *)data + body_start, content_length);
	String path = request_line[1].split("?")[0];
	_send(p_client, _handle_http(request_line[0], path, headers, body));
}

void GiantMCPServer::_send(Client &p_client, const HTTPResponse &p_response) {
	String status_text;
	switch (p_response.status) {
		case 200:
			status_text = "OK";
			break;
		case 202:
			status_text = "Accepted";
			break;
		case 400:
			status_text = "Bad Request";
			break;
		case 401:
			status_text = "Unauthorized";
			break;
		case 403:
			status_text = "Forbidden";
			break;
		case 404:
			status_text = "Not Found";
			break;
		case 405:
			status_text = "Method Not Allowed";
			break;
		case 413:
			status_text = "Content Too Large";
			break;
		default:
			status_text = "Error";
			break;
	}
	CharString body = p_response.body.utf8();
	String head = vformat("HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n", p_response.status, status_text, body.length());
	if (!p_response.body.is_empty()) {
		head += "Content-Type: " + p_response.content_type + "\r\n";
	}
	if (p_response.status == 401) {
		head += "WWW-Authenticate: Bearer\r\n";
	}
	if (p_response.status == 405) {
		head += "Allow: POST\r\n";
	}
	head += "\r\n";
	CharString head_utf8 = head.utf8();
	p_client.peer->put_data((const uint8_t *)head_utf8.get_data(), head_utf8.length());
	if (body.length() > 0) {
		p_client.peer->put_data((const uint8_t *)body.get_data(), body.length());
	}
	p_client.peer->disconnect_from_host();
	p_client.done = true;
}

GiantMCPServer::HTTPResponse GiantMCPServer::_json_response(int p_status, const Dictionary &p_body) {
	HTTPResponse response;
	response.status = p_status;
	response.body = JSON::stringify(p_body, "", false);
	return response;
}

Dictionary GiantMCPServer::_error_message(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	if (p_data.get_type() != Variant::NIL) {
		error["data"] = p_data;
	}
	Dictionary message;
	message["jsonrpc"] = "2.0";
	message["id"] = p_id;
	message["error"] = error;
	return message;
}

Dictionary GiantMCPServer::_result_message(const Variant &p_id, Dictionary p_result) {
	if (!p_result.has("resultType")) {
		p_result["resultType"] = "complete";
	}
	Dictionary meta = p_result.get("_meta", Dictionary());
	meta["io.modelcontextprotocol/serverInfo"] = _server_info();
	p_result["_meta"] = meta;
	Dictionary message;
	message["jsonrpc"] = "2.0";
	message["id"] = p_id;
	message["result"] = p_result;
	return message;
}

GiantMCPServer::HTTPResponse GiantMCPServer::_handle_http(const String &p_method, const String &p_path, const HashMap<String, String> &p_headers, const String &p_body) {
	// Reject cross-site requests from web pages (DNS rebinding protection).
	if (p_headers.has("origin")) {
		String origin = p_headers["origin"];
		bool local = origin.begins_with("http://127.0.0.1") || origin.begins_with("http://localhost") || origin.begins_with("https://127.0.0.1") || origin.begins_with("https://localhost");
		if (!local) {
			return _json_response(403, _error_message(Variant(), ERROR_INVALID_REQUEST, "Origin not allowed."));
		}
	}

	if (!token.is_empty()) {
		String expected = "Bearer " + token;
		if (!p_headers.has("authorization") || p_headers["authorization"] != expected) {
			return _json_response(401, _error_message(Variant(), ERROR_INVALID_REQUEST, "Missing or wrong bearer token. Find it in Editor Settings > Giant MCP > Token."));
		}
	}

	if (p_path != ENDPOINT_PATH) {
		HTTPResponse response;
		response.status = 404;
		return response;
	}
	if (p_method != "POST") {
		HTTPResponse response;
		response.status = 405;
		return response;
	}

	JSON json;
	if (json.parse(p_body) != OK) {
		return _json_response(400, _error_message(Variant(), ERROR_PARSE, "Parse error: " + json.get_error_message()));
	}
	if (json.get_data().get_type() != Variant::DICTIONARY) {
		return _json_response(400, _error_message(Variant(), ERROR_INVALID_REQUEST, "Expected a single JSON-RPC message object."));
	}
	return _handle_jsonrpc(json.get_data(), p_headers);
}

GiantMCPServer::HTTPResponse GiantMCPServer::_handle_jsonrpc(const Dictionary &p_message, const HashMap<String, String> &p_headers) {
	const Variant id = p_message.get("id", Variant());
	const String method = p_message.get("method", "");
	const Dictionary params = p_message.get("params", Dictionary());
	const Dictionary meta = params.get("_meta", Dictionary());

	if (method.is_empty()) {
		if (p_message.has("result") || p_message.has("error")) {
			return _json_response(400, _error_message(Variant(), ERROR_INVALID_REQUEST, "Clients must not send JSON-RPC responses."));
		}
		return _json_response(400, _error_message(id, ERROR_INVALID_REQUEST, "Missing method."));
	}

	// Notifications get 202 with no body. Older clients send notifications/initialized.
	if (!p_message.has("id")) {
		HTTPResponse response;
		response.status = 202;
		return response;
	}

	const PackedStringArray supported = _get_supported_versions();

	// Older clients start with the initialize handshake. The server keeps no session, so this only negotiates a version.
	if (method == "initialize") {
		String requested = params.get("protocolVersion", "");
		String version = (supported.has(requested) && !_is_modern_version(requested)) ? requested : String("2025-11-25");
		Dictionary capabilities;
		capabilities["tools"] = Dictionary();
		Dictionary result;
		result["protocolVersion"] = version;
		result["capabilities"] = capabilities;
		result["serverInfo"] = _server_info();
		result["instructions"] = GiantMCPTools::get_instructions();
		return _json_response(200, _result_message(id, result));
	}

	String version;
	if (p_headers.has("mcp-protocol-version")) {
		version = p_headers["mcp-protocol-version"];
	} else if (meta.has("io.modelcontextprotocol/protocolVersion")) {
		version = meta["io.modelcontextprotocol/protocolVersion"];
	} else {
		// Clients older than 2025-06-18 did not send the header.
		version = "2025-03-26";
	}

	if (!supported.has(version)) {
		Dictionary data;
		data["supported"] = supported;
		data["requested"] = version;
		return _json_response(400, _error_message(id, ERROR_UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version: " + version, data));
	}

	const bool modern = _is_modern_version(version);
	if (modern) {
		// The 2026-07-28 revision requires headers that mirror the body, and they must match.
		if (meta.has("io.modelcontextprotocol/protocolVersion") && String(meta["io.modelcontextprotocol/protocolVersion"]) != version) {
			return _json_response(400, _error_message(id, ERROR_HEADER_MISMATCH, "Header mismatch: MCP-Protocol-Version does not match _meta protocolVersion."));
		}
		if (!p_headers.has("mcp-method") || p_headers["mcp-method"] != method) {
			return _json_response(400, _error_message(id, ERROR_HEADER_MISMATCH, "Header mismatch: Mcp-Method header is missing or does not match the method."));
		}
		if (method == "tools/call") {
			String name = params.get("name", "");
			if (!p_headers.has("mcp-name") || _decode_header_value(p_headers["mcp-name"]) != name) {
				return _json_response(400, _error_message(id, ERROR_HEADER_MISMATCH, "Header mismatch: Mcp-Name header is missing or does not match the tool name."));
			}
		}
	}

	if (method == "server/discover") {
		Dictionary capabilities;
		capabilities["tools"] = Dictionary();
		Dictionary result;
		result["supportedVersions"] = supported;
		result["capabilities"] = capabilities;
		result["instructions"] = GiantMCPTools::get_instructions();
		result["ttlMs"] = 60000;
		result["cacheScope"] = "private";
		return _json_response(200, _result_message(id, result));
	}

	if (method == "ping" && !modern) {
		return _json_response(200, _result_message(id, Dictionary()));
	}

	if (method == "tools/list") {
		Dictionary result;
		result["tools"] = tools->list_tools();
		result["ttlMs"] = 60000;
		result["cacheScope"] = "private";
		return _json_response(200, _result_message(id, result));
	}

	if (method == "tools/call") {
		String name = params.get("name", "");
		Variant arguments = params.get("arguments", Dictionary());
		if (arguments.get_type() != Variant::DICTIONARY) {
			return _json_response(200, _error_message(id, ERROR_INVALID_PARAMS, "Tool arguments must be an object."));
		}
		bool found = false;
		Dictionary result = tools->call_tool(name, arguments, found);
		if (!found) {
			return _json_response(200, _error_message(id, ERROR_INVALID_PARAMS, "Unknown tool: " + name));
		}
		return _json_response(200, _result_message(id, result));
	}

	return _json_response(modern ? 404 : 200, _error_message(id, ERROR_METHOD_NOT_FOUND, "Method not found: " + method));
}

GiantMCPServer::~GiantMCPServer() {
	stop();
}
