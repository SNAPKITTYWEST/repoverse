using System.Diagnostics;
using System.Text.Json;
using Repoverse.Core;
using Repoverse.Core.Generation;
using Repoverse.Core.Ingestion;
using Repoverse.Core.Simulation;
using Repoverse.Core.Voxels;

// repoverse <command> [options]. Each command reads/writes plain JSON so stages can be run,
// inspected and diffed independently: snapshot → manifest → chunks/meshes.

var cmd = args.FirstOrDefault();
var opts = ParseOptions(args.Skip(1).ToArray());
try
{
    return cmd switch
    {
        "ingest-local" => IngestLocal(opts),
        "ingest-github" => await IngestGitHub(opts),
        "generate" => Generate(opts),
        "stats" => await Stats(opts),
        "export-mesh" => ExportMesh(opts),
        "diff" => Diff(opts),
        _ => Usage(),
    };
}
catch (Exception e) when (e is IOException or InvalidDataException or FormatException or ArgumentException or InvalidOperationException or JsonException)
{
    Console.Error.WriteLine($"error: {GitHubIngest.Redact(e.Message)}");
    return 1;
}

static int Usage()
{
    Console.Error.WriteLine("""
        usage:
          repoverse ingest-local  <git-dir> [--owner NAME] [--subprojects] --out snapshot.json
          repoverse ingest-github <owner> [--limit N] [--forks] [--cache DIR] --out snapshot.json   (token: $GITHUB_TOKEN)
          repoverse generate      --snapshot snapshot.json --out manifest.json [--seed N] [--overrides FILE] [--previous manifest.json]
          repoverse stats         --manifest manifest.json
          repoverse export-mesh   --manifest manifest.json --out world-mesh.json
          repoverse diff          --before snapshot.json --after snapshot.json
        """);
    return 2;
}

static int IngestLocal(Dictionary<string, string> o)
{
    var dir = Required(o, "_0");
    var owner = o.GetValueOrDefault("owner", "local");
    var repos = LocalGitIngest.Ingest(dir, owner, o.ContainsKey("subprojects"));
    var snap = new Snapshot { TakenAt = DateTimeOffset.UtcNow, Source = $"git:{Path.GetFullPath(dir)}", Repositories = repos };
    SnapshotStore.Save(Required(o, "out"), snap);
    Console.WriteLine($"[GITHUB] ingested {repos.Count} repositories from {dir}");
    return 0;
}

static async Task<int> IngestGitHub(Dictionary<string, string> o)
{
    var owner = Required(o, "_0");
    var outPath = Required(o, "out");
    var cache = o.GetValueOrDefault("cache", Path.Combine(Path.GetTempPath(), "repoverse-cache"));
    var token = Environment.GetEnvironmentVariable("GITHUB_TOKEN");
    var ingest = new GitHubIngest(new HttpClient(), cache, token);
    var result = await ingest.IngestOwnerAsync(owner, o.ContainsKey("forks"), o.TryGetValue("limit", out var l) ? int.Parse(l) : null);

    var merged = SnapshotStore.Merge(SnapshotStore.Load(outPath), result, $"github:{owner}", DateTimeOffset.UtcNow);
    if (merged.Snapshot.Repositories.Count > 0) SnapshotStore.Save(outPath, merged.Snapshot);
    foreach (var f in result.Failures) Console.Error.WriteLine($"[GITHUB] failed {f.Repo}: {f.Reason}");
    Console.WriteLine($"[GITHUB] {result.Repositories.Count} fresh, {merged.Stale.Count} stale, {result.Failures.Count} failed; " +
                      $"{ingest.RequestsSent} requests ({ingest.NotModified} not modified){(result.RateLimited ? "; RATE LIMITED" : "")}");
    return merged.Snapshot.Repositories.Count > 0 ? 0 : 1;
}

