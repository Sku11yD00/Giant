using System;
using System.Text.Json.Nodes;
using Giant;

namespace HelloGiant;

/// <summary>
/// Fades the editor's background from day-sky blue to a sunset orange and
/// back, through the same set_clear_color command the editor and the AI use.
/// </summary>
public sealed class SunsetFade : Script
{
    private float _time;

    public override void OnStart()
    {
        JsonNode? info = Engine.Execute("get_engine_info");
        Engine.Log($"Hello from C#! Running on {info?["adapter"]} ({info?["backend"]}).");
    }

    public override void OnUpdate(float deltaSeconds)
    {
        _time += deltaSeconds;
        // 0 = day, 1 = sunset, over a 20 second cycle.
        float t = 0.5f - 0.5f * MathF.Cos(_time * MathF.Tau / 20f);
        Engine.Execute("set_clear_color", new
        {
            r = Lerp(0.15f, 0.80f, t),
            g = Lerp(0.34f, 0.25f, t),
            b = Lerp(0.71f, 0.08f, t),
        });
    }

    public override void OnStop() => Engine.Log("SunsetFade stopped.");

    private static float Lerp(float a, float b, float t) => a + (b - a) * t;
}
