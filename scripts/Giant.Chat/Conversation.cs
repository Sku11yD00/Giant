using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Anthropic;
using Anthropic.Exceptions;
using Anthropic.Models.Beta.Messages;

namespace Giant.Chat;

/// <summary>A picture a tool returned (a screenshot): the file's bytes and its type ("image/png" or "image/jpeg").</summary>
internal sealed record ToolImage(byte[] Data, string MediaType);

/// <summary>What a tool answered: the result, or why it failed; and the pictures that came with it, for Claude to look at.</summary>
internal readonly record struct ToolReply(bool Ok, string Text, IReadOnlyList<ToolImage>? Images = null);

/// <summary>
/// The editor's side of Claude's tools, on its main thread. The editor may hold a tool until the
/// person has answered whether it may run (the editor's "ask before changes"): one at a time.
/// </summary>
internal interface IToolRunner
{
    /// <summary>Runs a tool. Null when the editor holds it instead, until the person has answered.</summary>
    ToolReply? Run(string name, string input);
    /// <summary>Whether the person has answered about the tool held.</summary>
    bool HeldAnswered();
    /// <summary>The held tool's reply, once answered: run if the person allowed it, else that they did not.</summary>
    ToolReply FinishHeld();
    /// <summary>The held tool is withdrawn (the work was stopped): the person is no longer asked.</summary>
    void DropHeld();
}

/// <summary>A tool Claude may call: its name, what it is for, and its arguments' JSON Schema.</summary>
internal sealed record ToolSpec(string Name, string Description, JsonElement Schema);

/// <summary>
/// One conversation with Claude. The person's message goes out with what
/// was said before; Claude answers in words or asks for tools, which the
/// editor runs on its main thread (<see cref="RunPending"/>) and whose
/// results go back, until Claude has nothing more to do.
///
/// The requests run on pool threads; everything the editor calls returns
/// at once.
/// </summary>
internal sealed class Conversation
{
    public const string Model = "claude-opus-5-5";
    /// <summary>Requests one message of the person's may take (then Claude is asked to be told to go on).</summary>
    public const int MostSteps = 40;
    /// <summary>Lines kept for reading; older ones are dropped.</summary>
    public const int MostEntries = 2000;
    /// <summary>Characters of a tool's result Claude is given (the rest is cut, and said to be).</summary>
    public const int MostResultChars = 60000;
    /// <summary>Characters of a tool's result or arguments kept for reading.</summary>
    public const int MostShownChars = 2000;
    private const int MostTokens = 16000;
    /// <summary>
    /// Pictures the conversation keeps for Claude (every request carries them all again), and the most
    /// one reply's tools may bring together. When a reply's results take the conversation over either,
    /// the oldest results' pictures are let go, down to half (so that it is not done at every look).
    /// </summary>
    public const int MostImages = 6;
    public const long MostImageBytes = 8_000_000;
    /// <summary>What stands in a result for a picture no longer kept.</summary>
    public const string ImageGone = "(A picture came with this result. It is no longer kept: take another if you need to look again.)";
    /// <summary>What a result says of pictures that were more than one reply's tools may bring.</summary>
    public const string ImagesLeftOut = "(Not every picture of this result was passed on: too many, or too large, for one step. Take fewer at once.)";

    /// <summary>A line of the conversation as the person reads it.</summary>
    private sealed record Entry(long Serial, string Kind, string Text, string? Tool, bool Ok);

    private sealed class Waiting(string name, string input, CancellationToken stop)
    {
        public readonly string Name = name;
        public readonly string Input = input;
        public readonly CancellationToken Stop = stop; // the stop of the work it is part of
        public bool Taken; // the main thread is running it (or has): it is no longer stopped
        public readonly TaskCompletionSource<ToolReply> Done = new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    // One tool's answer as it is handed back to Claude. `Images` only until the message is made of
    // it (the message holds them from then on); `Pictures` and `Bytes` tell what it carries there.
    private sealed record Answered(string ID, string Text, bool IsError, IReadOnlyList<ToolImage>? Images, int Pictures = 0, long Bytes = 0);

