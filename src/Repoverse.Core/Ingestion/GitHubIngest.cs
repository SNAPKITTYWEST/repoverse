using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Repoverse.Core.Model;

namespace Repoverse.Core.Ingestion;

public sealed class RateLimitedException(DateTimeOffset resetsAt) : Exception($"GitHub rate limit reached; resets at {resetsAt:u}.")
{
    public DateTimeOffset ResetsAt { get; } = resetsAt;
}

public sealed record IngestFailure(string Repo, string Reason);

public sealed record IngestResult(IReadOnlyList<CanonicalRepository> Repositories, IReadOnlyList<IngestFailure> Failures, bool RateLimited);

/// <summary>
/// The only place Repoverse talks to GitHub (§25). Responses are cached on disk with their
/// ETag and re-requested conditionally, so unchanged data costs no rate-limit budget.
/// A repo that fails to ingest is reported and skipped; it never aborts the whole run.
/// </summary>
public sealed class GitHubIngest
{
    private readonly HttpClient _http;
    private readonly string _cacheDir;
    private readonly TimeProvider _clock;

    /// <param name="token">Read from the environment or a secret store by the caller; never persisted (§41).</param>
    public GitHubIngest(HttpClient http, string cacheDir, string? token, TimeProvider? clock = null)
    {
        _http = http;
        _cacheDir = cacheDir;
        _clock = clock ?? TimeProvider.System;
        Directory.CreateDirectory(cacheDir);
        _http.BaseAddress ??= new Uri("https://api.github.com/");
        _http.DefaultRequestHeaders.UserAgent.TryParseAdd("repoverse/1");
        _http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        if (!string.IsNullOrEmpty(token))
            _http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
    }

    public int RequestsSent { get; private set; }
    public int NotModified { get; private set; }

    public async Task<IngestResult> IngestOwnerAsync(string owner, bool includeForks = false, int? limit = null, CancellationToken ct = default)
    {
        List<JsonElement> listing;
        try
        {
            // /users/{owner}/repos works for both users and organisations.
            listing = await GetPagedAsync($"users/{Uri.EscapeDataString(owner)}/repos?per_page=100&type=owner&sort=full_name", ct);
        }
        catch (RateLimitedException)
        {
            return new IngestResult([], [new IngestFailure(owner, "rate limited while listing repositories")], true);
        }

        var repos = new List<CanonicalRepository>();
        var failures = new List<IngestFailure>();
        foreach (var r in listing.Where(r => includeForks || !r.GetProperty("fork").GetBoolean()).Take(limit ?? int.MaxValue))
        {
            var fullName = r.GetProperty("full_name").GetString()!;
            try
            {
                repos.Add(await IngestRepoAsync(r, ct));
            }
            catch (RateLimitedException e)
            {
                failures.Add(new IngestFailure(fullName, e.Message));
                return new IngestResult(repos, failures, true);
            }
            catch (Exception e) when (e is HttpRequestException or JsonException or KeyNotFoundException or InvalidOperationException)
            {
                failures.Add(new IngestFailure(fullName, Redact(e.Message)));
            }
        }
        return new IngestResult(repos, failures, false);
    }

    private async Task<CanonicalRepository> IngestRepoAsync(JsonElement r, CancellationToken ct)
    {
        var id = RepoId.Parse(r.GetProperty("full_name").GetString()!);
        var path = $"repos/{Uri.EscapeDataString(id.Owner)}/{Uri.EscapeDataString(id.Name)}";
        var branch = r.TryGetProperty("default_branch", out var db) && db.ValueKind == JsonValueKind.String ? db.GetString()! : "main";
        var since = _clock.GetUtcNow().AddDays(-WorldConstants.ActivityWindowDays).ToString("yyyy-MM-ddTHH:mm:ssZ");

        var languages = (await GetAsync($"{path}/languages", ct)) is { ValueKind: JsonValueKind.Object } langs
            ? langs.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.GetInt64())
            : new Dictionary<string, long>();

        var tree = new List<TreeEntry>();
        // Empty repositories answer 409 here; treat as an empty tree.
        if (await GetAsync($"{path}/git/trees/{Uri.EscapeDataString(branch)}?recursive=1", ct, allowStatus: HttpStatusCode.Conflict) is { ValueKind: JsonValueKind.Object } t
            && t.TryGetProperty("tree", out var entries))
        {
            foreach (var e in entries.EnumerateArray())
            {
                var type = e.GetProperty("type").GetString();
                if (type is not ("blob" or "tree")) continue; // submodules ("commit") are dependencies, not rooms
                tree.Add(new TreeEntry(e.GetProperty("path").GetString()!,
                    type == "tree" ? TreeEntryKind.Directory : TreeEntryKind.File,
                    e.TryGetProperty("size", out var s) ? s.GetInt64() : 0));
            }
        }

        var releases = (await GetPagedAsync($"{path}/releases?per_page=100", ct, maxPages: 1))
            .Where(x => x.TryGetProperty("published_at", out var p) && p.ValueKind == JsonValueKind.String)
            .Select(x => new Release(x.GetProperty("tag_name").GetString()!, x.GetProperty("published_at").GetDateTimeOffset(), x.GetProperty("prerelease").GetBoolean()))
            .ToList();
        var commits = await GetPagedAsync($"{path}/commits?since={since}&per_page=100", ct, maxPages: 3, allowStatus: HttpStatusCode.Conflict);
        var pulls = await GetPagedAsync($"{path}/pulls?state=open&per_page=100", ct, maxPages: 3);
        var contributors = await GetPagedAsync($"{path}/contributors?per_page=100", ct, maxPages: 1, allowStatus: HttpStatusCode.NoContent);

