using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Anthropic;
using Giant.Chat;

namespace ChatTests;

/// <summary>Stands in for Claude's service: answers each request with the next reply set up, and keeps what was asked.</summary>
internal sealed class FakeService : HttpMessageHandler
{
    private readonly Lock _lock = new();
    private readonly Queue<(HttpStatusCode Status, string Body)> _replies = new();
    private readonly List<JsonDocument> _requests = [];
    private readonly List<string> _betas = [];
    private readonly List<string> _keys = [];
    /// <summary>When set, a request waits here until it is released (or stopped).</summary>
    public TaskCompletionSource? Hold;
    /// <summary>Answers every request the same way once the replies run out.</summary>
    public string? Always;
    /// <summary>When set, the next request fails with it instead of being answered.</summary>
    public Exception? Throw;

    public void Reply(string body, HttpStatusCode status = HttpStatusCode.OK)
    {
        lock (_lock)
        {
            _replies.Enqueue((status, body));
        }
    }

    public int Requests
    {
        get
        {
            lock (_lock)
            {
                return _requests.Count;
            }
        }
    }

    public JsonElement Request(int index)
    {
        lock (_lock)
        {
            return _requests[index].RootElement;
        }
    }

    public string Betas(int index) => _betas[index];
    public string Key(int index) => _keys[index];

    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken stop)
    {
        string body = request.Content == null ? "{}" : await request.Content.ReadAsStringAsync(stop).ConfigureAwait(false);
        (HttpStatusCode Status, string Body) reply;
        lock (_lock)
        {
            _requests.Add(JsonDocument.Parse(body));
            _betas.Add(request.Headers.TryGetValues("anthropic-beta", out IEnumerable<string>? betas) ? string.Join(",", betas) : "");
            // (As the SDK sends it: its own header, or a bearer token.)
            _keys.Add((request.Headers.TryGetValues("x-api-key", out IEnumerable<string>? keys) ? string.Join(",", keys) : "") +
                      (request.Headers.Authorization?.ToString() ?? ""));
            if (_replies.Count > 0)
            {
                reply = _replies.Dequeue();
            }
            else if (Always != null)
            {
                reply = (HttpStatusCode.OK, Always);
            }
            else
            {
                reply = (HttpStatusCode.InternalServerError, "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"no reply was set up\"}}");
            }
        }
        if (Throw is { } thrown)
        {
            Throw = null;
            throw thrown;
        }
        if (Hold is { } hold)
        {
            await hold.Task.WaitAsync(stop).ConfigureAwait(false);
        }
        return new HttpResponseMessage(reply.Status)
        {
            Content = new StringContent(reply.Body, Encoding.UTF8, "application/json"),
        };
    }
}

internal static class Program
{
    private static int s_failed;
    private static int s_checked;

    private static void Check(bool ok, string what)
    {
        ++s_checked;
        if (!ok)
        {
            ++s_failed;
            Console.WriteLine("FAILED: " + what);
        }
    }

    private static string Message(string stopReason, params string[] blocks) =>
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-5-5\",\"content\":[" +
        string.Join(",", blocks) + "],\"stop_reason\":\"" + stopReason +
        "\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";

    private static string Text(string text) => "{\"type\":\"text\",\"text\":" + JsonSerializer.Serialize(text) + "}";

    private static string Use(string id, string name, string inputJson) =>
        "{\"type\":\"tool_use\",\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"input\":" + inputJson + ",\"caller\":{\"type\":\"direct\"}}";

    private static readonly JsonElement s_schema = JsonSerializer.Deserialize<JsonElement>(
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"args\":{\"type\":\"object\"}},\"required\":[\"name\"]}");

    private static (Conversation, FakeService) Make(bool key = true)
    {
        FakeService service = new();
        AnthropicClient client = new() { ApiKey = "test-key", HttpClient = new HttpClient(service), MaxRetries = 0 };
        Conversation conversation = new(() => key ? client : null,
            [new ToolSpec("run_command", "Runs a command.", s_schema), new ToolSpec("find_commands", "Finds commands.", s_schema)],
            "You are in a test.");
        return (conversation, service);
    }

    // Lets the conversation work until it is idle, running the tools it asks for on this thread.
    private static bool Finish(Conversation conversation, Func<string, string, ToolReply>? run = null)
    {
        run ??= (_, _) => new ToolReply(true, "{}");
        DateTime until = DateTime.UtcNow.AddSeconds(30);
        while (conversation.Busy)
        {
            if (DateTime.UtcNow > until)
            {
                return false;
            }
            if (!conversation.RunPending(run))
            {
                Thread.Sleep(1);
            }
        }
        return true;
    }

    private static bool Until(Func<bool> done)
    {
        DateTime until = DateTime.UtcNow.AddSeconds(30);
        while (!done())
        {
            if (DateTime.UtcNow > until)
            {
                return false;
            }
            Thread.Sleep(1);
        }
        return true;
    }

    private static List<(string Kind, string Text, string? Tool, bool? Ok)> Lines(Conversation conversation, long after = 0, int limit = 256)
    {
        List<(string, string, string?, bool?)> lines = [];
        using JsonDocument read = JsonDocument.Parse(conversation.Read(after, limit, true));
        foreach (JsonElement e in read.RootElement.GetProperty("entries").EnumerateArray())
        {
            lines.Add((e.GetProperty("kind").GetString()!, e.GetProperty("text").GetString()!,
                       e.TryGetProperty("tool", out JsonElement tool) ? tool.GetString() : null,
                       e.TryGetProperty("ok", out JsonElement ok) ? ok.GetBoolean() : null));
        }
        return lines;
    }

    private static string Kinds(Conversation conversation) => string.Join(" ", Lines(conversation).ConvertAll(l => l.Kind));

