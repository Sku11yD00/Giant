using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Threading;

namespace Giant;

/// <summary>Functions the native engine hands to the managed side.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeApi
{
    // Runs a command. Writes a malloc'd UTF-8 string (result JSON or error)
    // to *reply; returns 0 on success, 1 on failure.
    public delegate* unmanaged<byte*, byte*, byte**, int> Execute;

    // Writes a log line: level 0 trace, 1 info, 2 warning, 3 error.
    public delegate* unmanaged<int, byte*, void> Log;

    // Frees a string from Execute.
    public delegate* unmanaged<byte*, void> FreeString;
}

/// <summary>
/// The entry points the native script host calls (engine use only; scripts
/// never call these). Everything runs on the engine's main thread, and
/// no exception may leave an entry point (that would end the process).
/// </summary>
public static unsafe class NativeHost
{
    private static NativeApi s_api;
    private static ScriptSet? s_scripts;
    private static int s_mainThread;
    // True while script code runs; scripts cannot load or unload scripts.
    private static bool s_busy;
    // Where `await` in a script resumes: queued, then run on the main thread
    // at the start of the next update. One per load, closed on unload.
    private static MainThreadContext? s_context;

    /// <summary>Receives the engine's functions; 0 on success.</summary>
    [UnmanagedCallersOnly]
    public static int Initialize(void* api, int size)
    {
        if (size != sizeof(NativeApi))
        {
            return 1; // engine and API built from different sources
        }
        s_api = *(NativeApi*)api;
        s_mainThread = Environment.CurrentManagedThreadId;
        return 0;
    }

    /// <summary>Loads a script assembly; 0 on success, otherwise logged.</summary>
    [UnmanagedCallersOnly]
    public static int LoadScripts(byte* pathUtf8)
    {
        if (s_busy)
        {
            Log(3, "scripts cannot load scripts while they run");
            return 1;
        }
        try
        {
            UnloadCurrent();
            string path = Marshal.PtrToStringUTF8((IntPtr)pathUtf8) ?? "";
            s_scripts = ScriptSet.Load(path);
            s_context = new MainThreadContext();
            RunScripts(s_scripts.Start);
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "could not load scripts: " + e);
            TryUnloadCurrent();
            return 1;
        }
    }

    /// <summary>Calls OnUpdate on every running script.</summary>
    [UnmanagedCallersOnly]
    public static int Update(float deltaSeconds)
    {
        if (s_busy || s_scripts == null)
        {
            return 0;
        }
        try
        {
            ScriptSet scripts = s_scripts;
            RunScripts(() =>
            {
                s_context?.Drain(); // continuations of earlier awaits first
                scripts.Update(deltaSeconds);
            });
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "script update failed: " + e);
            return 1;
        }
    }

    /// <summary>Stops and unloads the loaded scripts.</summary>
    [UnmanagedCallersOnly]
    public static int UnloadScripts()
    {
        if (s_busy)
        {
            Log(3, "scripts cannot unload scripts while they run");
            return 1;
        }
        return TryUnloadCurrent() ? 0 : 1;
    }

    /// <summary>Scripts currently running (not stopped by an error).</summary>
    [UnmanagedCallersOnly]
    public static int RunningScriptCount() => s_scripts?.RunningCount ?? 0;

    internal static (bool Ok, string Reply) Execute(string command, string argsJson)
    {
        if (s_api.Execute == null)
        {
            return (false, "the script host is not initialised");
        }
        if (Environment.CurrentManagedThreadId != s_mainThread)
        {
            // Engine state belongs to the main thread; an awaited task or a
            // worker thread must hand its result back to OnUpdate instead.
            throw new InvalidOperationException(
                "Engine.Execute must be called from the engine's main thread (OnStart, OnUpdate or OnStop)");
        }
        byte* reply = null;
        int status;
        fixed (byte* name = Utf8(command))
        fixed (byte* args = Utf8(argsJson))
        {
            status = s_api.Execute(name, args, &reply);
        }
        string text = reply != null ? Marshal.PtrToStringUTF8((IntPtr)reply) ?? "" : "";
        if (reply != null)
        {
            s_api.FreeString(reply);
        }
        return (status == 0, text);
    }

    internal static void Log(int level, string text)
    {
        if (s_api.Log == null)
        {
            Console.Error.WriteLine(text);
            return;
        }
        fixed (byte* bytes = Utf8(text))
        {
            s_api.Log(level, bytes);
        }
    }

    private static void RunScripts(Action action)
    {
        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(s_context);
        s_busy = true;
        try
        {
            action();
        }
        finally
        {
            s_busy = false;
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }

    private static bool TryUnloadCurrent()
    {
        try
        {
            UnloadCurrent();
            return true;
        }
        catch (Exception e)
        {
            Log(3, "could not unload scripts: " + e);
            s_scripts = null;
            return false;
        }
    }

    private static void UnloadCurrent()
    {
        ScriptSet? scripts = s_scripts;
        s_scripts = null; // first, so nothing reaches a half-stopped set
        if (scripts == null)
        {
            return;
        }
        WeakReference context = StopAndRelease(scripts);
        scripts = null;
        // Let the collectible context go, so a rebuilt assembly can load.
        for (int i = 0; i < 10 && context.IsAlive; i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        if (context.IsAlive)
        {
            // Something (a static, an event handler, a thread) still holds
            // the old scripts; their memory stays until that lets go.
            Log(2, "the previous scripts are still referenced and could not be unloaded");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)] // no references left in the caller's frame
    private static WeakReference StopAndRelease(ScriptSet scripts)
    {
        WeakReference context = null!;
        RunScripts(() => context = scripts.Stop());
        // Awaits still pending belong to the old scripts; drop them so they
        // neither run against the new scripts nor keep the old ones alive.
        s_context?.Close();
        s_context = null;
        return context;
    }

    private static byte[] Utf8(string text)
    {
        int length = Encoding.UTF8.GetByteCount(text);
        byte[] bytes = new byte[length + 1]; // zero-terminated
        Encoding.UTF8.GetBytes(text, 0, text.Length, bytes, 0);
        return bytes;
    }
}

/// <summary>
/// One loaded script assembly, in its own collectible load context so it can
/// be unloaded and reloaded while the engine runs.
/// </summary>
internal sealed class ScriptSet
{
    private readonly List<Script> _scripts = new();
    private readonly HashSet<Script> _stopped = new();
    private ScriptLoadContext? _context;

    public int RunningCount => _scripts.Count - _stopped.Count;

    public static ScriptSet Load(string path)
    {
        var set = new ScriptSet();
        set.LoadInto(path);
        return set;
    }

    [MethodImpl(MethodImplOptions.NoInlining)] // keeps context references out of the caller
    private void LoadInto(string path)
    {
        string full = Path.GetFullPath(path);
        _context = new ScriptLoadContext(full);
        // From bytes, so the file stays free to be rebuilt while it runs.
        using var stream = new MemoryStream(File.ReadAllBytes(full));
        Assembly assembly = _context.LoadFromStream(stream);
        foreach (Type type in assembly.GetTypes())
        {
            if (type.IsSubclassOf(typeof(Script)) && !type.IsAbstract &&
                type.GetConstructor(Type.EmptyTypes) != null)
            {
                _scripts.Add((Script)Activator.CreateInstance(type)!);
            }
        }
        NativeHost.Log(1, $"loaded {_scripts.Count} script(s) from {Path.GetFileName(full)}");
    }

    public void Start() => Each("OnStart", s => s.OnStart());

    public void Update(float deltaSeconds) => Each("OnUpdate", s => s.OnUpdate(deltaSeconds));

    [MethodImpl(MethodImplOptions.NoInlining)]
    public WeakReference Stop()
    {
        Each("OnStop", s => s.OnStop());
        _scripts.Clear();
        _stopped.Clear();
        var weak = new WeakReference(_context);
        _context?.Unload();
        _context = null;
        return weak;
    }

    private void Each(string what, Action<Script> action)
    {
        foreach (Script script in _scripts)
        {
            if (_stopped.Contains(script))
            {
                continue;
            }
            try
            {
                action(script);
            }
            catch (Exception e)
            {
                // One broken script must not take the others (or the editor) down.
                _stopped.Add(script);
                NativeHost.Log(3, $"{script.GetType().FullName}.{what} threw, script stopped: {e}");
            }
        }
    }
}

internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly AssemblyDependencyResolver _resolver;

    public ScriptLoadContext(string mainAssemblyPath)
        : base(isCollectible: true)
    {
        _resolver = new AssemblyDependencyResolver(mainAssemblyPath);
    }

    protected override Assembly? Load(AssemblyName name)
    {
        // Scripts must share the engine API the host loaded (in its own load
        // context), or their Script subclasses would not be the host's type.
        Assembly api = typeof(Script).Assembly;
        if (name.Name == api.GetName().Name)
        {
            return api;
        }
        string? path = _resolver.ResolveAssemblyToPath(name);
        return path != null ? LoadFromAssemblyPath(path) : null;
    }
}

