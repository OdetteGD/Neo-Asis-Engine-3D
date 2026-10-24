// ===========================================================================
//  HostApi.cs — managed mirror of engine/include/neo/script/ScriptApi.h
//
//  LAYOUT CONTRACT: field order and types must match the C struct exactly.
//  Reordering a field here is an ABI break that corrupts every call, so the
//  struct is laid out in the same order as the header and never reordered.
//
//  All calls use unmanaged function pointers: no delegates, no marshalling
//  stubs, no allocation per call.
// ===========================================================================
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace NeoAsis.Scripting;

/// <summary>Opaque engine world handle. Never dereferenced on this side.</summary>
public readonly unsafe struct NeoWorld
{
    public readonly nint Handle;
    public NeoWorld(nint handle) => Handle = handle;
    public bool IsValid => Handle != 0;
}

public enum LogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NeoHostApi
{
    public uint AbiVersion;

    public delegate* unmanaged<int, byte*, void> Log;

    public delegate* unmanaged<double> TimeSeconds;
    public delegate* unmanaged<float> DeltaSeconds;
    public delegate* unmanaged<ulong> FrameIndex;

    public delegate* unmanaged<NeoWorld, uint, uint> EntityIsAlive;
    public delegate* unmanaged<NeoWorld, byte*, uint> EntityFind;

    public delegate* unmanaged<NeoWorld, uint, float*, void> TransformGet;
    public delegate* unmanaged<NeoWorld, uint, float*, void> TransformSet;
    public delegate* unmanaged<NeoWorld, uint, float*, void> ScaleGet;
    public delegate* unmanaged<NeoWorld, uint, float*, void> ScaleSet;

    public delegate* unmanaged<int, uint> KeyDown;
    public delegate* unmanaged<float*, void> PointerPosition;
    public delegate* unmanaged<uint> PointerDown;

    public delegate* unmanaged<NeoWorld, float, void> SetSunElevation;
    public delegate* unmanaged<NeoWorld, float, void> SetCloudCoverage;
    public delegate* unmanaged<NeoWorld, void> RequestIblRebake;

    // Reserved: must match `void* reserved[8]` on the engine side.
    public fixed ulong Reserved[8];
}

/// <summary>
/// Thin, allocation-free wrapper over the host API. This is the only surface
/// gameplay code should touch; it keeps script code readable while the raw
/// pointers stay an implementation detail.
/// </summary>
public static unsafe class Engine
{
    private static NeoHostApi s_api;
    private static bool s_ready;

    public const uint RequiredAbiVersion = 3;

    internal static void Bind(NeoHostApi api)
    {
        s_api = api;
        s_ready = api.AbiVersion == RequiredAbiVersion;
    }

    public static bool IsReady => s_ready;
    public static uint AbiVersion => s_api.AbiVersion;

    // --- Logging ----------------------------------------------------------
    public static void Log(LogLevel level, string message)
    {
        if (!s_ready || s_api.Log == null) return;

        // Rent-free UTF-8 encode: most messages are short, so stack first.
        int max = Encoding.UTF8.GetMaxByteCount(message.Length) + 1;
        // A conditional expression cannot mix stackalloc with a pointer cast,
        // so reserve the stack buffer unconditionally and only fall back to the
        // heap for unusually long messages.
        byte* stackBuffer = stackalloc byte[512];
        byte* buffer = max <= 512 ? stackBuffer : (byte*)NativeMemory.Alloc((nuint)max);
        try
        {
            fixed (char* src = message)
            {
                int written = Encoding.UTF8.GetBytes(src, message.Length, buffer, max - 1);
                buffer[written] = 0;
                s_api.Log((int)level, buffer);
            }
        }
        finally
        {
            if (max > 512) NativeMemory.Free(buffer);
        }
    }

    public static void Print(string message) => Log(LogLevel.Info, message);

    // --- Time -------------------------------------------------------------
    public static double Time => s_ready && s_api.TimeSeconds != null ? s_api.TimeSeconds() : 0.0;
    public static float DeltaTime => s_ready && s_api.DeltaSeconds != null ? s_api.DeltaSeconds() : 0f;
    public static ulong Frame => s_ready && s_api.FrameIndex != null ? s_api.FrameIndex() : 0UL;

