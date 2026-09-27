using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Voxels;

/// <summary>
/// Keeps the chunks around a focus point resident (§11, §13). Each <see cref="Update"/>
/// starts generation for the nearest missing chunks on the thread pool (bounded by
/// <c>maxInFlight</c>), collects finished ones, and unloads chunks beyond radius + hysteresis,
/// cancelling their pending work. Memory is bounded by the radius, not by world size.
/// </summary>
public sealed class ChunkStreamer(WorldGenerator generator, WorldEdits edits, int radius = StreamRadiusChunks, int maxInFlight = 8)
{
    private readonly Dictionary<ChunkCoord, Chunk> _loaded = new();
    private readonly Dictionary<ChunkCoord, (Task<Chunk> Task, CancellationTokenSource Cts)> _pending = new();
    private readonly object _statsGate = new();

    public IReadOnlyDictionary<ChunkCoord, Chunk> Loaded => _loaded;
    public int InFlight => _pending.Count;
    public long ChunksGenerated { get; private set; }
    public TimeSpan TotalGenerationTime { get; private set; }

    public sealed record Tick(int Started, int Completed, int Unloaded, int Cancelled);

    public Tick Update(int focusX, int focusZ)
    {
        var center = ChunkCoord.FromVoxel(focusX, GroundLevel, focusZ);
        int completed = 0, unloaded = 0, cancelled = 0, started = 0;

        foreach (var (c, p) in _pending.Where(kv => kv.Value.Task.IsCompleted).ToList())
        {
            _pending.Remove(c);
            p.Cts.Dispose();
            if (p.Task.IsCompletedSuccessfully) { _loaded[c] = p.Task.Result; completed++; }
        }

        int keep = radius + StreamHysteresisChunks;
        foreach (var c in _loaded.Keys.Where(c => HorizontalDistance(c, center) > keep).ToList())
        {
            _loaded.Remove(c);
            unloaded++;
        }
        foreach (var (c, p) in _pending.Where(kv => HorizontalDistance(kv.Key, center) > keep).ToList())
        {
            p.Cts.Cancel();
            _pending.Remove(c);
            cancelled++;
        }

        foreach (var c in Wanted(center))
        {
            if (_pending.Count >= maxInFlight) break;
            if (_loaded.ContainsKey(c) || _pending.ContainsKey(c)) continue;
            var cts = new CancellationTokenSource();
            var token = cts.Token;
            _pending[c] = (Task.Run(() =>
            {
                var start = System.Diagnostics.Stopwatch.GetTimestamp();
                var chunk = generator.Generate(c, edits, token);
                lock (_statsGate) { ChunksGenerated++; TotalGenerationTime += System.Diagnostics.Stopwatch.GetElapsedTime(start); }
                return chunk;
            }, token), cts);
            started++;
        }
        return new Tick(started, completed, unloaded, cancelled);
    }

    /// <summary>Runs updates until everything wanted around the focus is resident.</summary>
    public async Task SettleAsync(int focusX, int focusZ, CancellationToken ct = default)
    {
        while (true)
        {
            Update(focusX, focusZ);
            if (_pending.Count == 0) return;
            await Task.WhenAny(_pending.Values.Select(p => p.Task)).WaitAsync(ct);
        }
    }

    /// <summary>Chunks within the radius, nearest first (§13 prioritisation).</summary>
    public IEnumerable<ChunkCoord> Wanted(ChunkCoord center)
    {
        var list = new List<ChunkCoord>();
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++)
                for (int y = 0; y <= WorldGenerator.MaxChunkY; y++)
                    list.Add(new ChunkCoord(center.X + dx, y, center.Z + dz));
        return list.OrderBy(c => HorizontalDistance(c, center)).ThenBy(c => Math.Abs(c.Y - center.Y)).ThenBy(c => c.X).ThenBy(c => c.Z);
    }

    private static int HorizontalDistance(ChunkCoord a, ChunkCoord b) => Math.Max(Math.Abs(a.X - b.X), Math.Abs(a.Z - b.Z));

    /// <summary>World-space voxel lookup across loaded chunks; null when the chunk is not resident.</summary>
    public Voxel? Sample(int x, int y, int z) =>
        _loaded.TryGetValue(ChunkCoord.FromVoxel(x, y, z), out var chunk)
            ? chunk[x - chunk.Coord.OriginX, y - chunk.Coord.OriginY, z - chunk.Coord.OriginZ]
            : null;
}