    // A message of tool results that carries pictures: where it is, and what it was made from.
    private sealed class Pictured(int index, List<Answered> answers)
    {
        public readonly int Index = index;
        public readonly List<Answered> Answers = answers;
    }

    /// <summary>What Claude is sent, and the work on it; replaced whole when the conversation is cleared.</summary>
    private sealed class Talk
    {
        public readonly List<BetaMessageParam> Messages = [];
        public readonly List<Pictured> Pictured = []; // oldest first
        // Claude's replies that hold its thinking (by their place in Messages): each as it is without.
        public readonly Dictionary<int, List<BetaContentBlockParam>> Unthought = [];
        public readonly Queue<Waiting> Tools = new();
        public CancellationTokenSource? Stop;
        public bool Busy;
        public bool Stopping; // the person stopped the work on the last message (until it has ended)
    }

    private readonly Lock _lock = new();
    private readonly Func<AnthropicClient?> _connect;
    private readonly List<BetaToolUnion> _tools = [];
    private readonly string _system;
    private readonly List<Entry> _entries = [];
    private Talk _talk = new();
    private Waiting? _held; // the tool the editor holds until the person has answered (of this talk, or one cleared since)
    private long _serial;
    private long _inputTokens;
    private long _outputTokens;
    private int _working; // messages being worked on, a cleared conversation's last included

    /// <summary>Whether any work is still ending (a stopped or cleared conversation's too): for tests.</summary>
    internal bool Working => Volatile.Read(ref _working) != 0;

    /// <param name="connect">The client, or null while there is no key.</param>
    public Conversation(Func<AnthropicClient?> connect, IReadOnlyList<ToolSpec> tools, string system)
    {
        _connect = connect;
        _system = system;
        foreach (ToolSpec tool in tools)
        {
            Dictionary<string, JsonElement> properties = [];
            List<string> required = [];
            if (tool.Schema.ValueKind == JsonValueKind.Object)
            {
                if (tool.Schema.TryGetProperty("properties", out JsonElement all) && all.ValueKind == JsonValueKind.Object)
                {
                    foreach (JsonProperty property in all.EnumerateObject())
                    {
                        properties[property.Name] = property.Value.Clone();
                    }
                }
                if (tool.Schema.TryGetProperty("required", out JsonElement needed) && needed.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement name in needed.EnumerateArray())
                    {
                        if (name.GetString() is { } text)
                        {
                            required.Add(text);
                        }
                    }
                }
            }
            _tools.Add(new BetaTool
            {
                Name = tool.Name,
                Description = tool.Description,
                InputSchema = new() { Properties = properties, Required = required },
            });
        }
    }

    public bool Busy
    {
        get
        {
            lock (_lock)
            {
                return _talk.Busy;
            }
        }
    }

    /// <summary>
    /// A number that changes whenever the conversation does (a line added, work begun or
    /// ended): its lowest bit is whether Claude is working, the rest the newest line's serial.
    /// </summary>
    public int Stamp
    {
        get
        {
            lock (_lock)
            {
                return (int)((_serial & 0x3fffffff) << 1) | (_talk.Busy ? 1 : 0);
            }
        }
    }

    /// <summary>Sends what the person wrote. Null when it went out, else why not.</summary>
    public string? Send(string text) => Send(text, out _);

