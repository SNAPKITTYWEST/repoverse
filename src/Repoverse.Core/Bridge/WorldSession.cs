using Repoverse.Core.Generation;
using Repoverse.Core.Model;
using Repoverse.Core.Persistence;
using Repoverse.Core.Voxels;

namespace Repoverse.Core.Bridge;

public sealed record MeshSection(ushort Key, bool Collision, float[][] Vertices, int[] Indices);
public sealed record MeshPacket(string Id, IReadOnlyList<MeshSection> Sections);
public sealed record StreamPacket(IReadOnlyList<string> Unloaded, IReadOnlyList<MeshPacket> Chunks, int Remaining, int Resident);
public sealed record Destination(string Repo, string Description, double X, double Y, double Z,
    int Floors, int LadderX, int LadderZ, IReadOnlyList<RoomPlacement> Rooms);

/// <summary>Single-client, serialized bridge session. Core Y-up units remain authoritative.</summary>
public sealed class WorldSession
{
    private readonly WorldGenerator _generator;
    private readonly Dictionary<ChunkCoord, Chunk> _chunks = new();
    private readonly HashSet<ChunkCoord> _dirty = new();
    private readonly int _radius;
    public WorldManifest Manifest { get; }
    public WorldEdits Edits { get; } = new();
    public PlayerState Player { get; private set; }
    public long Tick { get; private set; }
    public IReadOnlyList<Destination> Destinations { get; }
    public int Resident => _chunks.Count;

    public WorldSession(WorldManifest manifest, int radius = 1)
    {
        if (manifest.SchemaVersion != WorldManifest.CurrentSchema) throw new InvalidDataException("Unsupported manifest schema.");
        if (radius is < 0 or > 3) throw new ArgumentOutOfRangeException(nameof(radius));
        Manifest = manifest; _radius = radius; _generator = new(manifest);
        Destinations = manifest.Buildings.Select(b => {
            var p = BuildingRasterizer.Plan(b);
            return new Destination(b.Repo.Key, b.Description, p.Entrance.X + 3.5,
                p.Entrance.Y + 1.5, p.Entrance.Z + .5, b.Floors, p.LadderX, p.CorridorZ + 1, p.Rooms);
        }).ToArray();
        var start = Destinations.FirstOrDefault();
        Player = new PlayerState { X = start?.X ?? 10, Y = start?.Y ?? 11, Z = start?.Z ?? 10, Yaw = 180 };
    }

    public static void Validate(PlayerState p)
    {
        if (!double.IsFinite(p.X) || !double.IsFinite(p.Y) || !double.IsFinite(p.Z) || !double.IsFinite(p.Yaw)
            || Math.Abs(p.X) > 1_000_000 || Math.Abs(p.Z) > 1_000_000 || p.Y < -100 || p.Y > 10_000
            || !Enum.IsDefined(p.Mode)) throw new ArgumentException("Invalid player state.");
    }

    public void SetPlayer(PlayerState player) { Validate(player); Player = player; }

    private static IEnumerable<ChunkCoord> Neighbours(ChunkCoord c)
    {
        yield return c;
        yield return c with { X = c.X - 1 }; yield return c with { X = c.X + 1 };
        yield return c with { Y = c.Y - 1 }; yield return c with { Y = c.Y + 1 };
        yield return c with { Z = c.Z - 1 }; yield return c with { Z = c.Z + 1 };
    }
    private static int Distance(ChunkCoord a, ChunkCoord b) => Math.Max(Math.Abs(a.X-b.X), Math.Abs(a.Z-b.Z));

