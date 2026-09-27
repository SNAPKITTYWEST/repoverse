using System.Diagnostics;
using Repoverse.Core.Generation;
using Repoverse.Core.Model;

namespace Repoverse.Core.Ingestion;

/// <summary>
/// Builds canonical repositories from a local git checkout, with no network. With
/// <c>subprojects</c> each top-level directory becomes its own repository, which turns
/// a monorepo into a multi-building district.
/// </summary>
public static class LocalGitIngest
{
    public static IReadOnlyList<CanonicalRepository> Ingest(string repoPath, string owner, bool subprojects)
    {
        var files = Git(repoPath, "ls-files", "-z").Split('\0', StringSplitOptions.RemoveEmptyEntries);
        var head = Git(repoPath, "rev-parse", "HEAD").Trim();
        var since = $"--since={WorldConstants.ActivityWindowDays}.days";
        var tags = Git(repoPath, "for-each-ref", "--format=%(refname:short)|%(creatordate:iso-strict)", "refs/tags")
            .Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(l => l.Split('|'))
            .Select(p => new Release(p[0], DateTimeOffset.Parse(p[1]), p[0].Contains('-')))
            .ToList();

        CanonicalRepository Make(string name, string prefix, IEnumerable<string> paths)
        {
            var list = paths.ToList();
            var tree = new List<TreeEntry>();
            var dirs = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var p in list)
            {
                var rel = p[prefix.Length..];
                var full = Path.Combine(repoPath, p);
                tree.Add(new TreeEntry(rel, TreeEntryKind.File, File.Exists(full) ? new FileInfo(full).Length : 0));
                for (int i = rel.IndexOf('/'); i > 0; i = rel.IndexOf('/', i + 1)) dirs.Add(rel[..i]);
            }
            tree.AddRange(dirs.Select(d => new TreeEntry(d, TreeEntryKind.Directory, 0)));

            var languages = tree.Where(e => e.Kind == TreeEntryKind.File && !RoomGraphBuilder.IsIgnored(e.Path))
                .GroupBy(e => RoomGraphBuilder.LanguageFor(e.Path))
                .Where(g => g.Key is not ("Other" or "Markdown" or "JSON" or "TOML" or "YAML"))
                .ToDictionary(g => g.Key, g => g.Sum(e => e.SizeBytes));
            var scope = prefix.Length == 0 ? "." : prefix.TrimEnd('/');
            var authors = Git(repoPath, "log", "--format=%ae", "--", scope).Split('\n', StringSplitOptions.RemoveEmptyEntries).Distinct().Count();
            var recent = Git(repoPath, "rev-list", "--count", since, "HEAD", "--", scope).Trim();
            var dates = Git(repoPath, "log", "--format=%cI", "--", scope).Split('\n', StringSplitOptions.RemoveEmptyEntries);

            return new CanonicalRepository
            {
                Id = new RepoId(owner, name),
                Description = ReadmeSummary(repoPath, prefix, list),
                PrimaryLanguage = languages.OrderByDescending(kv => kv.Value).ThenBy(kv => kv.Key).Select(kv => kv.Key).FirstOrDefault(),
                Languages = languages,
                SizeKb = tree.Sum(e => e.SizeBytes) / 1024,
                CreatedAt = dates.Length > 0 ? DateTimeOffset.Parse(dates[^1]) : default,
                PushedAt = dates.Length > 0 ? DateTimeOffset.Parse(dates[0]) : default,
                Contributors = authors,
                CommitsLast90Days = int.TryParse(recent, out var n) ? n : 0,
                Releases = prefix.Length == 0 ? tags : [],
                Tree = tree,
                SnapshotId = $"git:{head}",
            };
        }

        if (!subprojects)
            return [Make(new DirectoryInfo(repoPath).Name, "", files)];

        return files.Where(f => f.Contains('/'))
            .GroupBy(f => f[..f.IndexOf('/')])
            .Where(g => !g.Key.StartsWith('.'))
            .OrderBy(g => g.Key, StringComparer.Ordinal)
            .Select(g => Make(g.Key, g.Key + "/", g))
            .ToList();
    }

    private static string? ReadmeSummary(string root, string prefix, List<string> files)
    {
        var readme = files.FirstOrDefault(f => string.Equals(f[prefix.Length..], "README.md", StringComparison.OrdinalIgnoreCase))
                     ?? files.FirstOrDefault(f => f.EndsWith(".md", StringComparison.OrdinalIgnoreCase) && !f[prefix.Length..].Contains('/'));
        if (readme == null) return null;
        string? prose = null;
        foreach (var line in File.ReadLines(Path.Combine(root, readme)).Take(60))
        {
            var t = line.Trim().TrimStart('#', ' ', '*', '>').Trim();
            // Agent reports open with "KEY: value" headers; TASK/MISSION/DESCRIPTION carry the purpose.
            var meta = MetadataLine.Match(t);
            if (meta.Success)
            {
                if (meta.Groups[1].Value is "TASK" or "MISSION" or "DESCRIPTION" or "SUMMARY") return Clip(meta.Groups[2].Value);
                continue;
            }
            if (prose == null && t.Length > 3 && !t.StartsWith("![") && !t.StartsWith('<') && !t.StartsWith("---") && !t.StartsWith("```"))
                prose = t;
        }
        return prose == null ? null : Clip(prose);
    }

    private static readonly System.Text.RegularExpressions.Regex MetadataLine = new(@"^([A-Z][A-Z_]+):\s*(.*)$");

    private static string Clip(string s) => s.Length > 160 ? s[..160] : s;

    private static string Git(string dir, params string[] args)
    {
        var psi = new ProcessStartInfo("git") { WorkingDirectory = dir, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("git is not available.");
        var output = p.StandardOutput.ReadToEnd();
        var err = p.StandardError.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new InvalidOperationException($"git {string.Join(' ', args)} failed: {err.Trim()}");
        return output;
    }
}
