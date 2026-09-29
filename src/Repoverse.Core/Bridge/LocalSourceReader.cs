using Repoverse.Core.Generation;

namespace Repoverse.Core.Bridge;

public sealed record SourcePage(string Path, int Offset, string[] Lines, bool HasMore);
/// <summary>Read-only, bounded source pages for artifacts in one explicitly mapped local repository.</summary>
public sealed class LocalSourceReader(WorldManifest manifest, string repo, string sourceRoot)
{
    private readonly string _root = Path.GetFullPath(sourceRoot);
    private readonly HashSet<string> _allowed = manifest.Buildings.Where(b => b.Repo.Key == repo.ToLowerInvariant())
        .SelectMany(b => b.Interior.Rooms).SelectMany(r => r.Artifacts).Select(a => a.Path).ToHashSet(StringComparer.Ordinal);
    public SourcePage Read(string requestedRepo, string relative, int offset)
    {
        if (!requestedRepo.Equals(repo,StringComparison.OrdinalIgnoreCase) || !_allowed.Contains(relative))
            throw new ArgumentException("Source is not an artifact in the mapped repository.");
        if (offset is < 0 or > 100_000 || Path.IsPathRooted(relative)) throw new ArgumentException("Invalid source page.");
        var parts = relative.Replace('\\','/').Split('/');
        if (parts.Any(p => p is "" or "." or ".." || p.Contains(':'))) throw new ArgumentException("Invalid source path.");
        // Reject links/junctions at every component, including the configured root.
        var target = _root;
        if ((File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Linked source roots are unsupported.");
        foreach (var part in parts) {
            target = Path.Combine(target,part);
            if ((File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Linked source paths are unsupported.");
        }
        if (new FileInfo(target).Length > 2*1024*1024) throw new ArgumentException("Source exceeds the 2 MiB inspection limit.");
        var text = File.ReadAllText(target);
        if (text.Contains('\0')) throw new ArgumentException("Binary artifact; open it in its native application.");
        var lines = text.Replace("\r\n","\n").Split('\n');
        return new(relative,offset,lines.Skip(offset).Take(32).Select(s => s.Length>240 ? s[..240]+"..." : s).ToArray(),offset+32<lines.Length);
    }
}
