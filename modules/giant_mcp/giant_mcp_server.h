/**************************************************************************/
/*  giant_mcp_server.h                                                    */
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

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

class GiantMCPTools;

// A Model Context Protocol server using the Streamable HTTP transport.
// It speaks the stateless 2026-07-28 revision and still answers clients that
// use the older initialize handshake (2025-03-26 to 2025-11-25).
// Everything runs on the main thread from poll(), so tools can touch the scene freely.
class GiantMCPServer {
public:
	static constexpr const char *LATEST_PROTOCOL_VERSION = "2026-07-28";
	static constexpr const char *ENDPOINT_PATH = "/mcp";

private:
	struct Client {
		Ref<StreamPeerTCP> peer;
		PackedByteArray buffer;
		uint64_t started_msec = 0;
		bool done = false;
	};

	struct HTTPResponse {
		int status = 200;
		String content_type = "application/json";
		String body;
	};

	Ref<TCPServer> server;
	LocalVector<Client> clients;
	String token;
	GiantMCPTools *tools = nullptr;

	static PackedStringArray _get_supported_versions();
	static bool _is_modern_version(const String &p_version);
	static Dictionary _server_info();
	static String _decode_header_value(const String &p_value);

	void _process_client(Client &p_client);
	void _send(Client &p_client, const HTTPResponse &p_response);
	HTTPResponse _handle_http(const String &p_method, const String &p_path, const HashMap<String, String> &p_headers, const String &p_body);
	HTTPResponse _handle_jsonrpc(const Dictionary &p_message, const HashMap<String, String> &p_headers);

	static HTTPResponse _json_response(int p_status, const Dictionary &p_body);
	static Dictionary _error_message(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data = Variant());
	static Dictionary _result_message(const Variant &p_id, Dictionary p_result);

public:
	Error start(int p_port, const String &p_token, GiantMCPTools *p_tools);
	void stop();
	void poll();
	bool is_listening() const;
	int get_port() const;

	~GiantMCPServer();
};