/// <summary>
/// Makes <c>await</c> in scripts come back to the engine's main thread: work
/// posted from any thread is queued and run at the start of the next update,
/// so code after an await may call the engine. An exception thrown there (or
/// by an <c>async void</c> method) is logged, not fatal.
/// </summary>
internal sealed class MainThreadContext : SynchronizationContext
{
    private readonly object _lock = new();
    private Queue<(SendOrPostCallback Callback, object? State)> _queue = new(); // guarded by _lock
    private bool _closed; // guarded by _lock

    public override void Post(SendOrPostCallback d, object? state)
    {
        lock (_lock)
        {
            if (!_closed)
            {
                _queue.Enqueue((d, state));
            }
        }
    }

    public override void Send(SendOrPostCallback d, object? state) =>
        throw new NotSupportedException("scripts cannot wait synchronously on the main thread");

    public override SynchronizationContext CreateCopy() => this;

    // Runs what was queued before this call; work queued meanwhile waits for
    // the next update, so a script that keeps re-posting cannot hang a frame.
    public void Drain()
    {
        Queue<(SendOrPostCallback Callback, object? State)> work;
        lock (_lock)
        {
            if (_queue.Count == 0)
            {
                return;
            }
            work = _queue;
            _queue = new();
        }
        foreach ((SendOrPostCallback callback, object? state) in work)
        {
            try
            {
                callback(state);
            }
            catch (Exception e)
            {
                NativeHost.Log(3, "a script's async code threw: " + e);
            }
        }
    }

    public void Close()
    {
        lock (_lock)
        {
            _closed = true;
            _queue.Clear();
        }
    }
}
