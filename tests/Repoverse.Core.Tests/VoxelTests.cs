using Repoverse.Core.Generation;
using Repoverse.Core.Model;
using Repoverse.Core.Voxels;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Tests;

public class VoxelTests
{
    [Fact]
    public void Voxel_packs_type_material_and_state()
    {
        var v = new Voxel(BlockType.Window, MaterialFamily.Glass, 3);
        Assert.Equal(BlockType.Window, v.Type);
        Assert.Equal(MaterialFamily.Glass, v.Material);
        Assert.Equal(3, v.State);
        Assert.False(v.IsOpaque);
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(31, 0)]
    [InlineData(32, 1)]
    [InlineData(-1, -1)]
    [InlineData(-32, -1)]
    [InlineData(-33, -2)]
    public void Chunk_coordinates_floor_toward_negative_infinity(int voxel, int chunk) =>
        Assert.Equal(chunk, ChunkCoord.FloorDiv(voxel));

    [Fact]
    public void Solid_box_meshes_to_six_quads()
    {
        var chunk = new Chunk(new ChunkCoord(0, 0, 0));
        for (int x = 4; x < 12; x++) for (int y = 4; y < 9; y++) for (int z = 4; z < 20; z++)
            chunk[x, y, z] = new Voxel(BlockType.Wall);
        var mesh = GreedyMesher.Build(chunk);
        Assert.Equal(6, mesh.Quads.Count);
        Assert.Equal(2 * (8 * 5 + 8 * 16 + 5 * 16), mesh.VisibleFaces);
    }

    [Fact]
    public void Greedy_quads_cover_exactly_the_visible_faces()
    {
        var rng = new SeededRandom(99);
        var chunk = new Chunk(new ChunkCoord(0, 0, 0));
        BlockType[] types = [BlockType.Air, BlockType.Air, BlockType.Wall, BlockType.Floor, BlockType.Window];
        for (int i = 0; i < ChunkVolume; i++)
            chunk[i % ChunkSize, i / (ChunkSize * ChunkSize), i / ChunkSize % ChunkSize] = new Voxel(rng.Pick(types));
        var mesh = GreedyMesher.Build(chunk);
        Assert.Equal(mesh.VisibleFaces, mesh.Quads.Sum(q => q.Width * q.Height));
        Assert.True(mesh.Quads.Count < mesh.VisibleFaces);
    }

    [Fact]
    public void Faces_against_a_loaded_solid_neighbour_are_culled()
    {
        var chunk = new Chunk(new ChunkCoord(0, 0, 0));
        for (int y = 0; y < ChunkSize; y++) for (int z = 0; z < ChunkSize; z++) chunk[ChunkSize - 1, y, z] = new Voxel(BlockType.Wall);
        var open = GreedyMesher.Build(chunk);
        var closed = GreedyMesher.Build(chunk, (x, _, _) => x >= ChunkSize ? new Voxel(BlockType.Wall) : null);
        Assert.Equal(open.VisibleFaces - ChunkSize * ChunkSize, closed.VisibleFaces);
    }

    [Fact]
    public void Generated_chunks_are_deterministic_and_contain_buildings()
    {
        var manifest = ManifestBuilder.Build(TestRepos.Slice(), new Seed(5));
        var b = manifest.Buildings[0];
        var c = ChunkCoord.FromVoxel(b.Footprint.X + 2, GroundLevel + 1, b.Footprint.Z + 2);
        var gen = new WorldGenerator(manifest);
        var one = gen.Generate(c);
        var two = new WorldGenerator(manifest).Generate(c);
        Assert.True(one.Raw.SequenceEqual(two.Raw));
        var local = one[b.Footprint.X + 2 - c.OriginX, GroundLevel - c.OriginY, b.Footprint.Z + 2 - c.OriginZ];
        Assert.Equal(BlockType.Floor, local.Type);
    }

    [Fact]
    public void Chunks_outside_the_world_height_are_empty()
    {
        var gen = new WorldGenerator(ManifestBuilder.Build(TestRepos.Slice(), new Seed(5)));
        Assert.True(gen.Generate(new ChunkCoord(0, -1, 0)).IsEmpty);
        Assert.True(gen.Generate(new ChunkCoord(0, WorldGenerator.MaxChunkY + 1, 0)).IsEmpty);
    }

    [Fact]
    public async Task Streaming_memory_is_bounded_by_radius_while_travelling()
    {
        var manifest = ManifestBuilder.Build(TestRepos.Slice(), new Seed(5));
        var streamer = new ChunkStreamer(new WorldGenerator(manifest), new WorldEdits(), radius: 2);
        int layers = WorldGenerator.MaxChunkY + 1;
        int maxResident = (2 * (2 + StreamHysteresisChunks) + 1) * (2 * (2 + StreamHysteresisChunks) + 1) * layers;

        for (int step = 0; step < 12; step++)
        {
            await streamer.SettleAsync(step * 3 * ChunkSize, 100);
            Assert.InRange(streamer.Loaded.Count, 1, maxResident);
        }
        Assert.Equal(0, streamer.InFlight);
    }

    [Fact]
    public async Task Player_edits_survive_chunk_unload_and_reload()
    {
        var manifest = ManifestBuilder.Build(TestRepos.Slice(), new Seed(5));
        var edits = new WorldEdits();
        var streamer = new ChunkStreamer(new WorldGenerator(manifest), edits, radius: 1);
        edits.Set(5, GroundLevel + 3, 5, new Voxel(BlockType.Wall, MaterialFamily.Wood));

        await streamer.SettleAsync(0, 0);
        Assert.Equal(BlockType.Wall, streamer.Sample(5, GroundLevel + 3, 5)!.Value.Type);
        await streamer.SettleAsync(50 * ChunkSize, 0);
        Assert.Null(streamer.Sample(5, GroundLevel + 3, 5));
        await streamer.SettleAsync(0, 0);
        Assert.Equal(MaterialFamily.Wood, streamer.Sample(5, GroundLevel + 3, 5)!.Value.Material);
    }

    [Fact]
    public void Generation_honours_cancellation()
    {
        var gen = new WorldGenerator(ManifestBuilder.Build(TestRepos.Slice(), new Seed(5)));
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        Assert.Throws<OperationCanceledException>(() => gen.Generate(new ChunkCoord(0, 0, 0), ct: cts.Token));
    }
}
