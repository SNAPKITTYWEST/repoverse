using Repoverse.Core.Model;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Generation;

/// <summary>
/// Canonical repositories → world manifest. Pure and deterministic: same snapshot,
/// seed, overrides and previous manifest give an equal manifest (§59).
/// </summary>
public static class ManifestBuilder
{
    private const int RoomWidth = 6;

    /// <summary>Plot slots of a district ordered outward from its centre, so districts grow as compact blobs.</summary>
    private static readonly (int Col, int Row)[] SpiralSlots = Enumerable.Range(0, DistrictSide * DistrictSide)
        .Select(i => (Col: i % DistrictSide, Row: i / DistrictSide))
        .OrderBy(s => Math.Max(Math.Abs(s.Col * 2 + 1 - DistrictSide), Math.Abs(s.Row * 2 + 1 - DistrictSide)))
        .ThenBy(s => Math.Atan2(s.Row * 2 + 1 - DistrictSide, s.Col * 2 + 1 - DistrictSide))
        .ToArray();

    public static (int X, int Z) DistrictOrigin(District d)
    {
        var i = (int)d;
        const int stride = DistrictSpan + 2 * PlotSize; // avenue between districts
        return (i % DistrictGridColumns * stride, i / DistrictGridColumns * stride);
    }

    public static (int X, int Z) PlotOrigin(PlotAddress plot)
    {
        var (ox, oz) = DistrictOrigin(plot.District);
        var (col, row) = SpiralSlots[plot.Slot];
        return (ox + col * PlotSize, oz + row * PlotSize);
    }

    public static WorldManifest Build(
        IReadOnlyList<CanonicalRepository> repos,
        Seed worldSeed,
        Overrides? overrides = null,
        WorldManifest? previous = null)
    {
        overrides ??= Overrides.Empty;
        var ordered = repos.OrderBy(r => r.Id.Key, StringComparer.Ordinal).ToList();
        var dup = ordered.GroupBy(r => r.Id.Key).FirstOrDefault(g => g.Count() > 1);
        if (dup != null) throw new ArgumentException($"Repository '{dup.Key}' appears twice in the snapshot.");

        var classified = ordered.Select(r =>
        {
            var category = Classifier.Classify(r, overrides);
            var district = overrides.District(r.Id) is { } d
                ? new Derived<District>(d, Provenance.Override(overrides.Source))
                : new Derived<District>(Classifier.DistrictFor(category.Value, r.Archived),
                    new("district-from-category", r.Archived ? "archived" : category.Value.ToString()));
            return (Repo: r, Category: category, District: district);
        }).ToList();

        // Sticky placement (§7, §85): a repo keeps its plot while it stays in the same district;
        // newcomers take the innermost free slot in id order.
        var taken = new Dictionary<District, HashSet<int>>();
        var slotOf = new Dictionary<string, PlotAddress>();
        foreach (var c in classified)
        {
            var old = previous?.Find(c.Repo.Id);
            if (old == null || old.Plot.District != c.District.Value) continue;
            if (!taken.TryGetValue(old.Plot.District, out var set)) taken[old.Plot.District] = set = [];
            if (set.Add(old.Plot.Slot)) slotOf[c.Repo.Id.Key] = old.Plot;
        }
        foreach (var c in classified)
        {
            if (slotOf.ContainsKey(c.Repo.Id.Key)) continue;
            if (!taken.TryGetValue(c.District.Value, out var set)) taken[c.District.Value] = set = [];
            var slot = Enumerable.Range(0, SpiralSlots.Length).FirstOrDefault(s => !set.Contains(s), -1);
            if (slot < 0) throw new InvalidOperationException($"District {c.District.Value} is full ({SpiralSlots.Length} plots).");
            set.Add(slot);
            slotOf[c.Repo.Id.Key] = new PlotAddress(c.District.Value, slot);
        }

        var inWorld = ordered.Select(r => r.Id.Key).ToHashSet();
        var buildings = classified.Select(c => Transform(c.Repo, c.Category, c.District, slotOf[c.Repo.Id.Key], worldSeed, overrides)).ToList();
        var edges = ordered
            .SelectMany(r => r.DependsOn.Where(d => inWorld.Contains(d.Key) && d.Key != r.Id.Key).Select(d => new DependencyEdge(r.Id, d)))
            .DistinctBy(e => (e.From.Key, e.To.Key))
            .OrderBy(e => e.From.Key, StringComparer.Ordinal).ThenBy(e => e.To.Key, StringComparer.Ordinal)
            .ToList();

        return new WorldManifest
        {
            WorldSeed = worldSeed.ToString(),
            Snapshots = ordered.ToDictionary(r => r.Id.Key, r => r.SnapshotId),
            Buildings = buildings,
            Dependencies = edges,
        };
    }

