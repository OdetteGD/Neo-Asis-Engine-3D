// ===========================================================================
//  ScriptRuntime.cs — the three exported entry points + the script registry.
//
//  These are the only symbols the engine resolves:
//      neo_script_init(world, host)  -> 0 on success
//      neo_script_update(world, dt)
//      neo_script_shutdown(world)
//
//  [UnmanagedCallersOnly] requires NativeAOT, which is why the project is
//  published with PublishAot=true.
// ===========================================================================
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NeoAsis.Scripting;

public static unsafe class ScriptRuntime
{
    private static readonly List<ScriptBehaviour> s_behaviours = new();
    private static readonly List<WorldSystem> s_systems = new();
    private static readonly List<ScriptBehaviour> s_pendingAdd = new();
    private static NeoWorld s_world;

    public static NeoWorld World => s_world;

    // --- Registration ------------------------------------------------------
    public static void Add(ScriptBehaviour behaviour)
    {
        if (behaviour == null) throw new ArgumentNullException(nameof(behaviour));
        behaviour.World = s_world;
        s_pendingAdd.Add(behaviour);
    }

    public static void Add(WorldSystem system)
    {
        if (system == null) throw new ArgumentNullException(nameof(system));
        system.World = s_world;
        s_systems.Add(system);
        system.OnCreate();
    }

    public static T? Get<T>(uint entity) where T : ScriptBehaviour
    {
        foreach (ScriptBehaviour b in s_behaviours)
        {
            if (b.Entity == entity && b is T typed) return typed;
        }
        return null;
    }

    public static int BehaviourCount => s_behaviours.Count;

    // --- Frame callbacks ---------------------------------------------------
    private static void FlushPending()
    {
        if (s_pendingAdd.Count == 0) return;

        foreach (ScriptBehaviour b in s_pendingAdd)
        {
            s_behaviours.Add(b);
            b.OnCreate();
        }
        s_pendingAdd.Clear();
    }

    private static void Tick(float dt)
    {
        FlushPending();

        for (int i = 0; i < s_behaviours.Count; i++)
        {
            ScriptBehaviour b = s_behaviours[i];
            if (!b.Enabled) continue;
            b.OnUpdate(dt);
        }

        for (int i = 0; i < s_behaviours.Count; i++)
        {
            ScriptBehaviour b = s_behaviours[i];
            if (!b.Enabled) continue;
            b.OnLateUpdate(dt);
        }

        for (int i = 0; i < s_systems.Count; i++) s_systems[i].OnUpdate(dt);
    }

    private static void Teardown()
    {
        for (int i = s_behaviours.Count - 1; i >= 0; i--) s_behaviours[i].OnDestroy();
        for (int i = s_systems.Count - 1; i >= 0; i--) s_systems[i].OnDestroy();
        s_behaviours.Clear();
        s_systems.Clear();
        s_pendingAdd.Clear();
    }

    // --- Exports -----------------------------------------------------------
    [UnmanagedCallersOnly(EntryPoint = "neo_script_init", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Init(NeoWorld world, NeoHostApi* host)
    {
        if (host == null) return 2;
        if (host->AbiVersion != Engine.RequiredAbiVersion) return 1;

        s_world = world;
        Engine.Bind(*host);

        Engine.Print($"Neo-Asis managed scripts online (ABI {host->AbiVersion}, .NET {Environment.Version})");

        // Registration lives in GameScripts.cs — the one file users edit.
        GameScripts.Register();

        FlushPending();
        return 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "neo_script_update", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Update(NeoWorld world, float deltaTime)
    {
        // A world swap means the engine recreated the scene; rebuild state.
        if (world.Handle != s_world.Handle)
        {
            Teardown();
            s_world = world;
            GameScripts.Register();
            FlushPending();
        }

        Tick(deltaTime);
    }

    [UnmanagedCallersOnly(EntryPoint = "neo_script_shutdown", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Shutdown(NeoWorld world)
    {
        Engine.Print("Neo-Asis managed scripts shutting down");
        Teardown();
        s_world = default;
    }
}