static int Generate(Dictionary<string, string> o)
{
    var snap = SnapshotStore.Load(Required(o, "snapshot")) ?? throw new IOException("snapshot not found");
    var seed = new Seed(o.TryGetValue("seed", out var s) ? ulong.Parse(s) : 0x5AA4C177UL);
    var overrides = o.TryGetValue("overrides", out var ov) ? Overrides.Load(ov) : null;
    var previous = o.TryGetValue("previous", out var pv) && File.Exists(pv) ? ReadManifest(pv) : null;

    var sw = Stopwatch.StartNew();
    var manifest = ManifestBuilder.Build(snap.Repositories, seed, overrides, previous);
    File.WriteAllText(Required(o, "out"), JsonSerializer.Serialize(manifest, Json.Options));
    Console.WriteLine($"[MANIFEST] {manifest.Buildings.Count} buildings, {manifest.Dependencies.Count} dependency edges in {sw.ElapsedMilliseconds} ms (seed {manifest.WorldSeed})");
    Console.WriteLine($"{"repository",-44} {"category",-22} {"district",-18} {"archetype",-13} {"floors",6} {"rooms",6}");
    foreach (var b in manifest.Buildings)
        Console.WriteLine($"{b.Repo.Key,-44} {b.Category.Value,-22} {b.District.Value,-18} {b.Archetype.Value,-13} {b.Floors,6} {b.Interior.Rooms.Count(BuildingRasterizer.IsWorkRoom),6}   ← {b.Category.Why}");
    return 0;
}

static async Task<int> Stats(Dictionary<string, string> o)
{
    var manifest = ReadManifest(Required(o, "manifest"));
    var gen = new WorldGenerator(manifest);
    var chunks = ChunksForBuildings(manifest).ToList();

    var sw = Stopwatch.StartNew();
    var generated = chunks.AsParallel().Select(c => gen.Generate(c)).ToList();
    var genTime = sw.Elapsed;
    var byCoord = generated.ToDictionary(c => c.Coord);
    Voxel? Sample(int x, int y, int z) => byCoord.TryGetValue(ChunkCoord.FromVoxel(x, y, z), out var ch)
        ? ch[x - ch.Coord.OriginX, y - ch.Coord.OriginY, z - ch.Coord.OriginZ] : null;

    sw.Restart();
    var meshes = generated.AsParallel().Select(c => GreedyMesher.Build(c, Sample)).ToList();
    var meshTime = sw.Elapsed;
    int faces = meshes.Sum(m => m.VisibleFaces), quads = meshes.Sum(m => m.Quads.Count);
    int objects = manifest.Buildings.Sum(b => BuildingRasterizer.Objects(b).Count());

    Console.WriteLine($"[VOXEL] {generated.Count} chunks, {generated.Sum(c => (long)c.NonAirCount):N0} solid voxels, {generated.Count * 64} KiB voxel memory");
    Console.WriteLine($"[VOXEL] generate {genTime.TotalMilliseconds:F0} ms total, {genTime.TotalMilliseconds / generated.Count:F2} ms/chunk (parallel)");
    Console.WriteLine($"[VOXEL] mesh {meshTime.TotalMilliseconds:F0} ms; {faces:N0} visible faces → {quads:N0} quads ({quads * 2:N0} tris), {100.0 * quads / Math.Max(1, faces):F1}% of naive");
    Console.WriteLine($"[VOXEL] draw groups: max {meshes.Max(m => m.MaterialGroups)} per chunk, {meshes.Sum(m => m.MaterialGroups)} total");
    Console.WriteLine($"[WORLDGEN] {objects} interactive objects (entities, not voxels)");

    var streamer = new ChunkStreamer(gen, new WorldEdits(), radius: 4);
    var b0 = manifest.Buildings[0];
    sw.Restart();
    await streamer.SettleAsync(b0.Footprint.X, b0.Footprint.Z);
    Console.WriteLine($"[STREAMING] radius 4 around {b0.Repo}: {streamer.Loaded.Count} chunks resident in {sw.ElapsedMilliseconds} ms");
    return 0;
}

