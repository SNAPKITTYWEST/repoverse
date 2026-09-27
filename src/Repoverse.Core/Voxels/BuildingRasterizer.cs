using Repoverse.Core.Generation;
using Repoverse.Core.Model;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Voxels;

/// <summary>An interactive object placed in the world, kept out of bulk voxel data (§12, §91).</summary>
public sealed record WorldObject(
    string Id,
    RepoId Repo,
    int X, int Y, int Z,
    ArtifactKind Kind,
    string SourcePath,
    int RoomId,
    IReadOnlyList<string> Actions);

/// <summary>Where each room of a building ended up in voxel space, for navigation checks and cutaway.</summary>
public sealed record RoomPlacement(int RoomId, int Floor, int MinX, int MinZ, int MaxX, int MaxZ, int FloorY);

/// <summary>
/// Turns a <see cref="BuildingDefinition"/> into voxels. Floor plan per storey:
/// a 3-wide corridor along X through the middle, rooms on both sides separated by
/// partitions, a door from every room to the corridor, a ladder shaft at the west end
/// of the corridor and the entrance on the east wall.
///
///   z=D-1 ┌──────────┬──────────┐
///         │  room    │  room    │
///         ├───door───┴───door───┤
///         │L   corridor         ⌂ entrance (floor 0)
///         ├───door───┬───door───┤
///         │  room    │  room    │
///   z=0   └──────────┴──────────┘
/// </summary>
public static class BuildingRasterizer
{
    public delegate void Setter(int x, int y, int z, Voxel v);

    public sealed record Layout(IReadOnlyList<RoomPlacement> Rooms, int CorridorZ, int LadderX, (int X, int Y, int Z) Entrance);

    public static Layout Plan(BuildingDefinition b)
    {
        var f = b.Footprint;
        int cz = f.Z + b.RoomDepth + 2;              // first corridor row
        int slot = (f.Width - 2) / b.RoomsPerSide;
        var rooms = new List<RoomPlacement>();
        var byFloor = b.Interior.Rooms.Where(IsWorkRoom).GroupBy(r => r.Floor);
        foreach (var floor in byFloor)
        {
            if (floor.Key >= b.Floors) continue;
            int y = GroundLevel + floor.Key * FloorHeight;
            int i = 0;
            foreach (var room in floor)
            {
                int side = i % 2, col = i / 2;
                int x0 = f.X + 1 + col * slot;
                int x1 = col == b.RoomsPerSide - 1 ? f.MaxX - 1 : x0 + slot - 2;
                var (z0, z1) = side == 0 ? (f.Z + 1, cz - 2) : (cz + 4, f.MaxZ - 1);
                rooms.Add(new RoomPlacement(room.Id, floor.Key, x0, z0, x1, z1, y));
                i++;
            }
        }
        return new Layout(rooms, cz, f.X + 1, (f.MaxX, GroundLevel + 1, cz + 1));
    }

    public static bool IsWorkRoom(Room r) => r.Purpose is not (RoomPurpose.Lobby or RoomPurpose.Corridor or RoomPurpose.Stairwell);

