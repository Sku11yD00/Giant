using System;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Giant;

/// <summary>Thrown when an engine command fails; the message says why.</summary>
public sealed class CommandFailedException : Exception
{
    /// <summary>Creates the exception.</summary>
    public CommandFailedException(string command, string error)
        : base($"{command}: {error}")
    {
        Command = command;
        Error = error;
    }

    /// <summary>The command that failed.</summary>
    public string Command { get; }

    /// <summary>Why it failed.</summary>
    public string Error { get; }
}

/// <summary>The engine, as seen from scripts.</summary>
public static class Engine
{
    /// <summary>
    /// Runs an engine command (see the editor's Commands panel or
    /// docs/ai-control.md) and returns its JSON result. Commands run from
    /// scripts are game logic, so they are not added to the editor's undo
    /// history.
    /// </summary>
    /// <exception cref="CommandFailedException">The command failed.</exception>
    public static JsonNode? Execute(string command, object? arguments = null)
    {
        ArgumentNullException.ThrowIfNull(command);
        string json = arguments switch
        {
            null => "{}",
            JsonNode node => node.ToJsonString(),
            string text => text,
            _ => JsonSerializer.Serialize(arguments, arguments.GetType(), JsonContext.Options),
        };
        (bool ok, string reply) = NativeHost.Execute(command, json);
        if (!ok)
        {
            throw new CommandFailedException(command, reply);
        }
        return JsonNode.Parse(reply);
    }

    /// <summary>Writes an info line to the engine log.</summary>
    public static void Log(string text) => NativeHost.Log(1, text);

    /// <summary>Writes a warning to the engine log.</summary>
    public static void LogWarning(string text) => NativeHost.Log(2, text);

    /// <summary>Writes an error to the engine log.</summary>
    public static void LogError(string text) => NativeHost.Log(3, text);
}

internal static class JsonContext
{
    // Reflection-based serialisation for anonymous argument objects such as
    // new { r = 1.0, g = 0.5, b = 0.2 }.
    public static readonly JsonSerializerOptions Options = new()
    {
        IncludeFields = true,
    };
}
