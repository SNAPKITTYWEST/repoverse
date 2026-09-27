using Repoverse.Core.Model;

namespace Repoverse.Core.Simulation;

/// <summary>Typed world events (§32). Each carries the data needed to visualise it.</summary>
public abstract record WorldEvent(RepoId Repo);

public sealed record RepositoryAdded(RepoId Repo) : WorldEvent(Repo);
public sealed record RepositoryRemoved(RepoId Repo) : WorldEvent(Repo);
public sealed record RepositoryArchived(RepoId Repo) : WorldEvent(Repo);
public sealed record RepositoryUnarchived(RepoId Repo) : WorldEvent(Repo);
public sealed record ReleasePublished(RepoId Repo, string Tag, bool Prerelease) : WorldEvent(Repo);
public sealed record IssuesChanged(RepoId Repo, int Before, int After) : WorldEvent(Repo);
public sealed record PullRequestsChanged(RepoId Repo, int Before, int After) : WorldEvent(Repo);
public sealed record ActivityChanged(RepoId Repo, int CommitsBefore, int CommitsAfter) : WorldEvent(Repo);
public sealed record StructureChanged(RepoId Repo, int FilesAdded, int FilesRemoved) : WorldEvent(Repo);

/// <summary>Turns two canonical snapshots into the events that justify world changes (§26).</summary>
public static class SnapshotDiff
{
    public static IReadOnlyList<WorldEvent> Diff(IReadOnlyList<CanonicalRepository> before, IReadOnlyList<CanonicalRepository> after)
    {
        var old = before.ToDictionary(r => r.Id.Key);
        var now = after.ToDictionary(r => r.Id.Key);
        var events = new List<WorldEvent>();

        foreach (var key in old.Keys.Union(now.Keys).OrderBy(k => k, StringComparer.Ordinal))
        {
            old.TryGetValue(key, out var a);
            now.TryGetValue(key, out var b);
            if (a == null) { events.Add(new RepositoryAdded(b!.Id)); continue; }
            if (b == null) { events.Add(new RepositoryRemoved(a.Id)); continue; }

            if (!a.Archived && b.Archived) events.Add(new RepositoryArchived(b.Id));
            if (a.Archived && !b.Archived) events.Add(new RepositoryUnarchived(b.Id));

            var oldTags = a.Releases.Select(r => r.Tag).ToHashSet();
            foreach (var r in b.Releases.Where(r => !oldTags.Contains(r.Tag)).OrderBy(r => r.PublishedAt))
                events.Add(new ReleasePublished(b.Id, r.Tag, r.Prerelease));

            if (a.OpenIssues != b.OpenIssues) events.Add(new IssuesChanged(b.Id, a.OpenIssues, b.OpenIssues));
            if (a.OpenPullRequests != b.OpenPullRequests) events.Add(new PullRequestsChanged(b.Id, a.OpenPullRequests, b.OpenPullRequests));
            if (a.CommitsLast90Days != b.CommitsLast90Days) events.Add(new ActivityChanged(b.Id, a.CommitsLast90Days, b.CommitsLast90Days));

            var aFiles = a.Tree.Where(e => e.Kind == TreeEntryKind.File).Select(e => e.Path).ToHashSet();
            var bFiles = b.Tree.Where(e => e.Kind == TreeEntryKind.File).Select(e => e.Path).ToHashSet();
            int added = bFiles.Count(f => !aFiles.Contains(f)), removed = aFiles.Count(f => !bFiles.Contains(f));
            if (added + removed > 0) events.Add(new StructureChanged(b.Id, added, removed));
        }
        return events;
    }
}
