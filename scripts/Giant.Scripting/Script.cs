namespace Giant;

/// <summary>
/// Base class for game scripts. The engine creates one instance of every
/// non-abstract subclass with a public parameterless constructor in a loaded
/// script assembly, calls <see cref="OnStart"/> once, <see cref="OnUpdate"/>
/// every frame, and <see cref="OnStop"/> before the scripts are unloaded or
/// reloaded.
/// </summary>
/// <remarks>
/// Scripts change the engine only through <see cref="Engine.Execute"/>, the
/// same commands the editor and the AI use. A script that throws is logged
/// and stopped; the others keep running.
/// </remarks>
public abstract class Script
{
    /// <summary>Called once, after every script in the assembly was created.</summary>
    public virtual void OnStart() { }

    /// <summary>Called every frame.</summary>
    /// <param name="deltaSeconds">Seconds since the previous frame.</param>
    public virtual void OnUpdate(float deltaSeconds) { }

    /// <summary>
    /// Called when something happened to a person's behaviour while the
    /// world ran: it began the behaviour, or went from one of its states to
    /// another. Called before the frame's <see cref="OnUpdate"/>, in the
    /// order things happened. The same is listed by the command
    /// <c>get_person_events</c>.
    /// </summary>
    /// <param name="e">What happened.</param>
    public virtual void OnPersonEvent(in PersonEvent e) { }

    /// <summary>Called once before the scripts are unloaded.</summary>
    public virtual void OnStop() { }
}

/// <summary>What happened to a person's behaviour (see <see cref="Script.OnPersonEvent"/>).</summary>
public readonly struct PersonEvent
{
    // (Null in a default value: given out as empty.)
    private readonly string? _behaviour;
    private readonly string? _from;
    private readonly string? _to;

    internal PersonEvent(ulong person, string behaviour, string from, string to, double seconds)
    {
        Person = person;
        _behaviour = behaviour;
        _from = from;
        _to = to;
        Seconds = seconds;
    }

    /// <summary>The person's id (as <c>get_people</c> gives it).</summary>
    public ulong Person { get; }

    /// <summary>The name of its behaviour.</summary>
    public string Behaviour => _behaviour ?? "";

    /// <summary>The state it left; empty when it began the behaviour.</summary>
    public string From => _from ?? "";

    /// <summary>The state it is in since.</summary>
    public string To => _to ?? "";

    /// <summary>The simulation's seconds when it happened.</summary>
    public double Seconds { get; }

    /// <summary>True when the person began the behaviour (in its first state).</summary>
    public bool Began => string.IsNullOrEmpty(_from);
}
