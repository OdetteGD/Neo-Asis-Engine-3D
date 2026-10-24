// ===========================================================================
//  GameScripts.cs — YOUR FILE.
//
//  This is the only file you need to edit to add gameplay. Register a script
//  with ScriptRuntime.Add(...) and it starts running on the next frame.
//
//  Entity ids are the indices the engine assigns when a renderable is added to
//  the scene: 0 is reserved, the ground plane is 1, the five spinning props are
//  2..6 and the centre column is 7 (see samples/hello-world/main.cpp).
//
//  Rebuild after editing:
//      ./scripting/build-scripts.sh android
//      ./scripting/build-scripts.sh desktop
// ===========================================================================
namespace NeoAsis.Scripting;

public static class GameScripts
{
    /// <summary>Called once at startup, and again if the world is recreated.</summary>
    public static void Register()
    {
        // --- Per-entity behaviours ----------------------------------------

        // Spin the five props. Alternating direction reads better and shows
        // that scripts run independently.
        for (uint entity = 2; entity <= 6; entity++)
        {
            float speed = (entity % 2 == 0) ? 0.9f : -1.3f;
            ScriptRuntime.Add(new SpinScript { Entity = entity, Speed = speed });
        }

        // Pulse the emissive column's scale so the bloom is obviously alive.
        ScriptRuntime.Add(new PulseScript { Entity = 7, Amplitude = 0.06f, Frequency = 1.4f });

        // --- World systems -------------------------------------------------
        ScriptRuntime.Add(new DayCycleSystem
        {
            DegreesPerSecond = 1.5f,
            MinElevation = 2f,
            MaxElevation = 65f,
        });
    }
}

/// <summary>Rotates its entity around Y at a constant rate.</summary>
public sealed class SpinScript : ScriptBehaviour
{
    public float Speed { get; init; } = 1.0f;

    public override void OnUpdate(float deltaTime)
    {
        RotateY(Speed * deltaTime);
    }
}

/// <summary>Breathes the entity's scale for a visible, cheap animation.</summary>
public sealed class PulseScript : ScriptBehaviour
{
    public float Amplitude { get; init; } = 0.05f;
    public float Frequency { get; init; } = 1.0f;

    private (float X, float Y, float Z) _base = (1f, 1f, 1f);

    public override void OnCreate()
    {
        // Remember the authored scale so pulsing is relative, not absolute.
        _base = Engine.GetScale(World, Entity);
        if (_base.X <= 0f) _base = (1f, 1f, 1f);
    }

    public override void OnUpdate(float deltaTime)
    {
        float s = 1f + Amplitude * MathF.Sin(Engine.Time * Frequency * MathF.Tau);
        Engine.SetScale(World, Entity, _base.X * s, _base.Y * s, _base.Z * s);
    }
}

/// <summary>Moves the sun through the sky, which also re-bakes the IBL.</summary>
public sealed class DayCycleSystem : WorldSystem
{
    public float DegreesPerSecond { get; init; } = 1.0f;
    public float MinElevation { get; init; } = 3.0f;
    public float MaxElevation { get; init; } = 70.0f;

    private float _elevation = 14.0f;
    private int _direction = 1;

    public override void OnCreate()
    {
        Log($"Day cycle online ({MinElevation:F0}..{MaxElevation:F0} degrees)");
    }

    public override void OnUpdate(float deltaTime)
    {
        _elevation += _direction * DegreesPerSecond * deltaTime;

        if (_elevation >= MaxElevation)
        {
            _elevation = MaxElevation;
            _direction = -1;
        }
        else if (_elevation <= MinElevation)
        {
            _elevation = MinElevation;
            _direction = 1;
        }

        Engine.SetSunElevation(World, _elevation);
    }
}