    /// <summary>Bounded generation and upload, nearest first. Unloads are sent before replacement meshes.</summary>
    public StreamPacket Focus(double x, double z, int budget = 4)
    {
        Validate(Player with { X = x, Z = z });
        if (budget is < 1 or > 16) throw new ArgumentOutOfRangeException(nameof(budget));
        var center = ChunkCoord.FromVoxel((int)Math.Floor(x), WorldConstants.GroundLevel, (int)Math.Floor(z));
        var unloaded = new List<string>();
        foreach (var c in _chunks.Keys.Where(c => Distance(c, center) > _radius + 1).ToArray())
        {
            _chunks.Remove(c); _dirty.Remove(c); unloaded.Add(c.ToString());
            foreach (var n in Neighbours(c)) if (_chunks.ContainsKey(n)) _dirty.Add(n);
        }
        var wanted = new List<ChunkCoord>();
        for (int dx = -_radius; dx <= _radius; dx++)
            for (int dz = -_radius; dz <= _radius; dz++)
                for (int y = 0; y <= WorldGenerator.MaxChunkY; y++)
                    wanted.Add(new(center.X + dx, y, center.Z + dz));
        wanted = wanted.OrderBy(c => Distance(c, center)).ThenBy(c => c.Y).ThenBy(c => c.X).ThenBy(c => c.Z).ToList();
        foreach (var c in wanted.Where(c => !_chunks.ContainsKey(c)).Take(budget))
        {
            _chunks[c] = _generator.Generate(c, Edits);
            foreach (var n in Neighbours(c)) if (_chunks.ContainsKey(n)) _dirty.Add(n);
        }
        var result = new List<MeshPacket>();
        foreach (var c in _dirty.OrderBy(c => Distance(c, center)).ThenBy(c => c.Y).ThenBy(c => c.X).ThenBy(c => c.Z).Take(budget).ToArray())
        {
            result.Add(Encode(GreedyMesher.Build(_chunks[c], Sample)));
            _dirty.Remove(c);
        }
        Tick++;
        return new(unloaded, result, wanted.Count(c => !_chunks.ContainsKey(c)) + _dirty.Count, _chunks.Count);
    }
    public Voxel? Sample(int x, int y, int z) => _chunks.TryGetValue(ChunkCoord.FromVoxel(x,y,z), out var c)
        ? c[x-c.Coord.OriginX, y-c.Coord.OriginY, z-c.Coord.OriginZ] : null;

    public static MeshPacket Encode(ChunkMesh mesh)
    {
        var sections = new List<MeshSection>();
        foreach (var group in mesh.Quads.GroupBy(q => q.MeshKey).OrderBy(g => g.Key))
        {
            var vertices = new List<float[]>(); var indices = new List<int>();
            foreach (var q in group)
            {
                int i = vertices.Count;
                vertices.AddRange(GreedyMesher.Corners(mesh.Coord, q).Select(v => new[] {v.X,v.Y,v.Z}));
                indices.AddRange([i,i+1,i+2,i,i+2,i+3]);
            }
            sections.Add(new(group.Key, !new Voxel(group.Key).IsPassable, vertices.ToArray(), indices.ToArray()));
        }
        return new(mesh.Coord.ToString(), sections);
    }
    public IReadOnlyList<WorldObject> Objects() => Manifest.Buildings.SelectMany(BuildingRasterizer.Objects).ToArray();
    public void Edit(int x, int y, int z, ushort packed)
    {
        if (y < 1 || y > WorldGenerator.MaxChunkY * WorldConstants.ChunkSize + 31 || Math.Abs((long)x)>1_000_000 || Math.Abs((long)z)>1_000_000
            || !Enum.IsDefined(new Voxel(packed).Type)) throw new ArgumentException("Invalid voxel edit.");
        Edits.Set(x,y,z,new Voxel(packed));
        var c = ChunkCoord.FromVoxel(x,y,z);
        if (_chunks.ContainsKey(c)) _chunks[c] = _generator.Generate(c,Edits);
        foreach (var n in Neighbours(c)) if (_chunks.ContainsKey(n)) _dirty.Add(n);
    }
    public SaveGame Capture() => SaveGame.Capture(Manifest, Player, Edits, Tick);
    public void Restore(SaveGame save)
    {
        if (save.WorldSeed != Manifest.WorldSeed || save.Generator != Manifest.Generator
            || save.Snapshots.Count != Manifest.Snapshots.Count || save.Snapshots.Any(s => !Manifest.Snapshots.TryGetValue(s.Key,out var v) || v != s.Value))
            throw new InvalidDataException("Save belongs to a different world snapshot.");
        Validate(save.Player);
        // Validate in a temporary store before changing the live session.
        var restored = new WorldEdits(); save.RestoreEdits(restored);
        Edits.Load(restored.Snapshot()); Player = save.Player; Tick = save.SimulationTick;
        foreach (var c in _chunks.Keys.ToArray()) { _chunks[c] = _generator.Generate(c,Edits); _dirty.Add(c); }
    }
}
