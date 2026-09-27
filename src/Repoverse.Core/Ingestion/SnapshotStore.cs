using System.Text.Json;
using Repoverse.Core.Generation;
using Repoverse.Core.Model;

namespace Repoverse.Core.Ingestion;

public sealed record Snapshot
{
    public const int CurrentSchema = 1;
    public int SchemaVersion { get; init; } = CurrentSchema;
    public required DateTimeOffset TakenAt { get; init; }
    public required string Source { get; init; }
    public required IReadOnlyList<CanonicalRepository> Repositories { get; init; }
    public IReadOnlyList<IngestFailure> Failures { get; init; } = [];
}

/// <summary>
/// Last-known-good canonical state (§27). A refresh is merged per repository: repos that
/// failed this time keep their previous record and are reported stale, and an empty or
/// wholly failed refresh never replaces a populated snapshot.
/// </summary>
public static class SnapshotStore
{
    public static Snapshot? Load(string path) =>
        File.Exists(path) ? JsonSerializer.Deserialize<Snapshot>(File.ReadAllText(path), Json.Options) : null;

    public static void Save(string path, Snapshot snapshot)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(snapshot, Json.Options));
        File.Move(tmp, path, overwrite: true); // atomic replace: a crash never leaves half a snapshot
    }

    public sealed record MergeResult(Snapshot Snapshot, IReadOnlyList<string> Stale, bool KeptPrevious);

    public static MergeResult Merge(Snapshot? previous, IngestResult fresh, string source, DateTimeOffset now)
    {
        if (previous != null && fresh.Repositories.Count == 0)
            return new MergeResult(previous with { Failures = fresh.Failures }, previous.Repositories.Select(r => r.Id.Key).ToList(), true);

        var failed = fresh.Failures.Select(f => f.Repo.ToLowerInvariant()).ToHashSet();
        var byKey = fresh.Repositories.ToDictionary(r => r.Id.Key);
        var stale = new List<string>();
        if (previous != null)
            foreach (var old in previous.Repositories)
                // Keep old records for repos that failed or were not reached (e.g. rate limit mid-run).
                if (!byKey.ContainsKey(old.Id.Key) && (failed.Contains(old.Id.Key) || fresh.RateLimited))
                {
                    byKey[old.Id.Key] = old;
                    stale.Add(old.Id.Key);
                }

        var merged = new Snapshot
        {
            TakenAt = now,
            Source = source,
            Repositories = byKey.Values.OrderBy(r => r.Id.Key, StringComparer.Ordinal).ToList(),
            Failures = fresh.Failures,
        };
        return new MergeResult(merged, stale, false);
    }
}
