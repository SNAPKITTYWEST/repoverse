using Repoverse.Core.Generation;
using Repoverse.Core.Model;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Voxels;

/// <summary>
/// Procedural base state for any chunk, derived only from the manifest (§11): terrain,
/// district road grid and buildings. Player edits are layered on top by <see cref="WorldEdits"/>.
/// </summary>
public sealed class WorldGenerator
{
    private readonly Dictionary<(int, int), BuildingDefinition> _byPlotCell = new();

    public WorldGenerator(WorldManifest manifest)
    {
        Manifest = manifest;
        foreach (var b in manifest.Buildings)
            _byPlotCell[(Math.DivRem(b.Footprint.X, PlotSize).Quotient, Math.DivRem(b.Footprint.Z, PlotSize).Quotient)] = b;
    }

    public WorldManifest Manifest { get; }

    /// <summary>Top chunk layer that can contain anything.</summary>
    public static int MaxChunkY => ChunkCoord.FloorDiv(GroundLevel + MaxFloors * FloorHeight + 1);

    public Chunk Generate(ChunkCoord c, WorldEdits? edits = null, CancellationToken ct = default)
    {
        var chunk = new Chunk(c);
        if (c.Y < 0 || c.Y > MaxChunkY) return chunk;

        for (int z = 0; z < ChunkSize; z++)
            for (int x = 0; x < ChunkSize; x++)
            {
                int wx = c.OriginX + x, wz = c.OriginZ + z;
                var surface = Surface(wx, wz);
                for (int y = 0; y < ChunkSize; y++)
                {
                    int wy = c.OriginY + y;
                    if (wy >= GroundLevel) break;
                    chunk[x, y, z] = wy == 0 ? new Voxel(BlockType.Bedrock, MaterialFamily.Stone)
                        : wy < GroundLevel - 1 ? new Voxel(BlockType.Soil, MaterialFamily.Stone)
                        : surface;
                }
            }
        ct.ThrowIfCancellationRequested();

        foreach (var b in BuildingsTouching(c))
        {
            BuildingRasterizer.Rasterize(b, (x, y, z, v) =>
            {
                int lx = x - c.OriginX, ly = y - c.OriginY, lz = z - c.OriginZ;
                if ((uint)lx < ChunkSize && (uint)ly < ChunkSize && (uint)lz < ChunkSize)
                    chunk[lx, ly, lz] = v;
            });
            ct.ThrowIfCancellationRequested();
        }

        edits?.ApplyTo(chunk);
        return chunk;
    }

    public IEnumerable<BuildingDefinition> BuildingsTouching(ChunkCoord c)
    {
        int x0 = Math.DivRem(c.OriginX - PlotSize, PlotSize).Quotient, x1 = Math.DivRem(c.OriginX + ChunkSize + PlotSize, PlotSize).Quotient;
        int z0 = Math.DivRem(c.OriginZ - PlotSize, PlotSize).Quotient, z1 = Math.DivRem(c.OriginZ + ChunkSize + PlotSize, PlotSize).Quotient;
        for (int px = x0; px <= x1; px++)
            for (int pz = z0; pz <= z1; pz++)
                if (_byPlotCell.TryGetValue((px, pz), out var b) && Overlaps(b, c))
                    yield return b;
    }

    private static bool Overlaps(BuildingDefinition b, ChunkCoord c)
    {
        // Rasterized extent: footprint plus one voxel of foundation/scaffold, ground-1 to roof.
        int minY = GroundLevel - 1, maxY = GroundLevel + b.Floors * FloorHeight;
        return b.Footprint.X - 1 < c.OriginX + ChunkSize && b.Footprint.MaxX + 1 >= c.OriginX
            && b.Footprint.Z - 1 < c.OriginZ + ChunkSize && b.Footprint.MaxZ + 1 >= c.OriginZ
            && minY < c.OriginY + ChunkSize && maxY >= c.OriginY;
    }

    /// <summary>Ground block: roads ring every plot inside a district, grass elsewhere.</summary>
    public static Voxel Surface(int wx, int wz)
    {
        const int stride = DistrictSpan + 2 * PlotSize;
        int dx = Mod(wx, stride), dz = Mod(wz, stride);
        if (dx >= DistrictSpan || dz >= DistrictSpan)
        {
            // Avenue between districts with a central road.
            int ax = dx - DistrictSpan, az = dz - DistrictSpan;
            bool onRoad = (dx >= DistrictSpan && Math.Abs(ax - PlotSize) < RoadWidth) || (dz >= DistrictSpan && Math.Abs(az - PlotSize) < RoadWidth);
            return onRoad ? new Voxel(BlockType.Road, MaterialFamily.Concrete) : new Voxel(BlockType.Grass, MaterialFamily.Wood);
        }
        int px = dx % PlotSize, pz = dz % PlotSize;
        int half = RoadWidth / 2;
        bool road = px < half || px >= PlotSize - half || pz < half || pz >= PlotSize - half;
        bool walk = !road && (px < RoadWidth || px >= PlotSize - RoadWidth || pz < RoadWidth || pz >= PlotSize - RoadWidth);
        return road ? new Voxel(BlockType.Road, MaterialFamily.Concrete)
            : walk ? new Voxel(BlockType.Sidewalk, MaterialFamily.Stone)
            : new Voxel(BlockType.Grass, MaterialFamily.Wood);
    }

    private static int Mod(int a, int m) => ((a % m) + m) % m;
}

/// <summary>Persistent user modifications, stored separately from procedural state (§11, §22).</summary>
public sealed class WorldEdits
{
    private readonly Dictionary<ChunkCoord, Dictionary<int, ushort>> _edits = new();
    private readonly object _gate = new();

    public void Set(int x, int y, int z, Voxel v)
    {
        var c = ChunkCoord.FromVoxel(x, y, z);
        lock (_gate)
        {
            if (!_edits.TryGetValue(c, out var map)) _edits[c] = map = new();
            map[Chunk.Index(x - c.OriginX, y - c.OriginY, z - c.OriginZ)] = v.Packed;
        }
    }

    public void ApplyTo(Chunk chunk)
    {
        lock (_gate)
        {
            if (!_edits.TryGetValue(chunk.Coord, out var map)) return;
            foreach (var (i, packed) in map)
            {
                int x = i % ChunkSize, z = i / ChunkSize % ChunkSize, y = i / (ChunkSize * ChunkSize);
                chunk[x, y, z] = new Voxel(packed);
            }
        }
    }

    public int Count { get { lock (_gate) return _edits.Values.Sum(m => m.Count); } }

    public IReadOnlyDictionary<ChunkCoord, IReadOnlyDictionary<int, ushort>> Snapshot()
    {
        lock (_gate)
            return _edits.ToDictionary(kv => kv.Key, kv => (IReadOnlyDictionary<int, ushort>)new Dictionary<int, ushort>(kv.Value));
    }

    public void Load(IReadOnlyDictionary<ChunkCoord, IReadOnlyDictionary<int, ushort>> data)
    {
        lock (_gate)
        {
            _edits.Clear();
            foreach (var (c, map) in data) _edits[c] = new Dictionary<int, ushort>(map);
        }
    }
}