    public static void Rasterize(BuildingDefinition b, Setter set)
    {
        var f = b.Footprint;
        var layout = Plan(b);
        var m = b.Material;
        int cz = layout.CorridorZ;
        int top = GroundLevel + b.Floors * FloorHeight;
        bool scaffold = b.Construction.Value == ConstructionState.Unreleased;
        byte state = (byte)b.Activity.Value;

        // Foundation one voxel wider than the footprint.
        for (int x = f.X - 1; x <= f.MaxX + 1; x++)
            for (int z = f.Z - 1; z <= f.MaxZ + 1; z++)
                set(x, GroundLevel - 1, z, new Voxel(BlockType.Foundation, MaterialFamily.Stone));

        for (int floor = 0; floor < b.Floors; floor++)
        {
            int y0 = GroundLevel + floor * FloorHeight;
            for (int x = f.X; x <= f.MaxX; x++)
                for (int z = f.Z; z <= f.MaxZ; z++)
                {
                    bool shaft = x == layout.LadderX && z == cz + 1;
                    set(x, y0, z, shaft && floor > 0 ? Voxel.Air : new Voxel(BlockType.Floor, m, state));
                    bool edge = x == f.X || x == f.MaxX || z == f.Z || z == f.MaxZ;
                    for (int y = y0 + 1; y < y0 + FloorHeight; y++)
                    {
                        if (edge)
                        {
                            bool window = (y - y0) is 2 or 3 && (x + z) % 3 != 0 && !IsCorner(f, x, z);
                            set(x, y, z, new Voxel(window ? BlockType.Window : BlockType.Wall, m, state));
                        }
                        else if (z == cz - 1 || z == cz + 3)
                            set(x, y, z, new Voxel(BlockType.Partition, m)); // corridor walls; doors carved below
                        else if (shaft)
                            set(x, y, z, new Voxel(BlockType.Ladder, MaterialFamily.Metal));
                    }
                }

            foreach (var room in layout.Rooms.Where(r => r.Floor == floor))
            {
                // East partition between neighbouring rooms.
                if (room.MaxX + 1 < f.MaxX)
                    for (int z = room.MinZ; z <= room.MaxZ; z++)
                        for (int y = y0 + 1; y < y0 + FloorHeight; y++)
                            set(room.MaxX + 1, y, z, new Voxel(BlockType.Partition, m));
                // Door into the corridor, centred on the room.
                int dx = (room.MinX + room.MaxX) / 2;
                int dz = room.MinZ < cz ? cz - 1 : cz + 3;
                for (int y = y0 + 1; y <= y0 + DoorHeight; y++)
                    set(dx, y, dz, Voxel.Air);
            }
            // Ladder shaft continues through the ceiling of every storey but the top one.
            if (floor < b.Floors - 1)
                set(layout.LadderX, y0 + FloorHeight, cz + 1, Voxel.Air);
        }

        // Entrance on the east wall at the corridor.
        for (int y = GroundLevel + 1; y <= GroundLevel + DoorHeight; y++)
            for (int z = cz; z <= cz + 2; z++)
                set(f.MaxX, y, z, Voxel.Air);

        for (int x = f.X; x <= f.MaxX; x++)
            for (int z = f.Z; z <= f.MaxZ; z++)
                set(x, top, z, new Voxel(BlockType.Roof, m, state));

        if (scaffold) // unreleased work reads as a construction site (§26)
            for (int y = GroundLevel; y <= top; y += 2)
                for (int x = f.X - 1; x <= f.MaxX + 1; x += 4)
                    set(x, y, f.Z - 1, new Voxel(BlockType.Scaffold, MaterialFamily.Metal));
    }

    private static bool IsCorner(Footprint f, int x, int z) => (x == f.X || x == f.MaxX) && (z == f.Z || z == f.MaxZ);

    /// <summary>Places each room's leading artifacts along its back wall as interactive objects.</summary>
    public static IEnumerable<WorldObject> Objects(BuildingDefinition b)
    {
        var layout = Plan(b);
        var rooms = b.Interior.Rooms.ToDictionary(r => r.Id);
        foreach (var p in layout.Rooms)
        {
            var room = rooms[p.RoomId];
            bool south = p.MinZ < layout.CorridorZ;
            int z = south ? p.MinZ : p.MaxZ;
            int x = p.MinX;
            foreach (var a in room.Artifacts)
            {
                if (x > p.MaxX) break;
                yield return new WorldObject($"{b.Repo.Key}:{a.Path}", b.Repo, x, p.FloorY + 1, z, a.Kind, a.Path, room.Id, ActionsFor(a.Kind));
                x++;
            }
        }
    }

    public static IReadOnlyList<string> ActionsFor(ArtifactKind k) => k switch
    {
        ArtifactKind.TestBench => ["inspect", "view-history", "test"],
        ArtifactKind.BuildStation => ["inspect", "view-history", "build"],
        ArtifactKind.ProofVault => ["inspect", "view-history", "verify"],
        _ => ["inspect", "view-history", "open"],
    };
}
