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

    /// <summary>Called once before the scripts are unloaded.</summary>
    public virtual void OnStop() { }
}
