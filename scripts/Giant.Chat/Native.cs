using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Anthropic;

namespace Giant.Chat;

/// <summary>Functions the editor hands to the chat (ChatApi in chat_bridge.cpp).</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeChatApi
{
    // Runs one of the chat's tools (name, arguments as JSON). Writes a
    // malloc'd UTF-8 string (the result or the error) to *reply; returns 0
    // on success, 1 on failure.
    public delegate* unmanaged<byte*, byte*, byte**, int> RunTool;

    // Writes a log line: level 0 trace, 1 info, 2 warning, 3 error.
    public delegate* unmanaged<int, byte*, void> Log;

    // Frees a string from RunTool.
    public delegate* unmanaged<byte*, void> FreeString;
}

/// <summary>
/// The entry points the editor calls (editor use only), all on its main
/// thread. No exception may leave an entry point (that would end the
/// process).
/// </summary>
public static unsafe class Native
{
    private static NativeChatApi s_api;
    private static Conversation? s_conversation;
    private static AnthropicClient? s_client;
    private static byte* s_reply; // the last Ask's answer, kept until the next
    private static long s_looked; // when a key was last looked for, for reading
    private static bool s_everLooked;

    /// <summary>
    /// Receives the editor's functions and the tools Claude may use (a JSON
    /// array of {name, description, input_schema}); 0 on success.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int Initialize(void* api, int size, byte* toolsJson)
    {
        try
        {
            if (size != sizeof(NativeChatApi))
            {
                return 1; // editor and chat built from different sources
            }
            s_api = *(NativeChatApi*)api;
            List<ToolSpec> tools = [];
            using (JsonDocument document = JsonDocument.Parse(Marshal.PtrToStringUTF8((IntPtr)toolsJson) ?? "[]"))
            {
                foreach (JsonElement tool in document.RootElement.EnumerateArray())
                {
                    tools.Add(new ToolSpec(tool.GetProperty("name").GetString() ?? "",
                                           tool.GetProperty("description").GetString() ?? "",
                                           tool.GetProperty("input_schema").Clone()));
                }
            }
            s_conversation = new Conversation(Connect, tools, Prompt.System);
            return 0;
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Log(3, "the chat could not start: " + e.Message);
            return 1;
        }
    }

    /// <summary>
    /// Asks the chat for something: "send" {text}, "read" {after, limit},
    /// "stop" or "clear". Writes the answer (JSON on success, the error on
    /// failure; valid until the next call) to *reply; 0 on success.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int Ask(byte* what, byte* argsJson, byte** reply)
    {
        bool ok;
        string text;
        try
        {
            (ok, text) = Answer(Marshal.PtrToStringUTF8((IntPtr)what) ?? "", Marshal.PtrToStringUTF8((IntPtr)argsJson) ?? "{}");
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            (ok, text) = (false, "the chat failed: " + e.Message);
        }
        NativeMemory.Free(s_reply);
        int length = Encoding.UTF8.GetByteCount(text);
        s_reply = (byte*)NativeMemory.Alloc((nuint)length + 1);
        Encoding.UTF8.GetBytes(text, new Span<byte>(s_reply, length));
        s_reply[length] = 0;
        *reply = s_reply;
        return ok ? 0 : 1;
    }

    /// <summary>
    /// Runs one tool Claude is waiting for, if any (one a frame: a command
    /// may take a while). Returns 1 while Claude is working, else 0.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int Update()
    {
        try
        {
            if (s_conversation == null)
            {
                return 0;
            }
            s_conversation.RunPending(RunTool);
            return s_conversation.Busy ? 1 : 0;
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Log(3, "the chat failed: " + e.Message);
            return 0;
        }
    }

    private static (bool Ok, string Text) Answer(string what, string argsJson)
    {
        if (s_conversation == null)
        {
            return (false, "the chat did not start");
        }
        using JsonDocument args = JsonDocument.Parse(argsJson);
        JsonElement root = args.RootElement;
        switch (what)
        {
            case "send":
            {
                string text = root.TryGetProperty("text", out JsonElement t) ? t.GetString() ?? "" : "";
                string? error = s_conversation.Send(text);
                return error == null ? (true, s_conversation.Read(long.MaxValue, 0, true)) : (false, error);
            }
            case "read":
            {
                long after = root.TryGetProperty("after", out JsonElement a) ? a.GetInt64() : 0;
                int limit = root.TryGetProperty("limit", out JsonElement l) ? l.GetInt32() : 64;
                return (true, s_conversation.Read(after, limit, KeyFound()));
            }
            case "stop":
                return (true, s_conversation.Stop() ? "{\"stopped\":true}" : "{\"stopped\":false}");
            case "clear":
                s_conversation.Clear();
                return (true, "{}");
            default:
                return (false, "the chat does not know \"" + what + "\"");
        }
    }

    // The client, made when a key is first found (so a key file written
    // while the editor runs is picked up).
    private static AnthropicClient? Connect()
    {
        if (s_client == null && Keys.Find() is { } key)
        {
            s_client = new AnthropicClient { ApiKey = key };
        }
        return s_client;
    }

    // Whether there is a key, for reading the chat: looked for at most once a
    // second (the chat may be read every frame; sending always looks).
    private static bool KeyFound()
    {
        long now = Environment.TickCount64;
        if (s_client == null && (!s_everLooked || now - s_looked >= 1000))
        {
            s_everLooked = true;
            s_looked = now;
            Connect();
        }
        return s_client != null;
    }

    private static ToolReply RunTool(string name, string input)
    {
        byte[] nameUtf8 = Encoding.UTF8.GetBytes(name + '\0');
        byte[] inputUtf8 = Encoding.UTF8.GetBytes(input + '\0');
        byte* reply = null;
        int status;
        fixed (byte* n = nameUtf8)
        fixed (byte* i = inputUtf8)
        {
            status = s_api.RunTool(n, i, &reply);
        }
        string text = reply != null ? Marshal.PtrToStringUTF8((IntPtr)reply) ?? "" : "";
        if (reply != null)
        {
            s_api.FreeString(reply);
        }
        return new ToolReply(status == 0, text);
    }

    private static void Log(int level, string message)
    {
        if (s_api.Log == null)
        {
            return;
        }
        fixed (byte* text = Encoding.UTF8.GetBytes(message + '\0'))
        {
            s_api.Log(level, text);
        }
    }
}