        var pushed = r.TryGetProperty("pushed_at", out var pa) && pa.ValueKind == JsonValueKind.String ? pa.GetDateTimeOffset() : default;
        return new CanonicalRepository
        {
            Id = id,
            Description = r.TryGetProperty("description", out var d) && d.ValueKind == JsonValueKind.String ? d.GetString() : null,
            PrimaryLanguage = r.TryGetProperty("language", out var l) && l.ValueKind == JsonValueKind.String ? l.GetString() : null,
            Languages = languages,
            Topics = r.TryGetProperty("topics", out var tp) ? tp.EnumerateArray().Select(x => x.GetString()!).ToList() : [],
            SizeKb = r.GetProperty("size").GetInt64(),
            Archived = r.GetProperty("archived").GetBoolean(),
            Fork = r.GetProperty("fork").GetBoolean(),
            DefaultBranch = branch,
            CreatedAt = r.GetProperty("created_at").GetDateTimeOffset(),
            PushedAt = pushed,
            License = r.TryGetProperty("license", out var li) && li.ValueKind == JsonValueKind.Object ? li.GetProperty("spdx_id").GetString() : null,
            // GitHub's open_issues_count includes PRs.
            OpenIssues = Math.Max(0, r.GetProperty("open_issues_count").GetInt32() - pulls.Count),
            OpenPullRequests = pulls.Count,
            Contributors = contributors.Count,
            CommitsLast90Days = commits.Count,
            Releases = releases,
            Tree = tree,
            SnapshotId = $"{pushed:O}",
        };
    }

    /// <summary>Follows rel="next" Link headers up to <paramref name="maxPages"/>.</summary>
    private async Task<List<JsonElement>> GetPagedAsync(string url, CancellationToken ct, int maxPages = 50, HttpStatusCode? allowStatus = null)
    {
        var items = new List<JsonElement>();
        string? next = url;
        for (int page = 0; next != null && page < maxPages; page++)
        {
            var (json, link) = await SendAsync(next, ct, allowStatus);
            if (json is { ValueKind: JsonValueKind.Array } arr) items.AddRange(arr.EnumerateArray());
            next = NextLink(link);
        }
        return items;
    }

    private async Task<JsonElement?> GetAsync(string url, CancellationToken ct, HttpStatusCode? allowStatus = null) =>
        (await SendAsync(url, ct, allowStatus)).Json;

    private async Task<(JsonElement? Json, string? Link)> SendAsync(string url, CancellationToken ct, HttpStatusCode? allowStatus)
    {
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(url)))[..32];
        var bodyPath = Path.Combine(_cacheDir, key + ".json");
        var metaPath = Path.Combine(_cacheDir, key + ".meta");
        string[] meta = File.Exists(metaPath) ? await File.ReadAllLinesAsync(metaPath, ct) : [];

        using var req = new HttpRequestMessage(HttpMethod.Get, url);
        if (meta.Length > 0 && meta[0].Length > 0 && File.Exists(bodyPath))
            req.Headers.TryAddWithoutValidation("If-None-Match", meta[0]);

        RequestsSent++;
        using var res = await _http.SendAsync(req, ct);
        if (res.StatusCode == HttpStatusCode.NotModified)
        {
            NotModified++;
            return (Parse(await File.ReadAllTextAsync(bodyPath, ct)), meta.Length > 1 ? meta[1] : null);
        }
        if (res.StatusCode is HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests
            && res.Headers.TryGetValues("x-ratelimit-remaining", out var rem) && rem.FirstOrDefault() == "0")
        {
            var reset = res.Headers.TryGetValues("x-ratelimit-reset", out var rs) && long.TryParse(rs.FirstOrDefault(), out var epoch)
                ? DateTimeOffset.FromUnixTimeSeconds(epoch) : _clock.GetUtcNow().AddHours(1);
            throw new RateLimitedException(reset);
        }
        if (allowStatus == res.StatusCode) return (null, null);
        if (!res.IsSuccessStatusCode)
            throw new HttpRequestException($"GET {url} → {(int)res.StatusCode} {res.ReasonPhrase}", null, res.StatusCode);

        var body = await res.Content.ReadAsStringAsync(ct);
        var link = res.Headers.TryGetValues("Link", out var lv) ? string.Join(",", lv) : null;
        await File.WriteAllTextAsync(bodyPath, body, ct);
        await File.WriteAllLinesAsync(metaPath, [res.Headers.ETag?.ToString() ?? "", link ?? ""], ct);
        return (Parse(body), link);
    }

    private static JsonElement? Parse(string body) => string.IsNullOrWhiteSpace(body) ? null : JsonDocument.Parse(body).RootElement.Clone();

    public static string? NextLink(string? link)
    {
        if (link == null) return null;
        foreach (var part in link.Split(','))
        {
            var bits = part.Split(';');
            if (bits.Length > 1 && bits[1..].Any(b => b.Trim() == "rel=\"next\""))
                return bits[0].Trim().TrimStart('<').TrimEnd('>');
        }
        return null;
    }

    /// <summary>Strips anything token-shaped from text that may end up in logs (§41).</summary>
    public static string Redact(string text) =>
        System.Text.RegularExpressions.Regex.Replace(text, @"(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|Bearer\s+\S+)", "[REDACTED]");
}
