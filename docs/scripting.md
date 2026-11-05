# C# scripting

Game logic is written in C# (.NET 10). The engine core stays C++; scripts
talk to it only through commands, the same ones the editor panels and the AI
use (`docs/ai-control.md`). So a script can do exactly what a person can, and
the engine's rules (argument checks, read-only commands) apply to scripts too.

## Writing a script

A script assembly is a .NET class library that references
`scripts/Giant.Scripting` (with `Private="false"`, so the engine's own copy
is used). Every non-abstract `Script` subclass with a parameterless
constructor is created when the assembly loads:

```csharp
using Giant;

public sealed class SunsetFade : Script
{
    private float _time;

    public override void OnStart() => Engine.Log("Hello from C#!");

    public override void OnUpdate(float deltaSeconds)
    {
        _time += deltaSeconds;
        Engine.Execute("set_clear_color", new { r = 0.8f, g = 0.3f, b = 0.1f });
    }
}
```

`Engine.Execute(command, arguments)` returns the command's JSON result and
throws `CommandFailedException` when it fails. Changes made by scripts are
game logic, so they are not added to the editor's undo history.

The full sample is `scripts/samples/HelloGiant`. The build puts it in
`build/<preset>/managed/samples/HelloGiant/HelloGiant.dll`.

## Running scripts

- At start-up: `giant_editor --scripts path/to/MyScripts.dll`
- From the AI or a tool: the `load_scripts` command with `{"path": ...}`;
  `unload_scripts` stops them; `get_scripts` reports what runs.
- In the editor: the Engine panel shows the loaded scripts with Reload and
  Unload buttons.

Reloading: rebuild the assembly (`dotnet build`) and load it again. The old
scripts get `OnStop`, their load context is unloaded, and the new ones start.
The file is read into memory, so it can be rebuilt while it runs.

A script that throws is logged with its stack trace and stopped; the other
scripts and the editor keep running.

## How it works

`engine/script` starts .NET inside the engine process through `hostfxr` (the
official hosting API; no SDK headers are needed to build the engine), loads
`Giant.Scripting.dll` from the `managed` folder next to the editor, and calls
its `[UnmanagedCallersOnly]` entry points. Script assemblies load into a
collectible `AssemblyLoadContext` that shares the engine's API assembly.

.NET can start only once per process. Under ThreadSanitizer the .NET tests
are skipped (it cannot follow .NET's threads); under AddressSanitizer they
run with `handle_segv=0`, because .NET turns null references into
exceptions through its own signal handler.