    // --- Entities ---------------------------------------------------------
    public static bool IsAlive(NeoWorld world, uint entity) =>
        s_ready && s_api.EntityIsAlive != null && s_api.EntityIsAlive(world, entity) != 0;

    public static uint Find(NeoWorld world, string name)
    {
        if (!s_ready || s_api.EntityFind == null) return 0;

        int max = Encoding.UTF8.GetMaxByteCount(name.Length) + 1;
        byte* buffer = stackalloc byte[max];
        fixed (char* src = name)
        {
            int written = Encoding.UTF8.GetBytes(src, name.Length, buffer, max - 1);
            buffer[written] = 0;
            return s_api.EntityFind(world, buffer);
        }
    }

    // --- Transform --------------------------------------------------------
    public struct Pose
    {
        public float Px, Py, Pz;
        public float Qx, Qy, Qz, Qw;

        public static Pose Identity => new() { Qw = 1f };
    }

    public static Pose GetPose(NeoWorld world, uint entity)
    {
        Pose p = Pose.Identity;
        if (!s_ready || s_api.TransformGet == null) return p;

        float* buf = stackalloc float[7];
        s_api.TransformGet(world, entity, buf);
        p.Px = buf[0]; p.Py = buf[1]; p.Pz = buf[2];
        p.Qx = buf[3]; p.Qy = buf[4]; p.Qz = buf[5]; p.Qw = buf[6];
        return p;
    }

    public static void SetPose(NeoWorld world, uint entity, Pose p)
    {
        if (!s_ready || s_api.TransformSet == null) return;

        float* buf = stackalloc float[7];
        buf[0] = p.Px; buf[1] = p.Py; buf[2] = p.Pz;
        buf[3] = p.Qx; buf[4] = p.Qy; buf[5] = p.Qz; buf[6] = p.Qw;
        s_api.TransformSet(world, entity, buf);
    }

    /// <summary>Rotate an entity around the Y axis by <paramref name="radians"/>.</summary>
    public static void RotateY(NeoWorld world, uint entity, float radians)
    {
        Pose p = GetPose(world, entity);

        // Conjugate the existing quaternion by a Y-axis rotation:
        //   q' = Ry * q,  Ry = (0, sin(a/2), 0, cos(a/2))
        float s = MathF.Sin(radians * 0.5f);
        float c = MathF.Cos(radians * 0.5f);

        float x = c * p.Qx + s * p.Qz;
        float y = c * p.Qy + s * p.Qw;
        float z = c * p.Qz - s * p.Qx;
        float w = c * p.Qw - s * p.Qy;

        p.Qx = x; p.Qy = y; p.Qz = z; p.Qw = w;
        SetPose(world, entity, p);
    }

    // --- Scale ------------------------------------------------------------
    public static (float X, float Y, float Z) GetScale(NeoWorld world, uint entity)
    {
        if (!s_ready || s_api.ScaleGet == null) return (1f, 1f, 1f);
        float* buf = stackalloc float[3];
        s_api.ScaleGet(world, entity, buf);
        return (buf[0], buf[1], buf[2]);
    }

    public static void SetScale(NeoWorld world, uint entity, float x, float y, float z)
    {
        if (!s_ready || s_api.ScaleSet == null) return;
        float* buf = stackalloc float[3];
        buf[0] = x; buf[1] = y; buf[2] = z;
        s_api.ScaleSet(world, entity, buf);
    }

    // --- Scene knobs ------------------------------------------------------
    public static void SetSunElevation(NeoWorld world, float degrees)
    {
        if (s_ready && s_api.SetSunElevation != null) s_api.SetSunElevation(world, degrees);
    }

    public static void SetCloudCoverage(NeoWorld world, float coverage)
    {
        if (s_ready && s_api.SetCloudCoverage != null) s_api.SetCloudCoverage(world, coverage);
    }
}
