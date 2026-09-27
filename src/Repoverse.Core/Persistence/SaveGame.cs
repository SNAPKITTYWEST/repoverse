using System.Text.Json;
using System.Text.Json.Nodes;
using Repoverse.Core.Generation;
using Repoverse.Core.Voxels;

namespace Repoverse.Core.Persistence;

public enum ViewMode { World, Code, Graph, Build, Cad, Audit, Agent }

public sealed record Bookmark(string Label, string Uri);

public sealed record PlayerState
{
    public double X { get; init; }
    public double Y { get; init; }
    public double Z { get; init; }
    public double Yaw { get; init; }
    public ViewMode Mode { get; init; } = ViewMode.World;
    /// <summary>repoverse:// URI of the current selection, if any.</summary>
    public string? Selection { get; init; }
    public IReadOnlyList<Bookmark> Bookmarks { get; init; } = [];
}

public sealed record ChunkEdits(string Chunk, IReadOnlyDictionary<int, ushort> Voxels);

/// <summary>
/// Versioned save (§42, §43). Holds player state and user modifications only;
/// canonical and generated state are rebuilt from the snapshot and manifest.
/// </summary>
public sealed record SaveGame
{
    public const int CurrentSchema = 2;

    public int SchemaVersion { get; init; } = CurrentSchema;
    public required string WorldSeed { get; init; }
    public required string Generator { get; init; }
    public required IReadOnlyDictionary<string, string> Snapshots { get; init; }
    public required PlayerState Player { get; init; }
    public IReadOnlyList<ChunkEdits> Edits { get; init; } = [];
    public long SimulationTick { get; init; }

    public static SaveGame Capture(WorldManifest manifest, PlayerState player, WorldEdits edits, long tick) => new()
    {
        WorldSeed = manifest.WorldSeed,
        Generator = manifest.Generator,
        Snapshots = manifest.Snapshots,
        Player = player,
        Edits = edits.Snapshot().OrderBy(kv => kv.Key.ToString(), StringComparer.Ordinal)
            .Select(kv => new ChunkEdits(kv.Key.ToString(), kv.Value)).ToList(),
        SimulationTick = tick,
    };

    public void RestoreEdits(WorldEdits edits) => edits.Load(Edits.ToDictionary(
        e => { var p = e.Chunk.Split(','); return new ChunkCoord(int.Parse(p[0]), int.Parse(p[1]), int.Parse(p[2])); },
        e => e.Voxels));

    public static void Write(string path, SaveGame save)
    {
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(save, Json.Options));
        File.Move(tmp, path, overwrite: true);
    }

    public static SaveGame Read(string path) => Parse(File.ReadAllText(path));

    public static SaveGame Parse(string json)
    {
        var node = JsonNode.Parse(json)?.AsObject() ?? throw new InvalidDataException("Save file is empty.");
        var version = node["schemaVersion"]?.GetValue<int>() ?? 1;
        if (version > CurrentSchema)
            throw new InvalidDataException($"Save schema {version} is newer than this build supports ({CurrentSchema}).");
        for (; version < CurrentSchema; version++) Migrations[version](node);
        node["schemaVersion"] = CurrentSchema;
        return node.Deserialize<SaveGame>(Json.Options) ?? throw new InvalidDataException("Save file could not be read.");
    }

    /// <summary>Migration from schema N to N+1, applied in order.</summary>
    private static readonly Dictionary<int, Action<JsonObject>> Migrations = new()
    {
        // v1 kept "seed" and "cameraMode" and had no bookmarks or edits.
        [1] = n =>
        {
            if (n["seed"] is { } seed) { n.Remove("seed"); n["worldSeed"] = seed.GetValue<string>(); }
            n["generator"] ??= "repoverse-gen/0";
            n["snapshots"] ??= new JsonObject();
            if (n["player"] is JsonObject p)
            {
                if (p["cameraMode"] is { } mode) { p.Remove("cameraMode"); p["mode"] = mode.GetValue<string>(); }
                p["bookmarks"] ??= new JsonArray();
            }
            n["edits"] ??= new JsonArray();
        },
    };
}
