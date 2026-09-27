using Repoverse.Core.Model;

namespace Repoverse.Core.Voxels;

/// <summary>Structural role of a voxel. Interactive things are entities, not blocks (§12).</summary>
public enum BlockType : byte
{
    Air = 0,
    Bedrock,
    Soil,
    Grass,
    Road,
    Sidewalk,
    Foundation,
    Wall,
    Floor,
    Roof,
    Window,
    Partition,
    Ladder,
    Scaffold,
}

/// <summary>
/// 16-bit packed voxel: 8 bits block type, 4 bits material family, 4 bits state.
/// A 32³ chunk is 64 KiB.
/// </summary>
public readonly record struct Voxel(ushort Packed)
{
    public static readonly Voxel Air = default;

    public Voxel(BlockType type, MaterialFamily material = MaterialFamily.Concrete, byte state = 0)
        : this((ushort)((byte)type | ((int)material & 0xF) << 8 | (state & 0xF) << 12)) { }

    public BlockType Type => (BlockType)(Packed & 0xFF);
    public MaterialFamily Material => (MaterialFamily)((Packed >> 8) & 0xF);
    public byte State => (byte)(Packed >> 12);

    public bool IsAir => Type == BlockType.Air;
    /// <summary>Blocks the player/agents can move through.</summary>
    public bool IsPassable => Type is BlockType.Air or BlockType.Ladder;
    /// <summary>Hides faces of neighbours. Windows and ladders are see-through.</summary>
    public bool IsOpaque => Type is not (BlockType.Air or BlockType.Window or BlockType.Ladder or BlockType.Scaffold);

    /// <summary>Key used to batch faces into one draw call (§14 material groups).</summary>
    public ushort MeshKey => (ushort)(Packed & 0x0FFF);
}

public readonly record struct ChunkCoord(int X, int Y, int Z)
{
    public static ChunkCoord FromVoxel(int x, int y, int z) =>
        new(FloorDiv(x), FloorDiv(y), FloorDiv(z));

    public static int FloorDiv(int v) => v >= 0 ? v / WorldConstants.ChunkSize : (v + 1) / WorldConstants.ChunkSize - 1;

    public int OriginX => X * WorldConstants.ChunkSize;
    public int OriginY => Y * WorldConstants.ChunkSize;
    public int OriginZ => Z * WorldConstants.ChunkSize;

    public int ChebyshevDistance(ChunkCoord o) => Math.Max(Math.Abs(X - o.X), Math.Max(Math.Abs(Y - o.Y), Math.Abs(Z - o.Z)));

    public override string ToString() => $"{X},{Y},{Z}";
}

public sealed class Chunk
{
    private const int S = WorldConstants.ChunkSize;
    private readonly ushort[] _voxels = new ushort[WorldConstants.ChunkVolume];

    public Chunk(ChunkCoord coord) => Coord = coord;

    public ChunkCoord Coord { get; }
    public int NonAirCount { get; private set; }
    public bool IsEmpty => NonAirCount == 0;

    public static int Index(int x, int y, int z) => (y * S + z) * S + x;

    public Voxel this[int x, int y, int z]
    {
        get => new(_voxels[Index(x, y, z)]);
        set
        {
            var i = Index(x, y, z);
            var was = _voxels[i] & 0xFF;
            var now = value.Packed & 0xFF;
            if (was == 0 && now != 0) NonAirCount++;
            else if (was != 0 && now == 0) NonAirCount--;
            _voxels[i] = value.Packed;
        }
    }

    public Voxel GetIndex(int i) => new(_voxels[i]);
    public ReadOnlySpan<ushort> Raw => _voxels;
}
