using Repoverse.Core.Model;

namespace Repoverse.Core;

/// <summary>
/// Deep link into the world (§76). Grammar, version 1:
/// <code>
/// repoverse://{owner}/{repo}[/{path}][#L{line}]
/// repoverse://{owner}/{repo}?room={id}
/// repoverse://@{x},{y},{z}
/// </code>
/// Path segments are percent-encoded; the repo part matches GitHub's owner/name.
/// </summary>
public sealed record RepoverseUri
{
    public const string Scheme = "repoverse";

    public RepoId? Repo { get; init; }
    public string? Path { get; init; }
    public int? Line { get; init; }
    public int? Room { get; init; }
    public (int X, int Y, int Z)? Location { get; init; }

    public static RepoverseUri Parse(string text)
    {
        const string prefix = Scheme + "://";
        if (!text.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            throw new FormatException($"Not a {Scheme} URI: '{text}'.");
        var rest = text[prefix.Length..];

        if (rest.StartsWith('@'))
        {
            var p = rest[1..].Split(',');
            if (p.Length != 3) throw new FormatException("Location must be @x,y,z.");
            return new RepoverseUri { Location = (int.Parse(p[0]), int.Parse(p[1]), int.Parse(p[2])) };
        }

        int? line = null, room = null;
        var hash = rest.IndexOf('#');
        if (hash >= 0)
        {
            var frag = rest[(hash + 1)..];
            if (!frag.StartsWith('L') || !int.TryParse(frag[1..], out var l)) throw new FormatException($"Bad fragment '#{frag}'.");
            line = l;
            rest = rest[..hash];
        }
        var q = rest.IndexOf('?');
        if (q >= 0)
        {
            foreach (var kv in rest[(q + 1)..].Split('&'))
                if (kv.StartsWith("room=") && int.TryParse(kv[5..], out var r)) room = r;
                else throw new FormatException($"Unknown query '{kv}'.");
            rest = rest[..q];
        }

        var parts = rest.Split('/', 3);
        if (parts.Length < 2 || parts[0].Length == 0 || parts[1].Length == 0) throw new FormatException("Expected owner/repo.");
        var path = parts.Length == 3 && parts[2].Length > 0
            ? string.Join('/', parts[2].Split('/').Select(Uri.UnescapeDataString))
            : null;
        return new RepoverseUri { Repo = new RepoId(Uri.UnescapeDataString(parts[0]), Uri.UnescapeDataString(parts[1])), Path = path, Line = line, Room = room };
    }

    public override string ToString()
    {
        if (Location is var (x, y, z)) return $"{Scheme}://@{x},{y},{z}";
        var s = $"{Scheme}://{Uri.EscapeDataString(Repo!.Value.Owner)}/{Uri.EscapeDataString(Repo.Value.Name)}";
        if (Path != null) s += "/" + string.Join('/', Path.Split('/').Select(Uri.EscapeDataString));
        if (Room != null) s += $"?room={Room}";
        if (Line != null) s += $"#L{Line}";
        return s;
    }

    /// <summary>External reference back to GitHub (§77); internal identity stays the RepoId.</summary>
    public string? GitHubUrl(string branch = "HEAD") => Repo is not { } r ? null
        : Path == null ? $"https://github.com/{r.Owner}/{r.Name}"
        : $"https://github.com/{r.Owner}/{r.Name}/blob/{branch}/{Path}{(Line is { } l ? $"#L{l}" : "")}";
}