    /// <summary>
    /// Repository → building (§9). Floors follow module structure (room count), depth follows
    /// code volume, activity follows recent commits, construction follows releases — not size → height.
    /// </summary>
    public static BuildingDefinition Transform(
        CanonicalRepository repo,
        Derived<SemanticCategory> category,
        Derived<District> district,
        PlotAddress plot,
        Seed worldSeed,
        Overrides overrides)
    {
        var seed = worldSeed.Derive("repo:" + repo.Id.Key);
        var archetype = overrides.Archetype(repo.Id) is { } a
            ? new Derived<BuildingArchetype>(a, Provenance.Override(overrides.Source))
            : new Derived<BuildingArchetype>(Classifier.ArchetypeFor(category.Value), new("archetype-from-category", category.Value.ToString()));
        bool landmark = overrides.Landmark(repo.Id);

        // Count prospective rooms first to size the floor plate, then build the real graph with that width.
        var probe = RoomGraphBuilder.Build(repo, int.MaxValue);
        int workRooms = probe.Rooms.Count(r => r.Purpose is not (RoomPurpose.Lobby or RoomPurpose.Corridor or RoomPurpose.Stairwell));
        int maxPerSide = (MaxFootprint - 2) / RoomWidth;
        int perSide = Math.Clamp((int)Math.Ceiling(Math.Sqrt(Math.Max(1, workRooms))), 1, maxPerSide);
        if (landmark) perSide = maxPerSide;
        var interior = RoomGraphBuilder.Build(repo, perSide * 2);
        int floors = Math.Min(MaxFloors, interior.Rooms.Count(r => r.Purpose == RoomPurpose.Corridor));

        long codeBytes = repo.Languages.Values.Sum();
        if (codeBytes == 0) codeBytes = repo.Tree.Where(e => e.Kind == TreeEntryKind.File).Sum(e => e.SizeBytes);
        int roomDepth = Math.Clamp(3 + (int)Math.Log2(Math.Max(1, codeBytes / 4096.0) + 1), 4, 10);

        int width = Math.Clamp(2 + perSide * RoomWidth, MinFootprint, MaxFootprint);
        int depth = Math.Clamp(2 * roomDepth + 7, MinFootprint, MaxFootprint);
        var (px, pz) = PlotOrigin(plot);
        var footprint = new Footprint(px + (PlotSize - width) / 2, pz + (PlotSize - depth) / 2, width, depth);

        var activity = repo.CommitsLast90Days switch
        {
            0 => ActivityLevel.Dormant,
            < 10 => ActivityLevel.Low,
            < 50 => ActivityLevel.Moderate,
            _ => ActivityLevel.High,
        };
        var construction = repo.Archived ? ConstructionState.Archived
            : repo.Releases.Any(r => !r.Prerelease) ? ConstructionState.Released
            : repo.Releases.Count > 0 ? ConstructionState.Prerelease
            : ConstructionState.Unreleased;

        return new BuildingDefinition
        {
            Repo = repo.Id,
            Description = repo.Description ?? "",
            Category = category,
            District = district,
            Archetype = archetype,
            Material = MaterialFor(archetype.Value),
            Plot = plot,
            Footprint = footprint,
            Floors = floors,
            RoomsPerSide = perSide,
            RoomDepth = roomDepth,
            Activity = new(activity, new("commits-in-window", $"{repo.CommitsLast90Days} commits / {ActivityWindowDays}d")),
            Construction = new(construction, new("release-state", repo.Archived ? "archived" : $"{repo.Releases.Count} releases")),
            Landmark = landmark,
            Population = Math.Clamp(repo.Contributors, 1, perSide * 2 * floors),
            Seed = seed.ToString(),
            Interior = interior,
        };
    }

    public static MaterialFamily MaterialFor(BuildingArchetype a) => a switch
    {
        BuildingArchetype.Foundry => MaterialFamily.Metal,
        BuildingArchetype.ControlTower => MaterialFamily.Concrete,
        BuildingArchetype.Vault => MaterialFamily.Security,
        BuildingArchetype.ProofChamber => MaterialFamily.Stone,
        BuildingArchetype.Exchange => MaterialFamily.Glass,
        BuildingArchetype.Laboratory => MaterialFamily.Laboratory,
        BuildingArchetype.Library => MaterialFamily.Archive,
        BuildingArchetype.Studio => MaterialFamily.Glass,
        BuildingArchetype.Warehouse => MaterialFamily.Concrete,
        BuildingArchetype.Relay => MaterialFamily.Electronics,
        BuildingArchetype.Commons => MaterialFamily.Wood,
        _ => MaterialFamily.Wood,
    };
}