static int ExportMesh(Dictionary<string, string> o)
{
    var manifest = ReadManifest(Required(o, "manifest"));
    var gen = new WorldGenerator(manifest);
    var chunks = ChunksForBuildings(manifest).AsParallel().Select(c => gen.Generate(c)).ToDictionary(c => c.Coord);
    Voxel? Sample(int x, int y, int z) => chunks.TryGetValue(ChunkCoord.FromVoxel(x, y, z), out var ch)
        ? ch[x - ch.Coord.OriginX, y - ch.Coord.OriginY, z - ch.Coord.OriginZ] : null;

    // Flat arrays per mesh key keep the file small and map 1:1 onto GPU buffers.
    var groups = new Dictionary<ushort, (List<float> Pos, List<int> Normal)>();
    foreach (var mesh in chunks.Values.AsParallel().Select(c => GreedyMesher.Build(c, Sample)).ToList())
        foreach (var q in mesh.Quads)
        {
            if (!groups.TryGetValue(q.MeshKey, out var g)) groups[q.MeshKey] = g = ([], []);
            foreach (var (x, y, z) in GreedyMesher.Corners(mesh.Coord, q)) { g.Pos.Add(x); g.Pos.Add(y); g.Pos.Add(z); }
            g.Normal.Add((q.Axis + 1) * (q.Positive ? 1 : -1));
        }

    var export = new
    {
        manifest.WorldSeed,
        Groups = groups.OrderBy(kv => kv.Key).Select(kv => new
        {
            Block = ((BlockType)(kv.Key & 0xFF)).ToString(),
            Material = ((Repoverse.Core.Model.MaterialFamily)(kv.Key >> 8 & 0xF)).ToString(),
            Positions = kv.Value.Pos,
            Normals = kv.Value.Normal,
        }),
        Buildings = manifest.Buildings.Select(b => new
        {
            Repo = b.Repo.Key, b.Description, Category = b.Category.Value.ToString(), CategoryWhy = b.Category.Why.ToString(),
            District = b.District.Value.ToString(), Archetype = b.Archetype.Value.ToString(), b.Floors,
            Activity = b.Activity.Value.ToString(), Construction = b.Construction.Value.ToString(),
            b.Footprint.X, b.Footprint.Z, b.Footprint.Width, b.Footprint.Depth, b.Height,
            Rooms = b.Interior.Rooms.Where(BuildingRasterizer.IsWorkRoom).Select(r => new { r.Name, Purpose = r.Purpose.ToString(), r.Floor, r.FileCount }),
        }),
        Objects = manifest.Buildings.SelectMany(BuildingRasterizer.Objects)
            .Select(w => new { w.X, w.Y, w.Z, Kind = w.Kind.ToString(), Uri = new RepoverseUri { Repo = w.Repo, Path = w.SourcePath }.ToString() }),
        Dependencies = manifest.Dependencies.Select(d => new[] { d.From.Key, d.To.Key }),
    };
    File.WriteAllText(Required(o, "out"), JsonSerializer.Serialize(export, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
    Console.WriteLine($"[VOXEL] exported {groups.Values.Sum(g => g.Normal.Count):N0} quads in {groups.Count} material groups");
    return 0;
}

static int Diff(Dictionary<string, string> o)
{
    var before = SnapshotStore.Load(Required(o, "before")) ?? throw new IOException("before snapshot not found");
    var after = SnapshotStore.Load(Required(o, "after")) ?? throw new IOException("after snapshot not found");
    foreach (var e in SnapshotDiff.Diff(before.Repositories, after.Repositories)) Console.WriteLine(e);
    return 0;
}

static IEnumerable<ChunkCoord> ChunksForBuildings(WorldManifest m)
{
    var set = new HashSet<ChunkCoord>();
    foreach (var b in m.Buildings)
    {
        var lo = ChunkCoord.FromVoxel(b.Footprint.X - WorldConstants.RoadWidth, 0, b.Footprint.Z - WorldConstants.RoadWidth);
        var hi = ChunkCoord.FromVoxel(b.Footprint.MaxX + WorldConstants.RoadWidth, WorldConstants.GroundLevel + b.Height, b.Footprint.MaxZ + WorldConstants.RoadWidth);
        for (int x = lo.X; x <= hi.X; x++) for (int y = 0; y <= hi.Y; y++) for (int z = lo.Z; z <= hi.Z; z++)
            set.Add(new ChunkCoord(x, y, z));
    }
    return set.OrderBy(c => c.X).ThenBy(c => c.Z).ThenBy(c => c.Y);
}

static WorldManifest ReadManifest(string path) =>
    JsonSerializer.Deserialize<WorldManifest>(File.ReadAllText(path), Json.Options) ?? throw new InvalidDataException($"{path} is empty");

static string Required(Dictionary<string, string> o, string key) =>
    o.TryGetValue(key, out var v) ? v : throw new ArgumentException(key.StartsWith('_') ? "missing positional argument" : $"missing --{key}");

static Dictionary<string, string> ParseOptions(string[] a)
{
    var o = new Dictionary<string, string>();
    int pos = 0;
    for (int i = 0; i < a.Length; i++)
    {
        if (!a[i].StartsWith("--")) { o[$"_{pos++}"] = a[i]; continue; }
        var key = a[i][2..];
        o[key] = i + 1 < a.Length && !a[i + 1].StartsWith("--") ? a[++i] : "true";
    }
    return o;
}