    /// <summary>The same; <paramref name="serial"/> is the line the message became (read on from it).</summary>
    public string? Send(string text, out long serial)
    {
        serial = 0;
        if (string.IsNullOrWhiteSpace(text))
        {
            return "there is nothing to send";
        }
        Talk talk;
        CancellationToken stop;
        AnthropicClient? client;
        lock (_lock)
        {
            talk = _talk;
            if (talk.Busy)
            {
                return talk.Stopping
                    ? "Claude is still stopping its work on the last message: try again in a moment (get_chat's `busy` says when)"
                    : "Claude is still working on the last message (stop_chat stops it)";
            }
            client = _connect();
            if (client == null)
            {
                return Keys.HowToSet();
            }
            talk.Busy = true;
            talk.Stop = new CancellationTokenSource();
            stop = talk.Stop.Token;
            talk.Messages.Add(new BetaUserMessageParam { Content = text });
            Add(talk, "you", text);
            serial = _serial;
        }
        _ = Task.Run(() => RunAsync(client, talk, stop), CancellationToken.None);
        return null;
    }

    /// <summary>Stops the work on the last message, if any. True when there was some.</summary>
    public bool Stop()
    {
        lock (_lock)
        {
            if (!_talk.Busy)
            {
                return false;
            }
            _talk.Stopping = true;
            Cancel(_talk);
            return true;
        }
    }

    /// <summary>Forgets the conversation (stopping any work on it): Claude starts fresh.</summary>
    public void Clear()
    {
        lock (_lock)
        {
            // (The new conversation first: whatever stopping the old one does, it is gone.)
            Talk old = _talk;
            _talk = new Talk();
            _entries.Clear();
            _inputTokens = 0;
            _outputTokens = 0;
            Add(_talk, "note", "The conversation was cleared.");
            old.Stopping = true;
            Cancel(old);
        }
    }

    // (Locked by the caller.) What waits on the stop is told at once, here; none of it may break the caller.
    private static void Cancel(Talk talk)
    {
        try
        {
            talk.Stop?.Cancel();
        }
        catch (AggregateException)
        {
        }
    }

    // An editor that holds nothing (tests).
    private sealed class Runs(Func<string, string, ToolReply> run) : IToolRunner
    {
        public ToolReply? Run(string name, string input) => run(name, input);
        public bool HeldAnswered() => true;
        public ToolReply FinishHeld() => new(false, "nothing is held");
        public void DropHeld()
        {
        }
    }

    /// <summary>The same, for an editor that holds nothing.</summary>
    public bool RunPending(Func<string, string, ToolReply> run) => RunPending(new Runs(run));

    /// <summary>
    /// Gives Claude's tools a turn on the calling (main) thread: runs the next one waiting, or, while
    /// the editor holds one for the person's answer, looks whether they have answered (nothing else
    /// runs until then). True when a tool was run or held.
    /// </summary>
    public bool RunPending(IToolRunner editor)
    {
        Waiting? held;
        bool withdrawn = false;
        lock (_lock)
        {
            held = _held;
            if (held != null && held.Done.Task.IsCompleted)
            {
                // Stopped or cleared while the person was asked: not run.
                _held = null;
                held = null;
                withdrawn = true;
            }
        }
        if (withdrawn)
        {
            editor.DropHeld();
        }
        if (held != null)
        {
            if (!editor.HeldAnswered())
            {
                return false;
            }
            lock (_lock)
            {
                _held = null;
                if (held.Done.Task.IsCompleted || held.Stop.IsCancellationRequested)
                {
                    // Stopped just now (or before its wait was set up to hear of it): not run all the same.
                    held.Done.TrySetCanceled(held.Stop);
                    withdrawn = true;
                }
                else
                {
                    held.Taken = true; // from here it runs to its end, as any tool the main thread took
                }
            }
            if (withdrawn)
            {
                editor.DropHeld();
                return false;
            }
            held.Done.TrySetResult(Guarded(() => editor.FinishHeld()) ?? new ToolReply(false, "the editor gave no answer"));
            return true;
        }
        Waiting? next = null;
        lock (_lock)
        {
            while (_talk.Tools.Count > 0)
            {
                Waiting waiting = _talk.Tools.Dequeue();
                if (_talk.Stopping)
                {
                    // (Stopped before its wait was set up to hear of it: not run all the same.)
                    waiting.Done.TrySetCanceled();
                }
                else if (!waiting.Done.Task.IsCompleted) // (not one stopped while it waited)
                {
                    waiting.Taken = true;
                    next = waiting;
                    break;
                }
            }
        }
        if (next == null)
        {
            return false;
        }
        ToolReply? reply = Guarded(() => editor.Run(next.Name, next.Input));
        if (reply is { } done)
        {
            next.Done.TrySetResult(done);
            return true;
        }
        // The editor holds it: the person is asked first. Until they answer it can be stopped
        // again, as while it waited (and it is, if a stop came while the editor looked at it).
        lock (_lock)
        {
            next.Taken = false;
            if (next.Stop.IsCancellationRequested)
            {
                next.Done.TrySetCanceled(next.Stop);
                withdrawn = true;
            }
            else
            {
                _held = next;
            }
        }
        if (withdrawn)
        {
            editor.DropHeld();
        }
        return true;
    }

