# script

Hosts .NET 10 inside the engine and runs C# game scripts
(`scripts/Giant.Scripting` is their API). Scripts reach the engine only
through a command handler the editor connects to its command registry. See
`docs/scripting.md`. The C# component and system framework comes with the
world module (N2).
