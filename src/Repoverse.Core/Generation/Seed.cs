using System.Text;

namespace Repoverse.Core.Generation;

/// <summary>
/// Deterministic seed derivation (§8). string.GetHashCode is randomized per process,
/// so seeds use FNV-1a over UTF-8 followed by a SplitMix64 finalizer.
/// </summary>
public readonly record struct Seed(ulong Value)
{
    public Seed Derive(string scope)
    {
        var h = 14695981039346656037UL ^ Value;
        foreach (var b in Encoding.UTF8.GetBytes(scope))
        {
            h ^= b;
            h *= 1099511628211UL;
        }
        return new Seed(Mix(h));
    }

    public SeededRandom Random() => new(Value);

    internal static ulong Mix(ulong z)
    {
        z += 0x9E3779B97F4A7C15UL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
        return z ^ (z >> 31);
    }

    public override string ToString() => Value.ToString("x16");
}

/// <summary>SplitMix64 stream. Same seed → same sequence on every platform and runtime.</summary>
public sealed class SeededRandom(ulong state)
{
    private ulong _state = state;

    public ulong NextULong()
    {
        _state += 0x9E3779B97F4A7C15UL;
        return Seed.Mix(_state - 0x9E3779B97F4A7C15UL);
    }

    /// <summary>Uniform integer in [min, maxExclusive).</summary>
    public int Next(int min, int maxExclusive)
    {
        if (maxExclusive <= min) return min;
        return min + (int)(NextULong() % (ulong)(maxExclusive - min));
    }

    public T Pick<T>(IReadOnlyList<T> items) => items[Next(0, items.Count)];
}