    // What the editor answered, or that it could not: nothing it throws leaves the turn.
    private static ToolReply? Guarded(Func<ToolReply?> ask)
    {
        try
        {
            return ask();
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            return new ToolReply(false, "the editor could not run it: " + e.Message);
        }
    }

    /// <summary>
    /// The lines after serial <paramref name="after"/>, oldest first, at most
    /// <paramref name="limit"/>, as JSON, with where to read from next.
    /// </summary>
    public string Read(long after, int limit, bool keyFound)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter json = new(stream))
        {
            lock (_lock)
            {
                long cursor = after;
                json.WriteStartObject();
                json.WriteStartArray("entries");
                int written = 0;
                foreach (Entry entry in _entries)
                {
                    if (entry.Serial <= after)
                    {
                        continue;
                    }
                    if (written == limit)
                    {
                        break;
                    }
                    json.WriteStartObject();
                    json.WriteNumber("serial", entry.Serial);
                    json.WriteString("kind", entry.Kind);
                    json.WriteString("text", entry.Text);
                    if (entry.Tool != null)
                    {
                        json.WriteString("tool", entry.Tool);
                    }
                    if (entry.Kind == "result")
                    {
                        json.WriteBoolean("ok", entry.Ok);
                    }
                    json.WriteEndObject();
                    cursor = entry.Serial;
                    ++written;
                }
                json.WriteEndArray();
                json.WriteNumber("after", Math.Min(Math.Max(cursor, 0), _serial));
                json.WriteNumber("latest", _serial);
                json.WriteNumber("oldest", _entries.Count > 0 ? _entries[0].Serial : _serial + 1);
                json.WriteBoolean("busy", _talk.Busy);
                json.WriteBoolean("key_found", keyFound);
                json.WriteString("model", Model);
                json.WriteNumber("input_tokens", _inputTokens);
                json.WriteNumber("output_tokens", _outputTokens);
                json.WriteEndObject();
            }
        }
        return Encoding.UTF8.GetString(stream.GetBuffer(), 0, (int)stream.Length);
    }

    // --- The work on one message (pool threads). ---

    private async Task RunAsync(AnthropicClient client, Talk talk, CancellationToken stop)
    {
        Interlocked.Increment(ref _working);
        try
        {
            for (int step = 0; ; ++step)
            {
                if (step == MostSteps)
                {
                    Note(talk, "note", "Claude stopped after " + MostSteps + " steps. Say \"go on\" to let it continue.");
                    break;
                }
                BetaMessage reply = await client.Beta.Messages.Create(Request(talk), stop).ConfigureAwait(false);
                lock (_lock)
                {
                    if (!ReferenceEquals(talk, _talk))
                    {
                        break; // (cleared meanwhile: this reply is no part of the new conversation, nor of its counts)
                    }
                    _inputTokens += reply.Usage.InputTokens + (reply.Usage.CacheReadInputTokens ?? 0) +
                                    (reply.Usage.CacheCreationInputTokens ?? 0);
                    _outputTokens += reply.Usage.OutputTokens;
                }
                // (Before anything of the reply is used: a declined reply's content is not to be kept.)
                if (reply.StopReason == BetaStopReason.Refusal)
                {
                    if (step == 0)
                    {
                        // The message that was declined is taken back, or it would go out again,
                        // and be declined again, with every later one.
                        lock (_lock)
                        {
                            talk.Messages.RemoveAt(talk.Messages.Count - 1);
                        }
                        Note(talk, "error", "Claude declined to answer this, and the message was taken back: Claude will not remember it." + Why(reply));
                    }
                    else
                    {
                        Note(talk, "error", "Claude declined to go on with this." + Why(reply) +
                                            " If it declines the next message too, clear the chat (clear_chat) and start again.");
                    }
                    break;
                }
                List<BetaContentBlockParam> said = [];
                List<BetaContentBlockParam> unthought = []; // `said` without the thinking
                List<BetaToolUseBlock> asked = [];
                StringBuilder words = new();
                // Where another model took over (the service's fallback), if one did: what the
                // first model thought or asked for before that is not handed back, its words are,
                // and the mark stays where it was.
                int tookOver = -1;
                for (int i = 0; i < reply.Content.Count; ++i)
                {
                    if (reply.Content[i].TryPickFallback(out _))
                    {
                        tookOver = i;
                    }
                }
                for (int i = 0; i < reply.Content.Count; ++i)
                {
                    BetaContentBlock block = reply.Content[i];
                    if (block.TryPickText(out BetaTextBlock? text))
                    {
                        if (!string.IsNullOrWhiteSpace(text.Text)) // (an empty one may not be handed back)
                        {
                            said.Add(new BetaTextBlockParam { Text = text.Text });
                            unthought.Add(said[^1]);
                            words.Append(text.Text);
                        }
                    }
                    else if (block.TryPickFallback(out BetaFallbackBlock? fallback))
                    {
                        said.Add(new BetaFallbackBlockParam
                        {
                            From = new BetaFallbackInfoParam { Model = fallback.From.Model },
                            To = new BetaFallbackInfoParam { Model = fallback.To.Model },
                        });
                        unthought.Add(said[^1]);
                    }
                    else if (i < tookOver)
                    {
                    }
                    else if (block.TryPickThinking(out BetaThinkingBlock? thinking))
                    {
                        // (Handed back as it came: the signature is checked.)
                        said.Add(new BetaThinkingBlockParam { Thinking = thinking.Thinking, Signature = thinking.Signature });
                    }
                    else if (block.TryPickRedactedThinking(out BetaRedactedThinkingBlock? redacted))
                    {
                        said.Add(new BetaRedactedThinkingBlockParam { Data = redacted.Data });
                    }
                    else if (block.TryPickToolUse(out BetaToolUseBlock? use))
                    {
                        said.Add(new BetaToolUseBlockParam { ID = use.ID, Name = use.Name, Input = use.Input });
                        unthought.Add(said[^1]);
                        asked.Add(use);
                    }
                }
                if (words.Length > 0)
                {
                    Note(talk, "claude", words.ToString());
                }
                bool whole = reply.StopReason == BetaStopReason.ToolUse;
                if (asked.Count > 0 && !whole)
                {
                    // Cut short (the length limit, or anything else that is not Claude asking for
                    // its tools): a tool's arguments may be cut short too. None is run, and the
                    // reply is not kept (a call kept must be answered).
                    Note(talk, "error", "Claude's answer was cut off before it finished. Ask for it in smaller parts.");
                    break;
                }
                if (words.Length == 0 && asked.Count == 0)
                {
                    // Nothing to keep (a reply of thoughts alone may not be handed back).
                    Note(talk, "note", "Claude had nothing to say.");
                    break;
                }
                if (asked.Count == 0)
                {
                    lock (_lock)
                    {
                        AddSaid(talk, said, unthought);
                    }
                    if (reply.StopReason == BetaStopReason.MaxTokens)
                    {
                        Note(talk, "note", "Claude's answer reached its length limit. Say \"go on\" for the rest.");
                    }
                    break;
                }
                // Every tool asked for is answered, in one message, or the next request is refused.
                List<Answered> answers = [];
                bool stopped = false;
                int pictures = 0; // of this reply's results together
                long pictureBytes = 0;
                foreach (BetaToolUseBlock use in asked)
                {
                    string input = JsonSerializer.Serialize(use.Input);
                    ToolReply answer;
                    bool ran = false;
                    if (stopped || stop.IsCancellationRequested)
                    {
                        stopped = true;
                        answer = new ToolReply(false, "not run: the person stopped the work");
                    }
                    else
                    {
                        Note(talk, "tool", Shown(input), use.Name);
                        try
                        {
                            answer = await OnMainThread(talk, use.Name, input, stop).ConfigureAwait(false);
                        }
                        catch (OperationCanceledException)
                        {
                            stopped = true;
                            answer = new ToolReply(false, "not run: the person stopped the work");
                        }
                        ran = !stopped;
                    }
                    // The pictures that go with it: a success's only (the service takes none with a
                    // failure), and no more than one reply's results may bring together.
                    List<ToolImage>? kept = null;
                    bool leftOut = false;
                    foreach (ToolImage image in answer.Ok && ran ? answer.Images ?? [] : [])
                    {
                        if (pictures == MostImages || pictureBytes + image.Data.Length > MostImageBytes)
                        {
                            leftOut = true;
                            continue;
                        }
                        ++pictures;
                        pictureBytes += image.Data.Length;
                        (kept ??= []).Add(image);
                    }
                    string given = answer.Text.Length <= MostResultChars
                        ? answer.Text
                        : string.Concat(Start(answer.Text, MostResultChars), "\n(cut: the result was longer; ask for less at once)");
                    if (leftOut)
                    {
                        given += "\n" + ImagesLeftOut;
                    }
                    if (ran)
                    {
                        int count = kept?.Count ?? 0;
                        Note(talk, "result", Shown(answer.Text) + (count == 0 ? "" : count == 1 ? " (with a picture)" : " (with " + count + " pictures)"),
                             use.Name, answer.Ok);
                    }
                    answers.Add(new Answered(use.ID, given, !answer.Ok, kept));
                }
                // (Made here, not under the lock the editor's frame takes: pictures are megabytes to write out.)
                BetaUserMessageParam results = Results(answers);
                bool pictured = false;
                for (int i = 0; i < answers.Count; ++i)
                {
                    if (answers[i].Images is { } images)
                    {
                        long bytes = 0;
                        foreach (ToolImage image in images)
                        {
                            bytes += image.Data.Length;
                        }
                        answers[i] = answers[i] with { Images = null, Pictures = images.Count, Bytes = bytes };
                        pictured = true;
                    }
                }
                // Claude's reply and the answers to it, together or not at all.
                lock (_lock)
                {
                    AddSaid(talk, said, unthought);
                    talk.Messages.Add(results);
                    if (pictured)
                    {
                        talk.Pictured.Add(new Pictured(talk.Messages.Count - 1, answers));
                        LetOldPicturesGo(talk);
                    }
                }
                if (stopped)
                {
                    Note(talk, "note", "Stopped.");
                    break;
                }
            }
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested)
        {
            Note(talk, "note", "Stopped.");
        }
        catch (OperationCanceledException)
        {
            Note(talk, "error", "Claude's service took too long to answer. Try again.");
        }
        catch (AnthropicUnauthorizedException)
        {
            Note(talk, "error", "Claude's service did not accept the key. Check " + Keys.Variable + " (or " + Keys.FilePath() +
                                "); a corrected key file is read with the next message.");
        }
        catch (AnthropicRateLimitException)
        {
            Note(talk, "error", "Claude's service says this key has reached its rate limit for now. Try again in a little while.");
        }
        catch (AnthropicApiException e)
        {
            // (A picture the service will not take would be sent, and refused, with every later
            // message: none is kept past an error that may be about one.)
            bool taken;
            lock (_lock)
            {
                taken = talk.Pictured.Count > 0;
                LetPicturesGo(talk, 0, 0, 0);
            }
            Note(talk, "error", "Claude's service answered with an error: " + e.Message +
                                (taken ? " The pictures were taken out of the conversation, in case one of them was why." : ""));
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            Note(talk, "error", "Claude could not be reached: " + e.Message);
        }
        finally
        {
            lock (_lock)
            {
                talk.Busy = false;
                talk.Stopping = false;
                talk.Stop?.Dispose();
                talk.Stop = null;
            }
            Interlocked.Decrement(ref _working);
        }
    }

    // The message that hands a reply's tool results back: each with its text, and its pictures after it.
    private static BetaUserMessageParam Results(List<Answered> answers)
    {
        List<BetaContentBlockParam> results = [];
        foreach (Answered answer in answers)
        {
            if (answer.Images == null)
            {
                results.Add(new BetaToolResultBlockParam { ToolUseID = answer.ID, Content = answer.Text, IsError = answer.IsError });
                continue;
            }
            // (An empty text block is refused.)
            List<Block> blocks = [new BetaTextBlockParam { Text = answer.Text.Length > 0 ? answer.Text : "(no text)" }];
            foreach (ToolImage image in answer.Images)
            {
                blocks.Add(new BetaImageBlockParam
                {
                    Source = new BetaBase64ImageSource
                    {
                        Data = Convert.ToBase64String(image.Data),
                        MediaType = image.MediaType == "image/jpeg" ? MediaType.ImageJpeg : MediaType.ImagePng,
                    },
                });
            }
            results.Add(new BetaToolResultBlockParam { ToolUseID = answer.ID, Content = blocks, IsError = answer.IsError });
        }
        return new BetaUserMessageParam { Content = results };
    }

    // (Locked by the caller.) Keeps the conversation's pictures within MostImages and MostImageBytes:
    // when they are over, the oldest results that carry any are let go until half of each is left
    // (the newest results keep theirs: Claude has yet to see them).
    private static void LetOldPicturesGo(Talk talk)
    {
        (int count, long bytes) = Kept(talk);
        if (count > MostImages || bytes > MostImageBytes)
        {
            LetPicturesGo(talk, 1, MostImages / 2, MostImageBytes / 2);
        }
    }

    private static (int Count, long Bytes) Kept(Talk talk)
    {
        int count = 0;
        long bytes = 0;
        foreach (Pictured pictured in talk.Pictured)
        {
            foreach (Answered answer in pictured.Answers)
            {
                count += answer.Pictures;
                bytes += answer.Bytes;
            }
        }
        return (count, bytes);
    }

    // (Locked by the caller.) Claude's reply joins the conversation.
    private static void AddSaid(Talk talk, List<BetaContentBlockParam> said, List<BetaContentBlockParam> unthought)
    {
        if (unthought.Count < said.Count)
        {
            talk.Unthought[talk.Messages.Count] = unthought;
        }
        talk.Messages.Add(new BetaAssistantMessageParam { Content = said });
    }

    // (Locked by the caller.) Lets the oldest results' pictures go, all but those of the newest
    // `keep` messages, until at most `count` pictures and `bytes` are left: each such message is
    // written again as its texts, saying a picture was there.
    //
    // Claude's thinking is signed against everything said before it, so a message written again
    // makes every later thinking block one the service refuses. All of the conversation's thinking
    // so far is therefore taken out with it (which is allowed; taking out some is not): Claude
    // goes on from its words and tools, without what it thought before.
    private static void LetPicturesGo(Talk talk, int keep, int count, long bytes)
    {
        (int kept, long keptBytes) = Kept(talk);
        bool rewritten = false;
        while (talk.Pictured.Count > keep && (kept > count || keptBytes > bytes))
        {
            rewritten = true;
            Pictured oldest = talk.Pictured[0];
            talk.Pictured.RemoveAt(0);
            for (int i = 0; i < oldest.Answers.Count; ++i)
            {
                Answered answer = oldest.Answers[i];
                if (answer.Pictures > 0)
                {
                    kept -= answer.Pictures;
                    keptBytes -= answer.Bytes;
                    oldest.Answers[i] = answer with { Text = answer.Text + "\n" + ImageGone, Pictures = 0, Bytes = 0 };
                }
            }
            talk.Messages[oldest.Index] = Results(oldest.Answers);
        }
        if (rewritten)
        {
            foreach ((int index, List<BetaContentBlockParam> unthought) in talk.Unthought)
            {
                talk.Messages[index] = new BetaAssistantMessageParam { Content = unthought };
            }
            talk.Unthought.Clear();
        }
    }

    private MessageCreateParams Request(Talk talk)
    {
        List<BetaMessageParam> messages;
        lock (_lock)
        {
            messages = [.. talk.Messages];
        }
        return new MessageCreateParams
        {
            Model = Model,
            MaxTokens = MostTokens,
            System = _system,
            Tools = _tools,
            Messages = messages,
            OutputConfig = new BetaOutputConfig { Effort = Effort.Medium },
            // What does not change from request to request (the tools, the
            // system text, the conversation so far) is read from the cache.
            CacheControl = new BetaCacheControlEphemeral(),
            // If Claude's safety checks decline a request, the service tries
            // it on the model it recommends instead of giving up.
            Betas = ["server-side-fallback-2026-07-01"],
            Fallbacks = new Default(),
        };
    }

    private Task<ToolReply> OnMainThread(Talk talk, string name, string input, CancellationToken stop)
    {
        Waiting waiting = new(name, input, stop);
        lock (_lock)
        {
            talk.Tools.Enqueue(waiting);
        }
        return WaitAsync(waiting, stop);
    }

    private async Task<ToolReply> WaitAsync(Waiting waiting, CancellationToken stop)
    {
        // Stopped while it waits: not run. Once the main thread has taken it, it runs to its
        // end and its true result goes back (a command is not said to be undone when it was done).
        using (stop.Register(() =>
               {
                   lock (_lock)
                   {
                       if (!waiting.Taken)
                       {
                           waiting.Done.TrySetCanceled(stop);
                       }
                   }
               }))
        {
            return await waiting.Done.Task.ConfigureAwait(false);
        }
    }

    // What the service says of why, when it says.
    private static string Why(BetaMessage reply) =>
        reply.StopDetails is { Explanation: { Length: > 0 } explanation } ? " " + explanation : "";

    private static string Shown(string text) =>
        text.Length <= MostShownChars ? text : string.Concat(Start(text, MostShownChars), " …");

    // The first `most` characters, not through the middle of one (a surrogate pair).
    private static ReadOnlySpan<char> Start(string text, int most) =>
        text.AsSpan(0, char.IsHighSurrogate(text[most - 1]) ? most - 1 : most);

    private void Note(Talk talk, string kind, string text, string? tool = null, bool ok = true)
    {
        lock (_lock)
        {
            Add(talk, kind, text, tool, ok);
        }
    }

    // (Locked by the caller.) A cleared conversation's late lines are not added.
    private void Add(Talk talk, string kind, string text, string? tool = null, bool ok = true)
    {
        if (!ReferenceEquals(talk, _talk))
        {
            return;
        }
        _entries.Add(new Entry(++_serial, kind, text, tool, ok));
        if (_entries.Count > MostEntries)
        {
            _entries.RemoveRange(0, _entries.Count - MostEntries);
        }
    }
}
