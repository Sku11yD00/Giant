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
