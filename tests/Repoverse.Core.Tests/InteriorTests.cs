using Repoverse.Core.Generation;
using Repoverse.Core.Voxels;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Tests;

public class InteriorTests
{
    [Fact]
    public void Ignored_directories_never_become_rooms()
    {
        var files = new[] { "src/a.js", "src/b.js" }
            .Concat(Enumerable.Range(0, 5000).Select(i => $"node_modules/pkg{i}/index.js"))
            .Concat(["target/debug/app", "build/out.o"]);
        var graph = RoomGraphBuilder.Build(TestRepos.Make("web", files: files), 6);
        Assert.DoesNotContain(graph.Rooms, r => r.Directories.Any(d => d.StartsWith("node_modules") || d.StartsWith("target") || d.StartsWith("build")));
        Assert.Contains(graph.Rooms, r => r.Name == "src");
    }

    [Fact]
    public void Huge_repositories_are_compressed_to_the_room_budget()
    {
        var files = Enumerable.Range(0, 200).SelectMany(d => Enumerable.Range(0, 3).Select(f => $"pkg{d:000}/file{f}.c"));
        var graph = RoomGraphBuilder.Build(TestRepos.Make("mono", files: files), 10);
        var work = graph.Rooms.Where(BuildingRasterizer.IsWorkRoom).ToList();
        Assert.True(work.Count <= MaxRoomsPerBuilding);
        Assert.Equal(600, work.Sum(r => r.FileCount)); // nothing lost, only folded into Commons
        Assert.Contains(work, r => r.Name == "Commons");
    }

    [Fact]
    public void Room_purpose_comes_from_directory_meaning()
    {
        var graph = RoomGraphBuilder.Build(TestRepos.Make("r", files: ["tests/a.py", "tests/b.py", "docs/x.md", "docs/y.md", "proofs/p.lean", "proofs/q.lean"]), 6);
        Assert.Equal(RoomPurpose.InspectionLab, graph.Rooms.Single(r => r.Name == "tests").Purpose);
        Assert.Equal(RoomPurpose.Archive, graph.Rooms.Single(r => r.Name == "docs").Purpose);
        Assert.Equal(RoomPurpose.ProofVault, graph.Rooms.Single(r => r.Name == "proofs").Purpose);
    }

    [Fact]
    public void Every_room_graph_in_the_slice_is_connected()
    {
        foreach (var b in ManifestBuilder.Build(TestRepos.Slice(), new Seed(1)).Buildings)
            Assert.Empty(b.Interior.Unreachable());
    }

    /// <summary>
    /// §89 against real geometry: flood-fill walkable air from the entrance (2 voxels of
    /// headroom, one-step climbs, ladders vertical) and require every room to be reached.
    /// </summary>
    [Theory]
    [InlineData(4)]
    [InlineData(40)]
    [InlineData(120)]
    public void Every_rasterized_room_is_reachable_on_foot(int directories)
    {
        var files = Enumerable.Range(0, directories).SelectMany(d => new[] { $"m{d:000}/a.rs", $"m{d:000}/b.rs" });
        var b = ManifestBuilder.Build([TestRepos.Make("walk", "kernel", "Rust", files)], new Seed(7)).Buildings[0];

        var grid = new Dictionary<(int, int, int), Voxel>();
        BuildingRasterizer.Rasterize(b, (x, y, z, v) => grid[(x, y, z)] = v);
        Voxel At(int x, int y, int z) => grid.TryGetValue((x, y, z), out var v) ? v : Voxel.Air;
        bool Standable(int x, int y, int z) => At(x, y, z).IsPassable && At(x, y + 1, z).IsPassable && (!At(x, y - 1, z).IsPassable || At(x, y, z).Type == BlockType.Ladder || At(x, y - 1, z).Type == BlockType.Ladder);

        var layout = BuildingRasterizer.Plan(b);
        var f = b.Footprint;
        var start = layout.Entrance;
        var seen = new HashSet<(int, int, int)> { start };
        var queue = new Queue<(int X, int Y, int Z)>(seen);
        while (queue.Count > 0)
        {
            var (x, y, z) = queue.Dequeue();
            foreach (var (dx, dz) in new[] { (1, 0), (-1, 0), (0, 1), (0, -1) })
                foreach (var dy in new[] { 0, 1, -1 })
                {
                    var n = (x + dx, y + dy, z + dz);
                    if (n.Item1 < f.X || n.Item1 > f.MaxX || n.Item3 < f.Z || n.Item3 > f.MaxZ) continue;
                    if (Standable(n.Item1, n.Item2, n.Item3) && seen.Add(n)) queue.Enqueue(n);
                }
            if (At(x, y, z).Type == BlockType.Ladder || At(x, y + 1, z).Type == BlockType.Ladder || At(x, y - 1, z).Type == BlockType.Ladder)
                foreach (var dy in new[] { 1, -1 })
                {
                    var n = (x, y + dy, z);
                    if (At(n.Item1, n.Item2, n.Item3).IsPassable && seen.Add(n)) queue.Enqueue(n);
                }
        }

        Assert.NotEmpty(layout.Rooms);
        Assert.Equal(b.Interior.Rooms.Count(BuildingRasterizer.IsWorkRoom), layout.Rooms.Count);
        foreach (var room in layout.Rooms)
        {
            var c = ((room.MinX + room.MaxX) / 2, room.FloorY + 1, (room.MinZ + room.MaxZ) / 2);
            Assert.True(seen.Contains(c), $"room {room.RoomId} on floor {room.Floor} is sealed");
        }
    }

    [Fact]
    public void Artifacts_become_interactive_objects_inside_their_rooms()
    {
        var b = ManifestBuilder.Build(TestRepos.Slice(), new Seed(1)).Buildings.Single(x => x.Repo.Name == "wasmc");
        var objects = BuildingRasterizer.Objects(b).ToList();
        var placements = BuildingRasterizer.Plan(b).Rooms.ToDictionary(r => r.RoomId);
        Assert.Contains(objects, o => o.SourcePath == "tests/lower.rs" && o.Kind == ArtifactKind.TestBench && o.Actions.Contains("test"));
        Assert.All(objects, o =>
        {
            var p = placements[o.RoomId];
            Assert.InRange(o.X, p.MinX, p.MaxX);
            Assert.InRange(o.Z, p.MinZ, p.MaxZ);
        });
    }
}
