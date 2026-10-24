// ===========================================================================
//  ScriptBehaviour.cs — the base class gameplay scripts derive from.
//
//  There is no reflection-based discovery: NativeAOT trims reflection away, so
//  scripts are registered explicitly in GameScripts.cs. That is a deliberate
//  trade — a few lines of boilerplate in exchange for a 2 MB assembly that
//  starts in single-digit milliseconds on a phone.
// ===========================================================================
namespace NeoAsis.Scripting;

/// <summary>
/// Base class for gameplay behaviour attached to an entity.
/// Override only what you need; every callback has a safe default.
/// </summary>
public abstract class ScriptBehaviour
{
    /// <summary>Entity this script is attached to.</summary>
    public uint Entity { get; internal set; }

    /// <summary>World handle, valid for the lifetime of the script.</summary>
    public NeoWorld World { get; internal set; }

    /// <summary>Human-readable name, used in logs.</summary>
    public virtual string Name => GetType().Name;

    /// <summary>Called once, after the assembly loads.</summary>
    public virtual void OnCreate() { }

    /// <summary>Called every frame while the script is enabled.</summary>
    public virtual void OnUpdate(float deltaTime) { }

    /// <summary>Called after the physics/transform step, before rendering.</summary>
    public virtual void OnLateUpdate(float deltaTime) { }

    /// <summary>Called once before the assembly unloads.</summary>
    public virtual void OnDestroy() { }

    /// <summary>Set false to stop OnUpdate without destroying the script.</summary>
    public bool Enabled { get; set; } = true;

    // --- Convenience helpers ----------------------------------------------
    protected float DeltaTime => Engine.DeltaTime;

    protected void Log(string message) => Engine.Log(LogLevel.Info, $"[{Name}] {message}");

    protected void LogWarning(string message) => Engine.Log(LogLevel.Warn, $"[{Name}] {message}");

    protected void LogError(string message) => Engine.Log(LogLevel.Error, $"[{Name}] {message}");

    protected Engine.Pose Pose
    {
        get => Engine.GetPose(World, Entity);
        set => Engine.SetPose(World, Entity, value);
    }

    protected void RotateY(float radians) => Engine.RotateY(World, Entity, radians);
}

/// <summary>
/// A script that needs to run once when the world starts, without being tied to
/// an entity. Useful for spawners, weather drivers and debug tooling.
/// </summary>
public abstract class WorldSystem
{
    public NeoWorld World { get; internal set; }
    public virtual string Name => GetType().Name;

    public virtual void OnCreate() { }
    public virtual void OnUpdate(float deltaTime) { }
    public virtual void OnDestroy() { }

    protected void Log(string message) => Engine.Log(LogLevel.Info, $"[{Name}] {message}");
}
