using System.Text.Json;
using Repoverse.Core.Generation;
using Repoverse.Core.Model;

namespace Repoverse.Core.Tests;

public class GenerationTests
{
    private static readonly Seed WorldSeed = new(42);

    [Fact]
    public void Same_inputs_produce_identical_manifest_json()
    {
        var a = JsonSerializer.Serialize(ManifestBuilder.Build(TestRepos.Slice(), WorldSeed), Json.Options);
        var b = JsonSerializer.Serialize(ManifestBuilder.Build(TestRepos.Slice().Reverse().ToList(), WorldSeed), Json.Options);
        Assert.Equal(a, b);
    }

    [Fact]
    public void Different_world_seed_changes_building_seeds_but_not_layout()
    {
        var a = ManifestBuilder.Build(TestRepos.Slice(), WorldSeed);
        var b = ManifestBuilder.Build(TestRepos.Slice(), new Seed(43));
        Assert.All(a.Buildings.Zip(b.Buildings), p =>
        {
            Assert.NotEqual(p.First.Seed, p.Second.Seed);
            Assert.Equal(p.First.Footprint, p.Second.Footprint);
        });
    }

    [Theory]
    [InlineData("ortho32", SemanticCategory.OperatingSystem, District.Kernel)]
    [InlineData("fpga-lab", SemanticCategory.Hardware, District.HardwareIndustrial)]
    [InlineData("tiny-llm", SemanticCategory.AgentSystem, District.Agent)]
    [InlineData("proofs", SemanticCategory.FormalVerification, District.FormalMethods)]
    [InlineData("ledger", SemanticCategory.Financial, District.Financial)]
    [InlineData("site", SemanticCategory.WebInterface, District.WebInterface)]
    [InlineData("corpus", SemanticCategory.Training, District.Training)]
    [InlineData("deployer", SemanticCategory.SystemsInfrastructure, District.Infrastructure)]
    [InlineData("wasmc", SemanticCategory.Compiler, District.Kernel)]
    [InlineData("scratch", SemanticCategory.Experimental, District.Experimental)]
    public void Slice_repositories_classify_into_distinct_districts(string name, SemanticCategory category, District district)
    {
        var b = ManifestBuilder.Build(TestRepos.Slice(), WorldSeed).Buildings.Single(x => x.Repo.Name == name);
        Assert.Equal(category, b.Category.Value);
        Assert.Equal(district, b.District.Value);
        Assert.False(string.IsNullOrEmpty(b.Category.Why.Evidence));
    }

    [Fact]
    public void Purpose_outweighs_language()
    {
        // §80: a Rust web server is a network system, not a foundry.
        var repo = TestRepos.Make("api-gw", "HTTP proxy and RPC gateway", "Rust");
        Assert.Equal(SemanticCategory.Networking, Classifier.Classify(repo).Value);
    }

    [Theory]
    [InlineData("Guppy-to-IR translation with linear type enforcement", SemanticCategory.Compiler)]
    [InlineData("A firmware build", SemanticCategory.Hardware)]
    [InlineData("Quick fixes for the build", SemanticCategory.Experimental)] // "ui" inside "build"/"quick" is not a word
    [InlineData("Theorems about schedulers", SemanticCategory.FormalVerification)]
    public void Keywords_match_whole_words(string description, SemanticCategory expected) =>
        Assert.Equal(expected, Classifier.Classify(TestRepos.Make("x", description, files: ["a.bin"])).Value);

    [Fact]
    public void Archived_repositories_move_to_the_archive_district()
    {
        var repos = new[] { TestRepos.Make("old-kernel", "kernel", "C", archived: true) };
        var b = ManifestBuilder.Build(repos, WorldSeed).Buildings[0];
        Assert.Equal(District.Archive, b.District.Value);
        Assert.Equal(ConstructionState.Archived, b.Construction.Value);
    }

    [Fact]
    public void Overrides_win_and_are_recorded_as_provenance()
    {
        var overrides = new Overrides(new Dictionary<string, Overrides.Entry>
        {
            ["snapkitty/scratch"] = new() { Category = SemanticCategory.Quantum, Landmark = true },
        }, "overrides.json");
        var b = ManifestBuilder.Build(TestRepos.Slice(), WorldSeed, overrides).Buildings.Single(x => x.Repo.Name == "scratch");
        Assert.Equal(SemanticCategory.Quantum, b.Category.Value);
        Assert.Equal("manual-override", b.Category.Why.Rule);
        Assert.True(b.Landmark);
    }

    [Fact]
    public void Existing_buildings_keep_their_plots_when_repositories_are_added()
    {
        var first = ManifestBuilder.Build(TestRepos.Slice(), WorldSeed);
        // "aaa-kernel" sorts first and would take slot 0 of the Kernel district in a fresh build.
        var grown = TestRepos.Slice().Append(TestRepos.Make("aaa-kernel", "kernel hypervisor", "C")).ToList();
        var second = ManifestBuilder.Build(grown, WorldSeed, previous: first);

        foreach (var b in first.Buildings)
            Assert.Equal(b.Footprint, second.Find(b.Repo)!.Footprint);
        var newcomer = second.Buildings.Single(b => b.Repo.Name == "aaa-kernel");
        Assert.DoesNotContain(first.Buildings, b => b.Plot == newcomer.Plot);
    }

    [Fact]
    public void Buildings_never_overlap()
    {
        var repos = Enumerable.Range(0, 60).Select(i => TestRepos.Make($"k{i:00}", "kernel", "C")).ToList();
        var m = ManifestBuilder.Build(repos, WorldSeed);
        var fps = m.Buildings.Select(b => b.Footprint).ToList();
        for (int i = 0; i < fps.Count; i++)
            for (int j = i + 1; j < fps.Count; j++)
                Assert.False(fps[i].X <= fps[j].MaxX && fps[j].X <= fps[i].MaxX && fps[i].Z <= fps[j].MaxZ && fps[j].Z <= fps[i].MaxZ,
                    $"{m.Buildings[i].Repo} overlaps {m.Buildings[j].Repo}");
    }

    [Fact]
    public void Floors_follow_module_structure_not_raw_size()
    {
        var flat = TestRepos.Make("flat", "tool", "Go", Enumerable.Range(0, 40).Select(i => $"f{i}.go"));
        var modular = TestRepos.Make("modular", "tool", "Go", Enumerable.Range(0, 40).Select(i => $"m{i % 20}/f{i}.go"));
        var m = ManifestBuilder.Build([flat, modular], WorldSeed);
        Assert.True(m.Find(modular.Id)!.Interior.Rooms.Count > m.Find(flat.Id)!.Interior.Rooms.Count);
    }

    [Fact]
    public void Dependencies_only_link_repositories_in_the_world()
    {
        var a = TestRepos.Make("a", dependsOn: [new RepoId("snapkitty", "b"), new RepoId("elsewhere", "x")]);
        var b = TestRepos.Make("b");
        var edge = Assert.Single(ManifestBuilder.Build([a, b], WorldSeed).Dependencies);
        Assert.Equal(("snapkitty/a", "snapkitty/b"), (edge.From.Key, edge.To.Key));
    }

    [Fact]
    public void Duplicate_repositories_are_rejected()
    {
        var r = TestRepos.Make("dup");
        Assert.Throws<ArgumentException>(() => ManifestBuilder.Build([r, r], WorldSeed));
    }
}
