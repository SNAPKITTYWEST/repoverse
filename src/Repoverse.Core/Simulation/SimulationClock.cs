namespace Repoverse.Core.Simulation;

/// <summary>
/// Fixed-step simulation time, decoupled from render frames (§31). The host calls
/// <see cref="Advance"/> with real frame time; the clock runs zero or more fixed ticks
/// scaled by <see cref="Speed"/>, and fires scheduled actions in tick order.
/// </summary>
public sealed class SimulationClock(double ticksPerSecond = WorldConstants.SimulationTicksPerSecond, int maxTicksPerAdvance = 16)
{
    private readonly PriorityQueue<(long Tick, long Seq, Action Action), (long, long)> _scheduled = new();
    private double _accumulator;
    private long _seq;

    public long Tick { get; private set; }
    public double Speed { get; set; } = 1.0;
    public bool Paused { get; set; }
    public TimeSpan TickLength => TimeSpan.FromSeconds(1 / ticksPerSecond);
    public TimeSpan SimulatedTime => TickLength * Tick;

    /// <summary>Raised once per fixed tick, after that tick's scheduled actions.</summary>
    public event Action<long>? Ticked;

    public void Schedule(long ticksFromNow, Action action)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(ticksFromNow);
        var at = Tick + ticksFromNow;
        _scheduled.Enqueue((at, _seq, action), (at, _seq++));
    }

    /// <returns>Number of ticks run.</returns>
    public int Advance(TimeSpan realElapsed)
    {
        if (Paused || Speed <= 0) return 0;
        _accumulator += realElapsed.TotalSeconds * Speed * ticksPerSecond;
        int ran = 0;
        while (_accumulator >= 1 && ran < maxTicksPerAdvance)
        {
            _accumulator -= 1;
            Step();
            ran++;
        }
        // Spiral-of-death guard: drop backlog rather than stall the frame.
        if (ran == maxTicksPerAdvance) _accumulator = Math.Min(_accumulator, 1);
        return ran;
    }

    /// <summary>Runs exactly one tick regardless of pause; used for deterministic stepping.</summary>
    public void Step()
    {
        Tick++;
        while (_scheduled.TryPeek(out var item, out _) && item.Tick <= Tick)
        {
            _scheduled.Dequeue();
            item.Action();
        }
        Ticked?.Invoke(Tick);
    }
}
