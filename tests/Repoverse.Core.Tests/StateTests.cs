using Repoverse.Core.Generation;
using Repoverse.Core.Model;
using Repoverse.Core.Persistence;
using Repoverse.Core.Simulation;
using Repoverse.Core.Voxels;

namespace Repoverse.Core.Tests;

public class StateTests
{
    [Fact]
    public void Save_round_trips_player_state_and_edits()
    {
        var manifest = ManifestBuilder.Build(TestRepos.Slice(), new Seed(3));
        var edits = new WorldEdits();
        edits.Set(-5, 12, 40, new Voxel(BlockType.Wall, MaterialFamily.Wood));
        var player = new PlayerState { X = 1.5, Z = -3, Mode = ViewMode.Code, Selection = "repoverse://snapkitty/wasmc/src/lower.rs#L10", Bookmarks = [new("home", "repoverse://@0,9,0")] };
        var path = Path.GetTempFileName();
        try
        {
            SaveGame.Write(path, SaveGame.Capture(manifest, player, edits, tick: 77));
            var loaded = SaveGame.Read(path);
            var restored = new WorldEdits();
            loaded.RestoreEdits(restored);

            Assert.Equal(player.Selection, loaded.Player.Selection);
            Assert.Equal(ViewMode.Code, loaded.Player.Mode);
            Assert.Equal(77, loaded.SimulationTick);
            Assert.Equal(manifest.WorldSeed, loaded.WorldSeed);
            var chunk = new Chunk(ChunkCoord.FromVoxel(-5, 12, 40));
            restored.ApplyTo(chunk);
            Assert.Equal(MaterialFamily.Wood, chunk[-5 - chunk.Coord.OriginX, 12, 40 - chunk.Coord.OriginZ].Material);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Version_1_saves_migrate()
    {
        const string v1 = """{"schemaVersion":1,"seed":"000000000000002a","player":{"x":4,"y":9,"z":2,"cameraMode":"graph"},"simulationTick":5}""";
        var save = SaveGame.Parse(v1);
        Assert.Equal(SaveGame.CurrentSchema, save.SchemaVersion);
        Assert.Equal("000000000000002a", save.WorldSeed);
        Assert.Equal(ViewMode.Graph, save.Player.Mode);
        Assert.Empty(save.Edits);
    }

    [Fact]
    public void Saves_from_a_newer_build_are_refused()
    {
        Assert.Throws<InvalidDataException>(() => SaveGame.Parse("""{"schemaVersion":99}"""));
    }

    [Theory]
    [InlineData("repoverse://snapkitty/wasmc")]
    [InlineData("repoverse://snapkitty/wasmc/src/lower.rs#L42")]
    [InlineData("repoverse://snapkitty/wasmc/dir%20with%20space/a.rs")]
    [InlineData("repoverse://snapkitty/wasmc?room=3")]
    [InlineData("repoverse://@-10,9,200")]
    public void Uris_round_trip(string text) => Assert.Equal(text, RepoverseUri.Parse(text).ToString());

    [Fact]
    public void Uri_maps_back_to_github()
    {
        var uri = RepoverseUri.Parse("repoverse://snapkitty/wasmc/src/lower.rs#L42");
        Assert.Equal("https://github.com/snapkitty/wasmc/blob/main/src/lower.rs#L42", uri.GitHubUrl("main"));
        Assert.Equal("src/lower.rs", uri.Path);
    }

    [Theory]
    [InlineData("http://x/y")]
    [InlineData("repoverse://onlyowner")]
    [InlineData("repoverse://a/b#nope")]
    public void Malformed_uris_are_rejected(string text) => Assert.Throws<FormatException>(() => RepoverseUri.Parse(text));

    [Fact]
    public void Clock_runs_fixed_ticks_independent_of_frame_rate()
    {
        var clock = new SimulationClock(ticksPerSecond: 10);
        int ticks = 0;
        clock.Ticked += _ => ticks++;
        for (int frame = 0; frame < 120; frame++) clock.Advance(TimeSpan.FromSeconds(1 / 120.0)); // one second at 120 FPS
        Assert.InRange(ticks, 9, 10);

        clock.Speed = 4;
        clock.Advance(TimeSpan.FromSeconds(1));
        Assert.InRange(ticks, 9 + 16, 10 + 16); // capped at maxTicksPerAdvance

        clock.Paused = true;
        Assert.Equal(0, clock.Advance(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public void Scheduled_actions_fire_in_tick_then_insertion_order()
    {
        var clock = new SimulationClock();
        var log = new List<string>();
        clock.Schedule(2, () => log.Add("b"));
        clock.Schedule(1, () => log.Add("a"));
        clock.Schedule(2, () => log.Add("c"));
        for (int i = 0; i < 3; i++) clock.Step();
        Assert.Equal(["a", "b", "c"], log);
    }

    [Fact]
    public void Snapshot_diff_emits_typed_events()
    {
        var before = new[] { TestRepos.Make("a", commits: 1), TestRepos.Make("gone") };
        var after = new[]
        {
            TestRepos.Make("a", commits: 30, archived: true, releases: [new Release("v2", DateTimeOffset.UnixEpoch, false)],
                files: ["README.md", "src/main.rs", "src/new.rs"]),
            TestRepos.Make("new"),
        };
        var events = SnapshotDiff.Diff(before, after);
        Assert.Contains(events, e => e is RepositoryArchived { Repo.Name: "a" });
        Assert.Contains(events, e => e is ReleasePublished { Tag: "v2" });
        Assert.Contains(events, e => e is ActivityChanged { CommitsBefore: 1, CommitsAfter: 30 });
        Assert.Contains(events, e => e is StructureChanged { FilesAdded: 1, FilesRemoved: 3 });
        Assert.Contains(events, e => e is RepositoryAdded { Repo.Name: "new" });
        Assert.Contains(events, e => e is RepositoryRemoved { Repo.Name: "gone" });
        Assert.Empty(SnapshotDiff.Diff(before, before));
    }
}
