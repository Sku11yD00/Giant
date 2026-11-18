using System;
using System.IO;

namespace Giant.Chat;

/// <summary>
/// Where the person's Claude key is: the ANTHROPIC_API_KEY environment
/// variable, or else a file in their own settings folder. Never the
/// project, so a key cannot be committed or packed into a game.
/// </summary>
internal static class Keys
{
    public const string Variable = "ANTHROPIC_API_KEY";

    /// <summary>The settings file: one line, the key.</summary>
    public static string FilePath()
    {
        // (On Windows %APPDATA% first: it is where the person would look, and what a test can point elsewhere.)
        string? roaming = OperatingSystem.IsWindows() ? Environment.GetEnvironmentVariable("APPDATA") : null;
        string folder = string.IsNullOrEmpty(roaming) ? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData, Environment.SpecialFolderOption.DoNotVerify) : roaming;
        return folder.Length == 0 ? "" : Path.Combine(folder, "Giant", "claude_key.txt");
    }

    /// <summary>The key, or null when there is none.</summary>
    public static string? Find()
    {
        string? key = Environment.GetEnvironmentVariable(Variable)?.Trim();
        if (!string.IsNullOrEmpty(key))
        {
            return key;
        }
        try
        {
            string path = FilePath();
            if (path.Length != 0 && File.Exists(path))
            {
                using StreamReader reader = new(path);
                key = reader.ReadLine()?.Trim();
                return string.IsNullOrEmpty(key) ? null : key;
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        return null;
    }

    public static string HowToSet() =>
        "No Claude key was found. Set the " + Variable + " environment variable and start the editor again, or put the key " +
        "on the first line of " + FilePath() + " (a key is made at https://platform.claude.com/settings/keys).";
}
