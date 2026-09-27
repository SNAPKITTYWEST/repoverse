namespace Repoverse.Core.Model;

/// <summary>Stable internal identity for a repository: lower-cased "owner/name".</summary>
public readonly record struct RepoId(string Owner, string Name)
{
    public string Key => $"{Owner.ToLowerInvariant()}/{Name.ToLowerInvariant()}";
    public override string ToString() => Key;

    public static RepoId Parse(string fullName)
    {
        var slash = fullName.IndexOf('/');
        if (slash <= 0 || slash == fullName.Length - 1)
            throw new FormatException($"Expected 'owner/name', got '{fullName}'.");
        return new RepoId(fullName[..slash], fullName[(slash + 1)..]);
    }
}

public enum TreeEntryKind { File, Directory }

public sealed record TreeEntry(string Path, TreeEntryKind Kind, long SizeBytes);

public sealed record Release(string Tag, DateTimeOffset PublishedAt, bool Prerelease);

/// <summary>
/// GitHub-sourced facts about a repository. Nothing in here is inferred by Repoverse;
/// derived values live in <see cref="DerivedRepository"/> with provenance.
/// </summary>
public sealed record CanonicalRepository
{
    public required RepoId Id { get; init; }
    public string? Description { get; init; }
    public string? PrimaryLanguage { get; init; }
    /// <summary>Bytes of code per language, as reported by GitHub's languages endpoint.</summary>
    public IReadOnlyDictionary<string, long> Languages { get; init; } = new Dictionary<string, long>();
    public IReadOnlyList<string> Topics { get; init; } = [];
    public long SizeKb { get; init; }
    public bool Archived { get; init; }
    public bool Fork { get; init; }
    public string DefaultBranch { get; init; } = "main";
    public DateTimeOffset CreatedAt { get; init; }
    public DateTimeOffset PushedAt { get; init; }
    public string? License { get; init; }
    public int OpenIssues { get; init; }
    public int OpenPullRequests { get; init; }
    public int Contributors { get; init; }
    public int CommitsLast90Days { get; init; }
    public IReadOnlyList<Release> Releases { get; init; } = [];
    public IReadOnlyList<TreeEntry> Tree { get; init; } = [];
    /// <summary>Other repos in the world this one depends on (from manifests / submodules).</summary>
    public IReadOnlyList<RepoId> DependsOn { get; init; } = [];
    /// <summary>Opaque version marker of the snapshot this record came from (e.g. pushed_at + ETag).</summary>
    public string SnapshotId { get; init; } = "";
}
