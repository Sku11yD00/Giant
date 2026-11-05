using System;
using System.Runtime.InteropServices;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
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
            _ => ArgumentJson.ToNode(arguments)?.ToJsonString() ?? "null",
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

/// <summary>
/// Turns argument objects such as <c>new { r = 1.0, g = 0.5 }</c> into JSON.
/// Hand-written on purpose: System.Text.Json caches type info for the
/// scripts' own types in process-wide caches, which keeps unloaded script
/// assemblies alive. This caches nothing.
/// </summary>
internal static class ArgumentJson
{
    private const int MaxDepth = 32;

    public static JsonNode? ToNode(object? value) => ToNode(value, 0);

    private static JsonNode? ToNode(object? value, int depth)
    {
        if (depth > MaxDepth)
        {
            throw new ArgumentException("command arguments are nested too deeply (or refer to themselves)");
        }
        switch (value)
        {
            case null:
                return null;
            case JsonNode node:
                return node.DeepClone();
            case string text:
                return JsonValue.Create(text);
            case bool flag:
                return JsonValue.Create(flag);
            case char c:
                return JsonValue.Create(c.ToString());
            case Enum e:
                return JsonValue.Create(e.ToString());
            case float f:
                return Finite(f) ? JsonValue.Create(f) : throw NotFinite();
            case double d:
                return Finite(d) ? JsonValue.Create(d) : throw NotFinite();
            case decimal m:
                return JsonValue.Create(m);
            case sbyte or byte or short or ushort or int or uint or long:
                return JsonValue.Create(Convert.ToInt64(value, CultureInfo.InvariantCulture));
            case ulong u:
                return JsonValue.Create(u);
            case IDictionary dictionary:
            {
                var obj = new JsonObject();
                foreach (DictionaryEntry entry in dictionary)
                {
                    string key = Convert.ToString(entry.Key, CultureInfo.InvariantCulture) ?? "";
                    obj[key] = ToNode(entry.Value, depth + 1);
                }
                return obj;
            }
            case IEnumerable items:
            {
                var array = new JsonArray();
                foreach (object? item in items)
                {
                    array.Add(ToNode(item, depth + 1));
                }
                return array;
            }
            default:
                return FromMembers(value, depth);
        }
    }

    // Public properties and fields, by name (anonymous types, records, classes).
    private static JsonObject FromMembers(object value, int depth)
    {
        var obj = new JsonObject();
        Type type = value.GetType();
        foreach (PropertyInfo property in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (property.CanRead && property.GetIndexParameters().Length == 0)
            {
                obj[property.Name] = ToNode(property.GetValue(value), depth + 1);
            }
        }
        foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.Instance))
        {
            obj[field.Name] = ToNode(field.GetValue(value), depth + 1);
        }
        return obj;
    }

    private static bool Finite(double d) => !double.IsNaN(d) && !double.IsInfinity(d);

    private static ArgumentException NotFinite() =>
        new("command arguments cannot hold NaN or infinity (JSON has no such numbers)");
}