    private static void AnAnswerInWords()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("end_turn", Text("Hello there.")));
        Check(c.Send("hi") == null, "a message goes out");
        Check(Finish(c), "the answer arrives");
        Check(Kinds(c) == "you claude", "the person's line, then Claude's: " + Kinds(c));
        Check(Lines(c)[1].Text == "Hello there.", "Claude's words");
        JsonElement asked = service.Request(0);
        Check(asked.GetProperty("model").GetString() == "claude-opus-5-5", "the model asked");
        Check(asked.GetProperty("system").ToString().Contains("You are in a test.", StringComparison.Ordinal), "the system text is sent");
        Check(asked.GetProperty("tools").GetArrayLength() == 2, "the tools are sent");
        Check(asked.GetProperty("tools")[0].GetProperty("input_schema").GetProperty("required")[0].GetString() == "name", "with their schemas");
        Check(asked.GetProperty("fallbacks").ToString() == "default", "the service's own fallback is asked for");
        Check(service.Betas(0).Contains("server-side-fallback-2026-07-01", StringComparison.Ordinal), "under its beta: " + service.Betas(0));
        Check(asked.GetProperty("output_config").GetProperty("effort").GetString() == "medium", "the effort is set");
        Check(asked.GetProperty("cache_control").GetProperty("type").GetString() == "ephemeral", "what repeats is cached");
        Check(!asked.TryGetProperty("tool_choice", out _), "Claude chooses its tools");
        Check(service.Key(0).Contains("test-key", StringComparison.Ordinal), "the key goes in a header");
        using JsonDocument read = JsonDocument.Parse(c.Read(0, 64, true));
        Check(read.RootElement.GetProperty("input_tokens").GetInt64() == 10 && read.RootElement.GetProperty("output_tokens").GetInt64() == 5, "the tokens are counted");
        Check(!read.RootElement.GetProperty("busy").GetBoolean() && read.RootElement.GetProperty("latest").GetInt64() == 2, "idle, two lines");
    }

    private static void ToolsRunOnTheCallingThread()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use",
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-1\"}",
            Text("I will look."),
            Use("toolu_1", "find_commands", "{\"name\":\"tree\"}"),
            Use("toolu_2", "run_command", "{\"name\":\"bad\",\"args\":{\"x\":1}}")));
        service.Reply(Message("end_turn", Text("Done.")));
        int main = Environment.CurrentManagedThreadId;
        List<string> ran = [];
        bool onMain = true;
        Check(c.Send("plant a tree") == null, "a message goes out");
        Check(Finish(c, (name, input) =>
        {
            onMain &= Environment.CurrentManagedThreadId == main;
            ran.Add(name + " " + input);
            return name == "run_command" ? new ToolReply(false, "no such command") : new ToolReply(true, "{\"commands\":[]}");
        }), "the work ends");
        Check(onMain, "tools run on the thread that gives them a turn");
        Check(ran.Count == 2 && ran[0] == "find_commands {\"name\":\"tree\"}" && ran[1] == "run_command {\"name\":\"bad\",\"args\":{\"x\":1}}",
              "each tool once, in order, with its arguments: " + string.Join(" | ", ran));
        Check(Kinds(c) == "you claude tool result tool result claude", "the lines: " + Kinds(c));
        var lines = Lines(c);
        Check(lines[2].Tool == "find_commands" && lines[3].Ok == true && lines[5].Ok == false && lines[5].Text == "no such command", "what was run and what came back");
        Check(service.Requests == 2, "two requests");
        JsonElement messages = service.Request(1).GetProperty("messages");
        Check(messages.GetArrayLength() == 3, "the second carries the first's answer and the results");
        JsonElement said = messages[1].GetProperty("content");
        Check(messages[1].GetProperty("role").GetString() == "assistant" && said.GetArrayLength() == 4, "Claude's whole reply is handed back");
        Check(said[0].GetProperty("type").GetString() == "thinking" && said[0].GetProperty("signature").GetString() == "sig-1", "its thinking as it came");
        Check(said[2].GetProperty("id").GetString() == "toolu_1" && said[3].GetProperty("input").GetProperty("args").GetProperty("x").GetInt32() == 1, "its tool calls as they came");
        JsonElement results = messages[2].GetProperty("content");
        Check(messages[2].GetProperty("role").GetString() == "user" && results.GetArrayLength() == 2, "both results in one message");
        Check(results[0].GetProperty("tool_use_id").GetString() == "toolu_1" && results[1].GetProperty("tool_use_id").GetString() == "toolu_2", "each for its call");
        Check(results[1].GetProperty("is_error").GetBoolean() && results[1].GetProperty("content").ToString().Contains("no such command", StringComparison.Ordinal), "a failure is said to be one");
        Check(!(results[0].TryGetProperty("is_error", out JsonElement bad) && bad.GetBoolean()), "a success is not");
    }

    private static void WithoutAKeyNothingIsSent()
    {
        (Conversation c, FakeService service) = Make(key: false);
        string? error = c.Send("hi");
        Check(error != null && error.Contains("ANTHROPIC_API_KEY", StringComparison.Ordinal), "the person is told how to give a key: " + error);
        Check(!c.Busy && Lines(c).Count == 0 && service.Requests == 0, "and nothing is sent or kept");
        Check(c.Send("  ") != null, "an empty message is not sent");
    }

    private static void OneMessageAtATime()
    {
        (Conversation c, FakeService service) = Make();
        service.Hold = new TaskCompletionSource();
        service.Reply(Message("end_turn", Text("One.")));
        Check(c.Send("first", out long serial) == null && serial == 1, "the first goes out, as line 1");
        string? error = c.Send("second");
        Check(error != null && error.Contains("still working", StringComparison.Ordinal), "the second waits its turn: " + error);
        service.Hold.SetResult();
        Check(Finish(c) && Kinds(c) == "you claude", "the first is answered: " + Kinds(c));
    }

    private static void ADeclinedRequestIsNotKept()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("refusal", Text("partial"), Use("toolu_1", "run_command", "{\"name\":\"x\"}")));
        service.Reply(Message("end_turn", Text("Fine.")));
        int ran = 0;
        c.Send("something");
        Check(Finish(c, (_, _) => { ++ran; return new ToolReply(true, "{}"); }), "the work ends");
        Check(ran == 0, "a declined reply's tools are not run");
        Check(Kinds(c) == "you error" && Lines(c)[1].Text.Contains("declined", StringComparison.Ordinal), "the person is told: " + Kinds(c));
        c.Send("something else");
        Check(Finish(c) && Kinds(c) == "you error you claude", "the conversation goes on: " + Kinds(c));
        JsonElement messages = service.Request(1).GetProperty("messages");
        Check(messages.GetArrayLength() == 1 && messages[0].GetProperty("role").GetString() == "user" &&
              messages[0].GetProperty("content").ToString().Contains("something else", StringComparison.Ordinal),
              "without the declined reply, and without the message that was declined (it would be declined again)");

        // Declined part way through the work: what was done stays said, and the person is told what to do if it goes on.
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        service.Reply(Message("refusal"));
        service.Reply(Message("end_turn", Text("Yes.")));
        c.Send("a third thing");
        Check(Finish(c) && Lines(c)[^1].Kind == "error" && Lines(c)[^1].Text.Contains("clear", StringComparison.Ordinal), "declined after a tool: " + Lines(c)[^1].Text);
        c.Send("and now?");
        Check(Finish(c) && Lines(c)[^1].Text == "Yes.", "the next message is answered");
        messages = service.Request(service.Requests - 1).GetProperty("messages");
        Check(messages.GetArrayLength() == 6 && messages[4].GetProperty("content")[0].GetProperty("tool_use_id").GetString() == "toolu_1" &&
              messages[5].GetProperty("content").ToString().Contains("and now?", StringComparison.Ordinal), "with the tool's call and its answer kept together");
    }

    private static void RepliesThatCannotBeKept()
    {
        // Thoughts alone, or a text with nothing in it: nothing to hand back.
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("end_turn", "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig\"}", Text("  ")));
        c.Send("hm");
        Check(Finish(c) && Kinds(c) == "you note" && Lines(c)[1].Text.Contains("nothing to say", StringComparison.Ordinal), "the person is told: " + Kinds(c));
        service.Reply(Message("end_turn"));
        c.Send("hm?");
        Check(Finish(c) && Kinds(c) == "you note you note", "a reply of nothing at all too: " + Kinds(c));
        Check(service.Request(1).GetProperty("messages").GetArrayLength() == 2, "neither was kept");
        // An empty text beside real content is left out of what is handed back.
        service.Reply(Message("tool_use", Text(""), Text("Looking."), Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        service.Reply(Message("end_turn", Text("Done.")));
        c.Send("go");
        Check(Finish(c), "the work ends");
        JsonElement said = service.Request(service.Requests - 1).GetProperty("messages")[3].GetProperty("content");
        Check(said.GetArrayLength() == 2 && said[0].GetProperty("text").GetString() == "Looking.", "the empty text is not handed back");
        // Tools asked for by a reply that did not end by asking for them (cut short some other way): not run.
        service.Reply(Message("model_context_window_exceeded", Use("toolu_2", "run_command", "{\"name\":\"b\"}")));
        int ran = 0;
        c.Send("more");
        Check(Finish(c, (_, _) => { ++ran; return new ToolReply(true, "{}"); }) && ran == 0 && Lines(c)[^1].Kind == "error", "a reply cut short some other way runs nothing");
        // Words cut at the length limit are kept, so "go on" has something to go on from.
        service.Reply(Message("max_tokens", Text("Once upon a")));
        service.Reply(Message("end_turn", Text(" time.")));
        c.Send("a story");
        Check(Finish(c) && Lines(c)[^1].Kind == "note" && Lines(c)[^1].Text.Contains("go on", StringComparison.Ordinal), "cut words: the person is told how to get the rest");
        int before = service.Requests;
        c.Send("go on");
        Check(Finish(c), "answered");
        JsonElement messages = service.Request(before).GetProperty("messages");
        Check(messages[messages.GetArrayLength() - 2].GetProperty("role").GetString() == "assistant" &&
              messages[messages.GetArrayLength() - 2].GetProperty("content").ToString().Contains("Once upon a", StringComparison.Ordinal), "and Claude has what it wrote");
    }

    private static void ACutOffReplyRunsNothing()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("max_tokens", Text("I will"), Use("toolu_1", "run_command", "{\"name\":\"delete_ent\"}")));
        int ran = 0;
        c.Send("delete it");
        Check(Finish(c, (_, _) => { ++ran; return new ToolReply(true, "{}"); }), "the work ends");
        Check(ran == 0, "a tool whose arguments may be cut short is not run");
        Check(Kinds(c) == "you claude error" && Lines(c)[2].Text.Contains("cut off", StringComparison.Ordinal), "the person is told: " + Kinds(c));
        service.Reply(Message("end_turn", Text("Ok.")));
        c.Send("again");
        Check(Finish(c), "the next message works");
        Check(service.Request(1).GetProperty("messages").GetArrayLength() == 2, "the cut reply was not kept (no call left unanswered)");
    }

    private static void StoppingLeavesTheConversationWhole()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}"), Use("toolu_2", "run_command", "{\"name\":\"b\"}")));
        c.Send("do two things");
        Check(Until(() => Kinds(c) == "you tool"), "Claude waits for its first tool: " + Kinds(c));
        Check(c.Stop(), "there was work to stop");
        Check(Until(() => !c.Busy), "it stops");
        int ran = 0;
        Check(!c.RunPending((_, _) => { ++ran; return new ToolReply(true, "{}"); }) && ran == 0, "the waiting tool is not run after");
        Check(Kinds(c) == "you tool note" && Lines(c)[2].Text == "Stopped.", "the person is told: " + Kinds(c));
        Check(!c.Stop(), "nothing more to stop");
        service.Reply(Message("end_turn", Text("Ok.")));
        c.Send("carry on");
        Check(Finish(c), "the next message works");
        JsonElement messages = service.Request(1).GetProperty("messages");
        Check(messages.GetArrayLength() == 4, "the person, Claude's calls, their answers, the person");
        JsonElement results = messages[2].GetProperty("content");
        Check(results.GetArrayLength() == 2 && results[0].GetProperty("is_error").GetBoolean() && results[1].GetProperty("is_error").GetBoolean() &&
              results[0].GetProperty("content").ToString().Contains("stopped", StringComparison.Ordinal),
              "every call is answered: stopped ones as not run");

        // Stopped while the service is still answering: nothing of that request is kept.
        (c, service) = Make();
        service.Hold = new TaskCompletionSource();
        service.Reply(Message("end_turn", Text("Late.")));
        c.Send("wait");
        Check(Until(() => service.Requests == 1), "the request is out");
        c.Stop();
        Check(Until(() => !c.Busy) && Kinds(c) == "you note", "it stops: " + Kinds(c));
        service.Hold = null;
        service.Reply(Message("end_turn", Text("Now.")));
        c.Send("and now");
        Check(Finish(c) && Lines(c)[^1].Text == "Now.", "the next message is answered");
        JsonElement after = service.Request(service.Requests - 1).GetProperty("messages");
        Check(after.GetArrayLength() == 2 && after[1].GetProperty("role").GetString() == "user", "with nothing of the stopped request's answer");
    }

    // An editor that holds the tools it is told to until the person answers (the editor's "ask before changes").
    private sealed class HoldingEditor : IToolRunner
    {
        public Func<string, bool> Holds = _ => true;
        public bool Answered;
        public bool Allowed;
        public string? Held;
        public readonly List<string> Ran = [];
        public int Dropped;
        public int Asked; // how often it was asked whether the person answered

        public ToolReply? Run(string name, string input)
        {
            if (Holds(input))
            {
                Held = input;
                Answered = false;
                return null;
            }
            Ran.Add(input);
            return new ToolReply(true, "{\"ran\":true}");
        }

        public bool HeldAnswered()
        {
            ++Asked;
            return Held == null || Answered;
        }

        public ToolReply FinishHeld()
        {
            string? input = Held;
            Held = null;
            if (input == null)
            {
                return new ToolReply(false, "nothing is held");
            }
            if (!Allowed)
            {
                return new ToolReply(false, "not run: the person did not allow this");
            }
            Ran.Add(input);
            return new ToolReply(true, "{\"ran\":true}");
        }

        public void DropHeld()
        {
            ++Dropped;
            Held = null;
        }
    }

    // Gives the conversation turns until `done`, as the editor's frames would.
    private static bool Frames(Conversation conversation, IToolRunner editor, Func<bool> done)
    {
        DateTime until = DateTime.UtcNow.AddSeconds(30);
        while (!done())
        {
            if (DateTime.UtcNow > until)
            {
                return false;
            }
            if (!conversation.RunPending(editor))
            {
                Thread.Sleep(1);
            }
        }
        return true;
    }

    private static void AHeldToolWaitsForThePerson()
    {
        (Conversation c, FakeService service) = Make();
        HoldingEditor editor = new() { Holds = input => !input.Contains("\"look\"", StringComparison.Ordinal) };
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"look\"}"), Use("toolu_2", "run_command", "{\"name\":\"a\"}"),
                              Use("toolu_3", "run_command", "{\"name\":\"b\"}")));
        service.Reply(Message("end_turn", Text("Done.")));
        c.Send("do things");
        // What the editor does not hold runs at once; the first it holds waits, and nothing after it runs.
        Check(Frames(c, editor, () => editor.Held != null), "the editor holds the first change");
        Check(editor.Ran.Count == 1 && editor.Held == "{\"name\":\"a\"}", "what only looks ran: " + string.Join(" | ", editor.Ran));
        int asked = editor.Asked;
        for (int i = 0; i < 50; ++i)
        {
            Check(!c.RunPending(editor), "nothing runs while the person has not answered");
        }
        Check(editor.Asked == asked + 50 && editor.Ran.Count == 1 && c.Busy && service.Requests == 1, "the editor is asked each turn, and Claude waits");
        Check(Kinds(c) == "you tool result tool", "the held tool is shown, with no result yet: " + Kinds(c));
        // Allowed: it runs, and its result goes back as any other's.
        editor.Allowed = true;
        editor.Answered = true;
        Check(c.RunPending(editor) && editor.Ran.Count == 2 && editor.Held == null, "allowed, it runs on the next turn");
        // The next change is asked about on its own; not allowed, it is not run and Claude is told.
        Check(Frames(c, editor, () => editor.Held != null) && editor.Held == "{\"name\":\"b\"}", "the next change waits too");
        editor.Allowed = false;
        editor.Answered = true;
        Check(Frames(c, editor, () => !c.Busy), "the work ends");
        Check(editor.Ran.Count == 2 && editor.Dropped == 0, "what was not allowed did not run");
        Check(Kinds(c) == "you tool result tool result tool result claude", "the lines: " + Kinds(c));
        Check(Lines(c)[6].Ok == false && Lines(c)[6].Text.Contains("did not allow", StringComparison.Ordinal), "the refusal is shown as one");
        JsonElement results = service.Request(1).GetProperty("messages")[2].GetProperty("content");
        Check(results.GetArrayLength() == 3 && !(results[1].TryGetProperty("is_error", out JsonElement e) && e.GetBoolean()) &&
              results[2].GetProperty("is_error").GetBoolean() && results[2].GetProperty("content").ToString().Contains("did not allow", StringComparison.Ordinal),
              "Claude is told which ran and which the person did not allow");
    }

    private static void AHeldToolIsWithdrawn()
    {
        // Stopped while the person is asked: not run, the question withdrawn, the conversation whole.
        (Conversation c, FakeService service) = Make();
        HoldingEditor editor = new();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}"), Use("toolu_2", "run_command", "{\"name\":\"b\"}")));
        c.Send("do two things");
        Check(Frames(c, editor, () => editor.Held != null), "the editor holds the first");
        Check(c.Stop(), "there was work to stop");
        Check(Until(() => !c.Busy), "it stops while the person had not answered");
        Check(editor.Dropped == 0 && editor.Held != null, "the editor is told on its next turn");
        // (Answered at that very moment: stopped all the same.)
        editor.Allowed = true;
        editor.Answered = true;
        c.RunPending(editor);
        Check(editor.Dropped == 1 && editor.Held == null && editor.Ran.Count == 0, "the question is withdrawn and nothing ran");
        Check(!c.RunPending(editor) && editor.Dropped == 1, "once");
        Check(Kinds(c) == "you tool note" && Lines(c)[2].Text == "Stopped.", "the person is told: " + Kinds(c));
        service.Reply(Message("end_turn", Text("Ok.")));
        c.Send("carry on");
        Check(Frames(c, editor, () => !c.Busy), "the next message works");
        JsonElement results = service.Request(1).GetProperty("messages")[2].GetProperty("content");
        Check(results.GetArrayLength() == 2 && results[0].GetProperty("is_error").GetBoolean() && results[1].GetProperty("is_error").GetBoolean() &&
              results[0].GetProperty("content").ToString().Contains("stopped", StringComparison.Ordinal), "both calls answered as not run");

        // Cleared while the person is asked: the same, and the new conversation's tools run.
        (c, service) = Make();
        editor = new HoldingEditor();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        c.Send("do a thing");
        Check(Frames(c, editor, () => editor.Held != null), "the editor holds it");
        c.Clear();
        service.Reply(Message("tool_use", Use("toolu_2", "run_command", "{\"name\":\"fresh\"}")));
        service.Reply(Message("end_turn", Text("Fresh.")));
        editor.Holds = _ => false;
        Check(Until(() => !c.Working), "the cleared work ends");
        c.Send("start again");
        Check(Frames(c, editor, () => !c.Busy), "the new conversation works");
        Check(editor.Dropped == 1 && editor.Ran.Count == 1 && editor.Ran[0] == "{\"name\":\"fresh\"}", "the old question was withdrawn, never run: " + string.Join(" | ", editor.Ran));
        Check(Kinds(c) == "note you tool result claude", "the new conversation's lines: " + Kinds(c));

        // Stopped while the editor is looking at the tool (before it says it holds it): withdrawn at once.
        (c, service) = Make();
        StoppingEditor stopping = new(c);
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        c.Send("do a thing");
        Check(Frames(c, stopping, () => !c.Busy), "the work ends");
        Check(stopping.Dropped == 1 && Kinds(c) == "you tool note", "the question is withdrawn as it is put: " + Kinds(c));
    }

    // An editor whose person stops the work while it decides to hold a tool.
    private sealed class StoppingEditor(Conversation conversation) : IToolRunner
    {
        public int Dropped;

        public ToolReply? Run(string name, string input)
        {
            conversation.Stop();
            return null;
        }

        public bool HeldAnswered() => false;
        public ToolReply FinishHeld() => new(false, "not run");
        public void DropHeld() => ++Dropped;
    }

    // The blocks of the tool result for `id` in a request's message, or its text when it is words alone.
    private static JsonElement ResultOf(JsonElement message, string id)
    {
        foreach (JsonElement result in message.GetProperty("content").EnumerateArray())
        {
            if (result.GetProperty("tool_use_id").GetString() == id)
            {
                return result.GetProperty("content");
            }
        }
        throw new InvalidOperationException("no result for " + id);
    }

    private static int Pictures(JsonElement content)
    {
        int count = 0;
        if (content.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement block in content.EnumerateArray())
            {
                count += block.GetProperty("type").GetString() == "image" ? 1 : 0;
            }
        }
        return count;
    }

    private static void AToolsPicturesGoToClaude()
    {
        (Conversation c, FakeService service) = Make();
        byte[] png = [0x89, 0x50, 0x4E, 0x47, 1, 2, 3];
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"look\"}"), Use("toolu_2", "run_command", "{\"name\":\"words\"}"),
                              Use("toolu_3", "run_command", "{\"name\":\"two\"}")));
        service.Reply(Message("end_turn", Text("I see it.")));
        c.Send("look at it");
        Check(Finish(c, (_, input) => input.Contains("look", StringComparison.Ordinal) ? new ToolReply(true, "{\"width\":4}", [new ToolImage(png, "image/png")])
                                    : input.Contains("two", StringComparison.Ordinal) ? new ToolReply(true, "", [new ToolImage(png, "image/png"), new ToolImage([9, 8], "image/jpeg")])
                                    : new ToolReply(false, "failed", [new ToolImage(png, "image/png")])), "the work ends");
        JsonElement results = service.Request(1).GetProperty("messages")[2];
        JsonElement looked = ResultOf(results, "toolu_1");
        Check(looked.ValueKind == JsonValueKind.Array && looked.GetArrayLength() == 2, "the result and its picture: " + looked);
        Check(looked[0].GetProperty("type").GetString() == "text" && looked[0].GetProperty("text").GetString() == "{\"width\":4}", "the result's text first");
        JsonElement source = looked[1].GetProperty("source");
        Check(looked[1].GetProperty("type").GetString() == "image" && source.GetProperty("type").GetString() == "base64" &&
              source.GetProperty("media_type").GetString() == "image/png" && source.GetProperty("data").GetString() == Convert.ToBase64String(png),
              "then the picture, whole: " + looked[1]);
        // A failure goes back as words alone, whatever came with it (the service takes no picture with one).
        JsonElement failed = ResultOf(results, "toolu_2");
        Check(failed.ValueKind == JsonValueKind.String && failed.GetString() == "failed" && results.GetProperty("content")[1].GetProperty("is_error").GetBoolean(),
              "a failure's pictures are not passed on: " + failed);
        // Two pictures: both, each with its type; and a text block is never empty.
        JsonElement two = ResultOf(results, "toolu_3");
        Check(Pictures(two) == 2 && two[2].GetProperty("source").GetProperty("media_type").GetString() == "image/jpeg" &&
              two[2].GetProperty("source").GetProperty("data").GetString() == Convert.ToBase64String([9, 8]), "two pictures, each of its type");
        Check(two[0].GetProperty("text").GetString() == "(no text)", "an empty result's text block is not empty");
        // The person reads that a picture came, not its bytes.
        Check(Lines(c)[2].Text == "{\"width\":4} (with a picture)" && Lines(c)[4].Text == "failed" && Lines(c)[6].Text == " (with 2 pictures)",
              "the lines say so: " + Lines(c)[2].Text + " | " + Lines(c)[6].Text);
        using JsonDocument read = JsonDocument.Parse(c.Read(0, 256, true));
        Check(!read.RootElement.GetRawText().Contains(Convert.ToBase64String(png), StringComparison.Ordinal), "and reading the chat gives no picture's bytes");
    }

    private static void ThePicturesOfOneReplyAreBounded()
    {
        // More pictures in one reply's results than a conversation keeps: the first are passed on, the rest said to be left out.
        (Conversation c, FakeService service) = Make();
        List<string> uses = [];
        for (int i = 0; i < 4; ++i)
        {
            uses.Add(Use("toolu_" + i, "run_command", "{\"name\":\"look\"}"));
        }
        service.Reply(Message("tool_use", [.. uses]));
        service.Reply(Message("end_turn", Text("Seen.")));
        c.Send("look four times at once");
        Check(Finish(c, (_, _) => new ToolReply(true, "two", [new ToolImage([1], "image/png"), new ToolImage([2], "image/png")])), "the work ends");
        JsonElement results = service.Request(1).GetProperty("messages")[2];
        int passed = 0;
        for (int i = 0; i < 4; ++i)
        {
            passed += Pictures(ResultOf(results, "toolu_" + i));
        }
        Check(passed == Conversation.MostImages, "as many as a conversation keeps: " + passed);
        JsonElement last = ResultOf(results, "toolu_3");
        Check(last.ValueKind == JsonValueKind.String && last.GetString() == "two\n" + Conversation.ImagesLeftOut, "the rest are said to be left out: " + last);
        Check(Lines(c)[^2].Text == "two", "and the chat's line claims no picture for it: " + Lines(c)[^2].Text);
        // By size too.
        (c, service) = Make();
        service.Reply(Message("tool_use", Use("toolu_a", "run_command", "{\"name\":\"look\"}")));
        service.Reply(Message("end_turn", Text("Seen.")));
        c.Send("look once");
        byte[] huge = new byte[Conversation.MostImageBytes + 1];
        Check(Finish(c, (_, _) => new ToolReply(true, "huge", [new ToolImage(huge, "image/png"), new ToolImage([1], "image/png")])), "the work ends");
        JsonElement one = ResultOf(service.Request(1).GetProperty("messages")[2], "toolu_a");
        Check(Pictures(one) == 1 && one[0].GetProperty("text").GetString() == "huge\n" + Conversation.ImagesLeftOut && one[1].GetProperty("source").GetProperty("data").GetString() == Convert.ToBase64String([1]),
              "one too large for a conversation is left out, the small one beside it passed on");
    }

    private static void AnErrorOfTheServiceTakesThePicturesOut()
    {
        // A picture the service will not take would fail every later message: after an error none is kept.
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use", "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-1\"}",
                              Use("toolu_1", "run_command", "{\"name\":\"look\"}"), Use("toolu_2", "run_command", "{\"name\":\"words\"}")));
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Could not process image\"}}", HttpStatusCode.BadRequest);
        c.Send("look");
        Check(Finish(c, (_, input) => input.Contains("look", StringComparison.Ordinal) ? new ToolReply(true, "shot", [new ToolImage([1, 2, 3], "image/png")]) : new ToolReply(false, "no")),
              "the work ends");
        Check(Pictures(ResultOf(service.Request(1).GetProperty("messages")[2], "toolu_1")) == 1, "the picture went out");
        Check(Lines(c)[^1].Kind == "error" && Lines(c)[^1].Text.Contains("pictures were taken out", StringComparison.Ordinal), "the person is told: " + Lines(c)[^1].Text);
        service.Reply(Message("end_turn", Text("Without it, then.")));
        c.Send("go on");
        Check(Finish(c) && Lines(c)[^1].Text == "Without it, then.", "the next message is answered");
        JsonElement messages = service.Request(2).GetProperty("messages");
        Check(messages[1].GetProperty("content")[0].GetProperty("type").GetString() == "tool_use", "Claude's thinking before it is taken out with the picture");
        JsonElement again = ResultOf(messages[2], "toolu_1");
        Check(messages.GetArrayLength() == 4 && again.ValueKind == JsonValueKind.String && again.GetString() == "shot\n" + Conversation.ImageGone,
              "the result is sent again as words: " + again);
        Check(ResultOf(messages[2], "toolu_2").GetString() == "no" && messages[2].GetProperty("content")[1].GetProperty("is_error").GetBoolean(),
              "the result beside it as it was");
        // An error with no picture kept says nothing of pictures.
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"bad\"}}", HttpStatusCode.BadRequest);
        c.Send("once more");
        Check(Finish(c) && Lines(c)[^1].Kind == "error" && !Lines(c)[^1].Text.Contains("pictures", StringComparison.Ordinal), "no pictures, no word of them");
    }

    private static void PicturesAfterADeclinedMessage()
    {
        // A declined first reply takes the person's message back out: the places of the picture messages after it still hold.
        (Conversation c, FakeService service) = Make();
        int taken = 0;
        Func<string, string, ToolReply> look = (_, _) => new ToolReply(true, "shot " + taken, [new ToolImage([(byte)taken++], "image/png")]);
        service.Reply(Message("tool_use", Use("toolu_0", "run_command", "{\"name\":\"look\"}")));
        service.Reply(Message("end_turn", Text("Seen.")));
        c.Send("look");
        Check(Finish(c, look), "the first look");
        service.Reply(Message("refusal"));
        c.Send("something declined");
        Check(Finish(c, look) && Lines(c)[^1].Kind == "error", "declined, and taken back");
        for (int i = 1; i <= Conversation.MostImages; ++i)
        {
            service.Reply(Message("tool_use", Use("toolu_" + i, "run_command", "{\"name\":\"look\"}")));
        }
        service.Reply(Message("end_turn", Text("Seen all.")));
        c.Send("look many times");
        Check(Finish(c, look), "the later looks");
        JsonElement messages = service.Request(service.Requests - 1).GetProperty("messages");
        // user, assistant, results 0, assistant "Seen.", user, then assistant and results for each later look.
        Check(messages[4].GetProperty("role").GetString() == "user" && messages.GetArrayLength() == 5 + 2 * Conversation.MostImages, "the declined message is not there: " + messages.GetArrayLength());
        int half = Conversation.MostImages / 2;
        for (int i = 0; i <= Conversation.MostImages; ++i)
        {
            JsonElement content = ResultOf(messages[i == 0 ? 2 : 4 + 2 * i], "toolu_" + i);
            bool gone = i <= Conversation.MostImages - half;
            Check(gone ? content.ValueKind == JsonValueKind.String && content.GetString() == "shot " + i + "\n" + Conversation.ImageGone
                       : Pictures(content) == 1 && content[0].GetProperty("text").GetString() == "shot " + i,
                  "each result is in its place, " + (gone ? "let go" : "kept") + ": " + i);
        }
    }

    private static void OldPicturesAreLetGo()
    {
        (Conversation c, FakeService service) = Make();
        int looks = Conversation.MostImages + 2;
        for (int i = 0; i < looks; ++i)
        {
            service.Reply(Message("tool_use", "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-" + i + "\"}",
                                  Use("toolu_" + i, "run_command", "{\"name\":\"look\"}")));
        }
        service.Reply(Message("end_turn", Text("Seen.")));
        c.Send("look often");
        int taken = 0;
        Check(Finish(c, (_, _) => new ToolReply(true, "shot " + taken, [new ToolImage([(byte)taken++, 1, 2], "image/png")])), "the work ends");
        JsonElement messages = service.Request(service.Requests - 1).GetProperty("messages");
        int kept = 0;
        for (int i = 0; i < looks; ++i)
        {
            JsonElement content = ResultOf(messages[2 + 2 * i], "toolu_" + i);
            bool gone = i < 4; // (at the seventh, down to half: the first four went together)
            if (gone)
            {
                Check(content.ValueKind == JsonValueKind.String && content.GetString() == "shot " + i + "\n" + Conversation.ImageGone,
                      "the oldest are words again, saying a picture was there: " + content);
            }
            else
            {
                Check(Pictures(content) == 1 && content[0].GetProperty("text").GetString() == "shot " + i, "the newest keep theirs: " + i);
                ++kept;
            }
        }
        Check(kept == 4 && Conversation.MostImages == 6, "half were left at the seventh, and the eighth came after: " + kept);
        // While there was room, nothing was rewritten (what was sent before is sent again as it was).
        JsonElement early = service.Request(Conversation.MostImages).GetProperty("messages");
        Check(Pictures(ResultOf(early[2], "toolu_0")) == 1, "the first was still there while there was room");
        Check(early[1].GetProperty("content")[0].GetProperty("signature").GetString() == "sig-0", "and Claude's thinking with it");
        // A message written again would make every later thinking block one the service refuses (each is
        // signed against all before it): the thinking up to then is taken out, whole; what comes after stays.
        for (int i = 0; i < looks; ++i)
        {
            JsonElement said = messages[1 + 2 * i].GetProperty("content");
            bool thought = said[0].GetProperty("type").GetString() == "thinking";
            Check(thought == (i == looks - 1) && said[said.GetArrayLength() - 1].GetProperty("id").GetString() == "toolu_" + i,
                  "reply " + i + (i == looks - 1 ? " keeps its thinking" : " is sent without its thinking") + ", with its tool call");
        }

        // By size too: the newest results keep theirs (Claude has yet to see them).
        (c, service) = Make();
        service.Reply(Message("tool_use", Use("toolu_a", "run_command", "{\"name\":\"look\"}")));
        service.Reply(Message("tool_use", Use("toolu_b", "run_command", "{\"name\":\"look\"}"), Use("toolu_w", "run_command", "{\"name\":\"words\"}")));
        service.Reply(Message("end_turn", Text("Seen.")));
        c.Send("look twice");
        byte[] large = new byte[Conversation.MostImageBytes / 2 + 1];
        Check(Finish(c, (_, input) => input.Contains("words", StringComparison.Ordinal) ? new ToolReply(true, "said") : new ToolReply(true, "big", [new ToolImage(large, "image/png")])),
              "the work ends");
        messages = service.Request(2).GetProperty("messages");
        Check(ResultOf(messages[2], "toolu_a").ValueKind == JsonValueKind.String && Pictures(ResultOf(messages[4], "toolu_b")) == 1,
              "two that are too much together: the older goes");
        Check(ResultOf(messages[4], "toolu_w").GetString() == "said" && messages[4].GetProperty("content").GetArrayLength() == 2, "the results beside it are as they were");
    }

    private static void AToolBeingRunIsNotStopped()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}"), Use("toolu_2", "run_command", "{\"name\":\"b\"}")));
        List<string> ran = [];
        c.Send("do two things");
        // The person stops the work while the first command runs: it ran, and is said to have.
        Check(Finish(c, (_, input) => { ran.Add(input); c.Stop(); return new ToolReply(true, "{\"done\":1}"); }), "the work ends");
        Check(ran.Count == 1 && ran[0] == "{\"name\":\"a\"}", "the first ran, the second did not: " + string.Join(" | ", ran));
        Check(Kinds(c) == "you tool result note" && Lines(c)[2].Ok == true, "its true result is shown: " + Kinds(c));
        service.Reply(Message("end_turn", Text("Ok.")));
        c.Send("carry on");
        Check(Finish(c), "the next message works");
        JsonElement results = service.Request(1).GetProperty("messages")[2].GetProperty("content");
        Check(results.GetArrayLength() == 2 && results[0].GetProperty("content").ToString().Contains("done", StringComparison.Ordinal) &&
              !(results[0].TryGetProperty("is_error", out JsonElement e) && e.GetBoolean()) && results[1].GetProperty("is_error").GetBoolean(),
              "Claude is told the first was done and the second was not");
    }

    private static void AnotherModelTakingOver()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use",
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-first\"}",
            Text("Let me "),
            "{\"type\":\"fallback\",\"from\":{\"model\":\"claude-opus-5-5\"},\"to\":{\"model\":\"claude-opus-5\"},\"trigger\":{\"type\":\"refusal\"}}",
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-second\"}",
            Text("look."),
            Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        service.Reply(Message("end_turn", Text("Done.")));
        c.Send("something");
        Check(Finish(c), "the work ends");
        Check(Lines(c)[1].Text == "Let me look.", "the words of both models are shown: " + Lines(c)[1].Text);
        JsonElement said = service.Request(1).GetProperty("messages")[1].GetProperty("content");
        List<string> types = [];
        foreach (JsonElement block in said.EnumerateArray())
        {
            types.Add(block.GetProperty("type").GetString()!);
        }
        Check(string.Join(" ", types) == "text fallback thinking text tool_use", "the first model's thinking is not handed back, the mark stays in place: " + string.Join(" ", types));
        Check(said[1].GetProperty("from").GetProperty("model").GetString() == "claude-opus-5-5" && said[1].GetProperty("to").GetProperty("model").GetString() == "claude-opus-5" &&
              said[2].GetProperty("signature").GetString() == "sig-second", "with who handed over to whom");
    }

    private static void TheStepsOfAMessageAreBounded()
    {
        (Conversation c, FakeService service) = Make();
        service.Always = Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}"));
        c.Send("loop");
        Check(Finish(c), "the work ends");
        Check(service.Requests == Conversation.MostSteps, "after the most steps: " + service.Requests);
        var lines = Lines(c, 0, 256);
        Check(lines[^1].Kind == "note" && lines[^1].Text.Contains("go on", StringComparison.Ordinal), "and the person is told how to let it go on");
        service.Always = null;
        service.Reply(Message("end_turn", Text("Finished.")));
        c.Send("go on");
        Check(Finish(c) && Lines(c, 0, 256)[^1].Text == "Finished.", "which it then does");
        JsonElement messages = service.Request(service.Requests - 1).GetProperty("messages");
        int count = messages.GetArrayLength();
        Check(count == 2 + 2 * Conversation.MostSteps && messages[count - 2].GetProperty("content")[0].GetProperty("type").GetString() == "tool_result" &&
              messages[count - 3].GetProperty("role").GetString() == "assistant", "every call of the forty answered before the new message: " + count);
    }

    private static void ClearingStartsAfresh()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        c.Send("first");
        Check(Until(() => Kinds(c) == "you tool"), "Claude waits for a tool");
        c.Clear();
        Check(!c.Busy, "a cleared conversation is idle at once");
        int ran = 0;
        Check(!c.RunPending((_, _) => { ++ran; return new ToolReply(true, "{}"); }) && ran == 0, "the old conversation's tool is not run");
        service.Reply(Message("end_turn", Text("New.")));
        Check(c.Send("second") == null && Finish(c), "a new message is answered");
        Check(Kinds(c) == "note you claude", "only the new lines are kept: " + Kinds(c));
        JsonElement messages = service.Request(service.Requests - 1).GetProperty("messages");
        Check(messages.GetArrayLength() == 1 && messages[0].GetProperty("content").ToString().Contains("second", StringComparison.Ordinal), "and only the new message is sent");
        Check(Until(() => !c.Working), "the old conversation's work ends");
        Check(Kinds(c) == "note you claude", "the old work adds nothing: " + Kinds(c));
        using JsonDocument read = JsonDocument.Parse(c.Read(0, 1, true));
        Check(read.RootElement.GetProperty("input_tokens").GetInt64() == 10 && read.RootElement.GetProperty("output_tokens").GetInt64() == 5,
              "the tokens counted are the new conversation's alone");
    }

    private static void TheServicesErrorsAreSaid()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"no\"}}", HttpStatusCode.Unauthorized);
        c.Send("hi");
        Check(Finish(c) && Kinds(c) == "you error" && Lines(c)[1].Text.Contains("did not accept the key", StringComparison.Ordinal), "a refused key: " + Lines(c)[^1].Text);
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"no\"}}", HttpStatusCode.TooManyRequests);
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Kind == "error" && Lines(c)[^1].Text.Contains("rate limit", StringComparison.Ordinal), "a rate limit: " + Lines(c)[^1].Text);
        // The service not answering in time is not the person stopping the work.
        service.Throw = new TaskCanceledException("timed out", new TimeoutException());
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Kind == "error" && Lines(c)[^1].Text.Contains("too long", StringComparison.Ordinal), "no answer in time: " + Lines(c)[^1].Text);
        service.Throw = new HttpRequestException("no route to host");
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Kind == "error" && Lines(c)[^1].Text.Contains("could not be reached", StringComparison.Ordinal), "no connection: " + Lines(c)[^1].Text);
        // An error after a tool ran: the call and its answer were kept together.
        service.Reply(Message("tool_use", Use("toolu_9", "run_command", "{\"name\":\"a\"}")));
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"boom\"}}", HttpStatusCode.InternalServerError);
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Kind == "error", "an error part way through");
        foreach ((string kind, string text, _, _) in Lines(c))
        {
            Check(!text.Contains("test-key", StringComparison.Ordinal), "the key itself is never shown (" + kind + ")");
        }
        service.Reply("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", (HttpStatusCode)529);
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Kind == "error", "an overloaded service: " + Lines(c)[^1].Text);
        service.Reply(Message("end_turn", Text("Back.")));
        c.Send("hi");
        Check(Finish(c) && Lines(c)[^1].Text == "Back.", "and the conversation goes on after");
        JsonElement messages = service.Request(service.Requests - 1).GetProperty("messages");
        int count = messages.GetArrayLength();
        bool answered = false;
        for (int i = 0; i + 1 < count; ++i)
        {
            answered |= messages[i].GetProperty("role").GetString() == "assistant" && messages[i].GetProperty("content")[0].GetProperty("id").GetString() == "toolu_9" &&
                        messages[i + 1].GetProperty("content")[0].GetProperty("tool_use_id").GetString() == "toolu_9";
        }
        Check(answered, "with the tool's call still followed by its answer");
    }

    private static void LongResultsAreCut()
    {
        (Conversation c, FakeService service) = Make();
        service.Reply(Message("tool_use", Use("toolu_1", "run_command", "{\"name\":\"a\"}")));
        service.Reply(Message("end_turn", Text("Ok.")));
        c.Send("list everything");
        Check(Finish(c, (_, _) => new ToolReply(true, new string('x', Conversation.MostResultChars + 500))), "the work ends");
        string given = service.Request(1).GetProperty("messages")[2].GetProperty("content")[0].GetProperty("content").ToString();
        Check(given.Length < Conversation.MostResultChars + 200 && given.Contains("(cut", StringComparison.Ordinal), "Claude gets the start, and is told it was cut: " + given.Length);
        Check(Lines(c)[2].Text.Length <= Conversation.MostShownChars + 2, "the line kept for reading is shorter still");
    }

    private static void OnlyTheNewestLinesAreKept()
    {
        (Conversation c, FakeService service) = Make();
        service.Always = Message("end_turn", Text("yes"));
        int rounds = Conversation.MostEntries / 2 + 5;
        for (int i = 0; i < rounds; ++i)
        {
            c.Send("q");
            if (!Finish(c))
            {
                Check(false, "answered");
                return;
            }
        }
        using JsonDocument read = JsonDocument.Parse(c.Read(0, 3, true));
        JsonElement root = read.RootElement;
        Check(root.GetProperty("latest").GetInt64() == 2 * rounds && root.GetProperty("oldest").GetInt64() == 2 * rounds - Conversation.MostEntries + 1, "the oldest kept moves on");
        Check(root.GetProperty("entries")[0].GetProperty("serial").GetInt64() == root.GetProperty("oldest").GetInt64() && root.GetProperty("after").GetInt64() == root.GetProperty("oldest").GetInt64() + 2,
              "reading from before it starts at it");
        c.Clear();
        using JsonDocument cleared = JsonDocument.Parse(c.Read(5, 64, true));
        Check(cleared.RootElement.GetProperty("entries").GetArrayLength() == 1 && cleared.RootElement.GetProperty("oldest").GetInt64() == 2 * rounds + 1 &&
              cleared.RootElement.GetProperty("after").GetInt64() == 2 * rounds + 1, "a reader from before a clear reads on into the new conversation");
    }

    private static void ReadingByCursor()
    {
        (Conversation c, FakeService service) = Make();
        for (int i = 0; i < 3; ++i)
        {
            service.Reply(Message("end_turn", Text("answer " + i)));
            c.Send("question " + i);
            Check(Finish(c), "answered");
        }
        using JsonDocument first = JsonDocument.Parse(c.Read(0, 4, true));
        Check(first.RootElement.GetProperty("entries").GetArrayLength() == 4 && first.RootElement.GetProperty("after").GetInt64() == 4, "the first four");
        using JsonDocument rest = JsonDocument.Parse(c.Read(4, 64, true));
        Check(rest.RootElement.GetProperty("entries").GetArrayLength() == 2 && rest.RootElement.GetProperty("after").GetInt64() == 6 &&
              rest.RootElement.GetProperty("entries")[0].GetProperty("serial").GetInt64() == 5, "then the rest");
        using JsonDocument none = JsonDocument.Parse(c.Read(6, 64, false));
        Check(none.RootElement.GetProperty("entries").GetArrayLength() == 0 && none.RootElement.GetProperty("after").GetInt64() == 6 &&
              none.RootElement.GetProperty("oldest").GetInt64() == 1 && !none.RootElement.GetProperty("key_found").GetBoolean(), "then nothing new");
        using JsonDocument beyond = JsonDocument.Parse(c.Read(1000, 64, true));
        Check(beyond.RootElement.GetProperty("after").GetInt64() == 6, "a cursor past the end comes back to it");
    }

    private static int Main()
    {
        (string, Action)[] tests =
        [
            ("an answer in words", AnAnswerInWords),
            ("tools run on the calling thread, results go back together", ToolsRunOnTheCallingThread),
            ("without a key nothing is sent", WithoutAKeyNothingIsSent),
            ("one message at a time", OneMessageAtATime),
            ("a declined request is not kept", ADeclinedRequestIsNotKept),
            ("a cut off reply runs nothing", ACutOffReplyRunsNothing),
            ("replies that cannot be kept", RepliesThatCannotBeKept),
            ("only the newest lines are kept", OnlyTheNewestLinesAreKept),
            ("stopping leaves the conversation whole", StoppingLeavesTheConversationWhole),
            ("a tool being run is not stopped", AToolBeingRunIsNotStopped),
            ("another model taking over", AnotherModelTakingOver),
            ("the steps of a message are bounded", TheStepsOfAMessageAreBounded),
            ("clearing starts afresh", ClearingStartsAfresh),
            ("a tool the editor holds waits for the person", AHeldToolWaitsForThePerson),
            ("a held tool can be stopped or cleared away", AHeldToolIsWithdrawn),
            ("a tool's pictures go to Claude with its result", AToolsPicturesGoToClaude),
            ("old pictures are let go", OldPicturesAreLetGo),
            ("the pictures of one reply are bounded", ThePicturesOfOneReplyAreBounded),
            ("an error of the service takes the pictures out", AnErrorOfTheServiceTakesThePicturesOut),
            ("pictures after a declined message", PicturesAfterADeclinedMessage),
            ("the service's errors are said", TheServicesErrorsAreSaid),
            ("long results are cut", LongResultsAreCut),
            ("reading by cursor", ReadingByCursor),
        ];
        foreach ((string name, Action test) in tests)
        {
            int before = s_failed;
            try
            {
                test();
            }
            catch (Exception e)
            {
                ++s_failed;
                Console.WriteLine("FAILED: " + name + " threw " + e);
            }
            Console.WriteLine((s_failed == before ? "ok      " : "FAILED  ") + name);
        }
        Console.WriteLine(s_checked + " checks, " + s_failed + " failed");
        return s_failed == 0 ? 0 : 1;
    }
}
