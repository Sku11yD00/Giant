using System;
using System.Text.Json.Nodes;
using Giant;

namespace ScriptTests;

/// <summary>Talks to the engine through commands; the C++ test checks the calls.</summary>
public sealed class Talker : Script
{
    private int _frames;

    public override void OnStart()
    {
        JsonNode? reply = Engine.Execute("echo", new { value = 41 });
        int value = reply?["value"]?.GetValue<int>() ?? -1;
        Engine.Execute("record", new { what = "start", echoed = value + 1 });
        try
        {
            Engine.Execute("always_fails");
            Engine.Execute("record", new { what = "no exception" });
        }
        catch (CommandFailedException e)
        {
            Engine.Execute("record", new { what = "failed", error = e.Error });
        }
        Engine.Log("talker started");
    }

    public override void OnUpdate(float deltaSeconds)
    {
        _frames++;
        Engine.Execute("record", new { what = "update", frame = _frames, dt = deltaSeconds });
    }

    public override void OnStop() => Engine.Execute("record", new { what = "stop" });
}

/// <summary>Throws on its second update; must be stopped without hurting Talker.</summary>
public sealed class Crasher : Script
{
    private int _frames;

    public override void OnUpdate(float deltaSeconds)
    {
        if (++_frames == 2)
        {
            throw new InvalidOperationException("crasher crashed on purpose");
        }
    }
}

/// <summary>Abstract scripts are not created.</summary>
public abstract class NotAScript : Script
{
}

/// <summary>Asks the engine to reload scripts from inside a script; must be refused, not crash.</summary>
public sealed class Reentrant : Script
{
    private bool _done;

    public override void OnUpdate(float deltaSeconds)
    {
        if (!_done)
        {
            _done = true;
            Engine.Execute("reenter");
        }
    }
}

/// <summary>Calls the engine from another thread; must get an exception, reported on the main thread.</summary>
public sealed class ThreadProbe : Script
{
    private string? _result;

    public override void OnStart()
    {
        var thread = new System.Threading.Thread(() =>
        {
            try
            {
                Engine.Execute("record", new { what = "from another thread" });
                _result = "allowed";
            }
            catch (InvalidOperationException)
            {
                _result = "refused";
            }
        });
        thread.Start();
        thread.Join();
        Engine.Execute("record", new { what = "thread", result = _result });
    }
}

public enum Weather
{
    Clear,
    Storm,
}

public sealed record Gust(float Strength, Weather Kind);

/// <summary>Sends many kinds of argument values; the C++ test checks the JSON.</summary>
public sealed class Shapes : Script
{
    public override void OnStart()
    {
        Engine.Execute("record", new
        {
            what = "shapes",
            f = 0.25f,
            big = long.MaxValue,
            weather = Weather.Storm,
            gust = new Gust(2.5f, Weather.Clear),
            list = new System.Collections.Generic.List<int> { 1, 2 },
            map = new System.Collections.Generic.Dictionary<string, object?> { ["k"] = null },
            when = new DateTime(2026, 11, 5, 1, 2, 3, DateTimeKind.Utc),
            span = TimeSpan.FromSeconds(1.5),
            id = Guid.Empty,
            v = new System.Numerics.Vector3(1, 2, 3),
        });
        try
        {
            Engine.Execute("record", new { bad = typeof(string) });
        }
        catch (ArgumentException)
        {
            Engine.Execute("record", new { what = "opaque refused" });
        }
    }
}

/// <summary>Code after an await runs on the main thread, so it may call the engine.</summary>
public sealed class Awaiter : Script
{
    private bool _started;

    public override async void OnUpdate(float deltaSeconds)
    {
        if (_started)
        {
            return;
        }
        _started = true;
        await System.Threading.Tasks.Task.Yield();
        Engine.Execute("record", new { what = "after yield" });
        int answer = await System.Threading.Tasks.Task.Run(() => 6 * 7);
        Engine.Execute("record", new { what = "after task", answer });
        throw new InvalidOperationException("async throw on purpose");
    }
}
