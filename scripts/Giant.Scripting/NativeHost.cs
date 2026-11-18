using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime;
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

/// <summary>An event as the native side hands it over (NativePersonEvent in script_host.cpp).</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativePersonEvent
{
    public ulong Person;
    public byte* Behaviour; // UTF-8, not zero-terminated
    public byte* From;
    public byte* To;
    public int BehaviourLength;
    public int FromLength;
    public int ToLength;
    public double Seconds;
}

/// <summary>
/// The entry points the native script host calls (engine use only; scripts
/// never call these). Everything runs on the engine's main thread, and
/// no exception may leave an entry point (that would end the process).
/// </summary>
public static unsafe class NativeHost
{
    private static NativeApi s_api;
    // The assembly loaded by LoadScripts (one: loading another replaces it).
    private static ScriptSet? s_scripts;
    // Those added beside it by AddScripts (mods' scripts), in the order added.
    private static readonly List<ScriptSet> s_added = new();
    private static int s_mainThread;
    // True while script code runs; scripts cannot load or unload scripts.
    private static bool s_busy;

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
        // Fewer garbage collector pauses: full collections run in the
        // background instead of blocking (unless memory runs short). Young
        // collections still pause, which is why scripts must not allocate
        // every frame.
        GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
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
            RunScripts(s_scripts, static (scripts, _) => scripts.Start(), 0f);
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "could not load scripts: " + e);
            TryUnloadCurrent();
            return 1;
        }
    }

    /// <summary>
    /// Loads a script assembly beside those loaded (a mod's scripts) and starts
    /// its scripts; 0 on success, otherwise logged (the others run on).
    /// </summary>
    [UnmanagedCallersOnly]
    public static int AddScripts(byte* pathUtf8)
    {
        if (s_busy)
        {
            Log(3, "scripts cannot load scripts while they run");
            return 1;
        }
        if (s_added.Count >= MostAdded)
        {
            Log(3, $"at most {MostAdded} script assemblies can be added");
            return 1;
        }
        ScriptSet? added = null;
        try
        {
            string path = Marshal.PtrToStringUTF8((IntPtr)pathUtf8) ?? "";
            added = ScriptSet.Load(path);
            s_added.Add(added);
            RunScripts(added, static (scripts, _) => scripts.Start(), 0f);
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "could not load scripts: " + e);
            // (One that did not start is not left running, listed where the engine thinks there is none.)
            if (added != null && s_added.Remove(added))
            {
                try
                {
                    StopAndRelease(added);
                }
                catch (Exception stopping)
                {
                    Log(3, "could not unload scripts: " + stopping);
                }
            }
            return 1;
        }
    }

    private const int MostAdded = 256; // (as many as a world has mods)

    /// <summary>Stops and unloads the assemblies AddScripts added (not the one LoadScripts loaded).</summary>
    [UnmanagedCallersOnly]
    public static int UnloadAddedScripts()
    {
        if (s_busy)
        {
            Log(3, "scripts cannot unload scripts while they run");
            return 1;
        }
        try
        {
            UnloadAdded();
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "could not unload scripts: " + e);
            s_added.Clear();
            return 1;
        }
    }

    /// <summary>Calls OnUpdate on every running script.</summary>
    [UnmanagedCallersOnly]
    public static int Update(float deltaSeconds)
    {
        if (s_busy || (s_scripts == null && s_added.Count == 0))
        {
            return 0;
        }
        try
        {
            if (s_scripts != null)
            {
                RunScripts(s_scripts, s_update, deltaSeconds);
            }
            for (int i = 0; i < s_added.Count; i++)
            {
                RunScripts(s_added[i], s_update, deltaSeconds);
            }
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "script update failed: " + e);
            return 1;
        }
    }

    // The events being told (PersonEvents), for the static lambda that tells them.
    private static NativePersonEvent* s_events;
    private static int s_eventCount;
    private static readonly NameCache s_names = new();

    // (Made once: nothing allocates per frame.)
    private static readonly Action<ScriptSet, float> s_update = static (scripts, delta) =>
    {
        scripts.Context.Drain(); // continuations of earlier awaits first
        scripts.Update(delta);
    };

    private static readonly Action<ScriptSet, float> s_tellEvents = static (scripts, _) =>
    {
        for (int i = 0; i < s_eventCount; i++)
        {
            ref readonly NativePersonEvent e = ref s_events[i];
            // Names seen before are the same strings again: nothing allocates for them.
            var told = new PersonEvent(
                e.Person,
                s_names.Get(e.Behaviour, e.BehaviourLength),
                s_names.Get(e.From, e.FromLength),
                s_names.Get(e.To, e.ToLength),
                e.Seconds);
            scripts.PersonEvent(in told);
        }
    };

    /// <summary>Calls OnPersonEvent on every running script, for each event in order.</summary>
    [UnmanagedCallersOnly]
    public static int PersonEvents(void* events, int count, int size)
    {
        if (size != sizeof(NativePersonEvent))
        {
            return 1; // engine and API built from different sources
        }
        if (s_busy || (s_scripts == null && s_added.Count == 0) || count <= 0)
        {
            return 0;
        }
        try
        {
            s_events = (NativePersonEvent*)events;
            s_eventCount = count;
            // (Each assembly hears all of them in order, one assembly after another.)
            if (s_scripts != null)
            {
                RunScripts(s_scripts, s_tellEvents, 0f);
            }
            for (int i = 0; i < s_added.Count; i++)
            {
                RunScripts(s_added[i], s_tellEvents, 0f);
            }
            return 0;
        }
        catch (Exception e)
        {
            Log(3, "telling scripts of a person's event failed: " + e);
            return 1;
        }
        finally
        {
            s_events = null;
            s_eventCount = 0;
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
    public static int RunningScriptCount()
    {
        int running = s_scripts?.RunningCount ?? 0;
        for (int i = 0; i < s_added.Count; i++)
        {
            running += s_added[i].RunningCount;
        }
        return running;
    }

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

    // Runs code of one assembly's scripts: awaits in it come back through that assembly's own context.
    private static void RunScripts(ScriptSet scripts, Action<ScriptSet, float> action, float value)
    {
        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(scripts.Context);
        s_busy = true;
        try
        {
            action(scripts, value);
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
        LetGo(context, null);
    }

    private static void UnloadAdded()
    {
        if (s_added.Count == 0)
        {
            return;
        }
        var contexts = new List<WeakReference>();
        StopAndReleaseAdded(contexts);
        LetGo(null, contexts);
    }

    [MethodImpl(MethodImplOptions.NoInlining)] // no references left in the caller's frame
    private static void StopAndReleaseAdded(List<WeakReference> contexts)
    {
        // (Taken out of the list first, so nothing reaches a half-stopped set; last added, first stopped.)
        ScriptSet[] added = s_added.ToArray();
        s_added.Clear();
        for (int i = added.Length - 1; i >= 0; i--)
        {
            try
            {
                contexts.Add(StopAndRelease(added[i]));
            }
            catch (Exception e)
            {
                Log(3, "could not unload scripts: " + e);
            }
            added[i] = null!;
        }
    }

    // Lets the collectible contexts go, so a rebuilt assembly can load.
    private static void LetGo(WeakReference? one, List<WeakReference>? several)
    {
        static bool Alive(WeakReference? one, List<WeakReference>? several)
        {
            if (one != null && one.IsAlive)
            {
                return true;
            }
            for (int i = 0; several != null && i < several.Count; i++)
            {
                if (several[i].IsAlive)
                {
                    return true;
                }
            }
            return false;
        }
        for (int i = 0; i < 10 && Alive(one, several); i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        if (Alive(one, several))
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
        try
        {
            RunScripts(scripts, static (stopping, _) =>
            {
                stopping.StopScripts();
                stopping.Context.Drain(); // e.g. an async void OnStop's exception, so it is logged
            }, 0f);
        }
        finally
        {
            // Awaits still pending belong to the old scripts; drop them so
            // they neither run later nor keep the old scripts alive.
            scripts.Context.Close();
            context = scripts.Unload();
        }
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

    // Where `await` in one of its scripts resumes: queued, then run on the main
    // thread at the start of the next update. Closed when the set is unloaded.
    public MainThreadContext Context { get; } = new();

    public int RunningCount => _scripts.Count - _stopped.Count;

    public static ScriptSet Load(string path)
    {
        var set = new ScriptSet();
        try
        {
            set.LoadInto(path);
        }
        catch
        {
            set.Unload(); // (what was loaded of it goes: nothing of it runs)
            throw;
        }
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

    public void Start() => Each("OnStart", static (s, _) => s.OnStart(), 0f);

    // Static lambdas with the state passed in: nothing allocates per frame.
    public void Update(float deltaSeconds) => Each("OnUpdate", static (s, delta) => s.OnUpdate(delta), deltaSeconds);

    public void PersonEvent(in PersonEvent e)
    {
        foreach (Script script in _scripts)
        {
            if (_stopped.Contains(script))
            {
                continue;
            }
            try
            {
                script.OnPersonEvent(in e);
            }
            catch (Exception x)
            {
                _stopped.Add(script);
                NativeHost.Log(3, $"{script.GetType().FullName}.OnPersonEvent threw, script stopped: {x}");
            }
        }
    }

    public void StopScripts() => Each("OnStop", static (s, _) => s.OnStop(), 0f);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public WeakReference Unload()
    {
        _scripts.Clear();
        _stopped.Clear();
        var weak = new WeakReference(_context);
        _context?.Unload();
        _context = null;
        return weak;
    }

    private void Each(string what, Action<Script, float> action, float value)
    {
        foreach (Script script in _scripts)
        {
            if (_stopped.Contains(script))
            {
                continue;
            }
            try
            {
                action(script, value);
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

/// <summary>
/// The strings of names the engine tells again and again (behaviours, states):
/// each made once and found again by its bytes, so that telling scripts of
/// events does not allocate. Main thread only.
/// </summary>
internal sealed unsafe class NameCache
{
    private const int MostKept = 16384; // (more than a scene's behaviours and their states have names)
    private const int LongestKept = 256; // bytes: longer names are made each time
    private readonly Dictionary<ulong, string> _byHash = new();

    public string Get(byte* utf8, int length)
    {
        if (length <= 0)
        {
            return "";
        }
        if (length > LongestKept)
        {
            return Encoding.UTF8.GetString(utf8, length);
        }
        ulong hash = 14695981039346656037UL;
        for (int i = 0; i < length; i++)
        {
            hash = (hash ^ utf8[i]) * 1099511628211UL;
        }
        if (_byHash.TryGetValue(hash, out string? kept) && Same(kept, utf8, length))
        {
            return kept;
        }
        // (A name that is not UTF-8 never matches what was made of it, and is made each time: the
        // engine's own names are UTF-8.)
        string made = Encoding.UTF8.GetString(utf8, length);
        if (_byHash.Count >= MostKept)
        {
            _byHash.Clear();
        }
        _byHash[hash] = made; // (two names with one hash take turns: right, only not kept)
        return made;
    }

    [SkipLocalsInit] // (the bytes are written before they are read)
    private static bool Same(string kept, byte* utf8, int length)
    {
        if (kept.Length > length)
        {
            return false; // (a string is never longer than its UTF-8)
        }
        // (A kept string was made from at most LongestKept bytes; each of them that was not UTF-8
        // became a character of three, so thrice as many always fit.)
        byte* bytes = stackalloc byte[LongestKept * 3];
        int written = Encoding.UTF8.GetBytes(kept, new Span<byte>(bytes, LongestKept * 3));
        return written == length && new ReadOnlySpan<byte>(bytes, written).SequenceEqual(new ReadOnlySpan<byte>(utf8, length));
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
