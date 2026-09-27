using System.Text.Json;
using System.Text.Json.Serialization;
using Repoverse.Core.Model;

namespace Repoverse.Core.Generation;

/// <summary>
/// Hand-authored world design (§83). Overrides augment procedural output; any repo
/// not listed is generated normally.
/// </summary>
public sealed class Overrides
{
    public sealed record Entry
    {
        public SemanticCategory? Category { get; init; }
        public District? District { get; init; }
        public BuildingArchetype? Archetype { get; init; }
        public bool Landmark { get; init; }
    }

    private readonly Dictionary<string, Entry> _entries;
    public string Source { get; }

    public Overrides(IReadOnlyDictionary<string, Entry> entries, string source)
    {
        _entries = entries.ToDictionary(kv => kv.Key.ToLowerInvariant(), kv => kv.Value);
        Source = source;
    }

    public static Overrides Empty { get; } = new(new Dictionary<string, Entry>(), "(none)");

    public static Overrides Load(string path)
    {
        var entries = JsonSerializer.Deserialize<Dictionary<string, Entry>>(File.ReadAllText(path), Json.Options)
            ?? throw new InvalidDataException($"Override file '{path}' is empty.");
        return new Overrides(entries, Path.GetFileName(path));
    }

    private Entry? Get(RepoId id) => _entries.GetValueOrDefault(id.Key);
    public SemanticCategory? Category(RepoId id) => Get(id)?.Category;
    public District? District(RepoId id) => Get(id)?.District;
    public BuildingArchetype? Archetype(RepoId id) => Get(id)?.Archetype;
    public bool Landmark(RepoId id) => Get(id)?.Landmark ?? false;
}

/// <summary>Shared serializer settings: enums as strings so manifests stay human-readable and diffable.</summary>
public static class Json
{
    public static JsonSerializerOptions Options { get; } = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase), new RepoIdConverter() },
    };

    private sealed class RepoIdConverter : JsonConverter<RepoId>
    {
        public override RepoId Read(ref Utf8JsonReader reader, Type t, JsonSerializerOptions o) => RepoId.Parse(reader.GetString()!);
        public override void Write(Utf8JsonWriter writer, RepoId value, JsonSerializerOptions o) => writer.WriteStringValue($"{value.Owner}/{value.Name}");
    }
}
