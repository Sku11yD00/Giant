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
    // 2: not run, and no reply: the editor holds it until the person has
    // answered whether it may run (HeldTool then).
    public delegate* unmanaged<byte*, byte*, byte**, int> RunTool;

    // The tool held. what 0: has the person answered? (0 yes, 2 not yet.)
    // 1: its reply as RunTool's (run if they allowed it). 2: it is
    // withdrawn (the work was stopped).
    public delegate* unmanaged<int, byte**, int> HeldTool;

    // A picture of the result of the tool run last (RunTool, or HeldTool's
    // finish): the index-th. 1 with its bytes (the editor's: copied here
    // before the next call), their count and its type; 0 when there is no
    // such picture.
    public delegate* unmanaged<int, byte**, int*, byte**, int> ToolImage;

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
    private static string? s_key; // the key s_client was made for
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
    /// may take a while). Returns the conversation's stamp: odd while Claude
    /// is working, and another number whenever a line was added.
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
            s_conversation.RunPending(s_editor);
            return s_conversation.Stamp;
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
                string? error = s_conversation.Send(text, out long serial);
                // (`after` is the message's own line: reading on from it misses nothing.)
                return error == null ? (true, s_conversation.Read(serial, 0, true)) : (false, error);
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

    // The client for the key there is now: looked for with every message, so
    // a key given, or corrected, while the editor runs is the one used.
    private static AnthropicClient? Connect()
    {
        string? key = Keys.Find();
        if (key == null)
        {
            return s_client; // (the last key that was found, if its file has gone since)
        }
        if (s_client == null || key != s_key)
        {
            s_key = key;
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

    private const int Held = 2;

    private sealed class Editor : IToolRunner
    {
        public ToolReply? Run(string name, string input)
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
            string text = Taken(reply);
            return status == Held ? null : new ToolReply(status == 0, text, Pictures());
        }

        public bool HeldAnswered()
        {
            byte* reply = null;
            int status = s_api.HeldTool(0, &reply);
            Taken(reply);
            return status != Held;
        }

        public ToolReply FinishHeld()
        {
            byte* reply = null;
            int status = s_api.HeldTool(1, &reply);
            return new ToolReply(status == 0, Taken(reply), Pictures());
        }

        public void DropHeld()
        {
            byte* reply = null;
            s_api.HeldTool(2, &reply);
            Taken(reply);
        }

        // The pictures of the result of the tool just run, copied.
        private static List<ToolImage>? Pictures()
        {
            List<ToolImage>? pictures = null;
            for (int index = 0; index < 16; ++index)
            {
                byte* data = null;
                int size = 0;
                byte* type = null;
                if (s_api.ToolImage(index, &data, &size, &type) == 0 || data == null || size <= 0)
                {
                    break;
                }
                (pictures ??= []).Add(new ToolImage(new ReadOnlySpan<byte>(data, size).ToArray(), Marshal.PtrToStringUTF8((IntPtr)type) ?? "image/png"));
            }
            return pictures;
        }

        // The editor's string as ours, and freed.
        private static string Taken(byte* reply)
        {
            if (reply == null)
            {
                return "";
            }
            string text = Marshal.PtrToStringUTF8((IntPtr)reply) ?? "";
            s_api.FreeString(reply);
            return text;
        }
    }

    private static readonly Editor s_editor = new();

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
