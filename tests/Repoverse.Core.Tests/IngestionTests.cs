using System.Net;
using System.Text;
using Repoverse.Core.Ingestion;

namespace Repoverse.Core.Tests;

public class IngestionTests : IDisposable
{
    private readonly string _cache = Path.Combine(Path.GetTempPath(), "repoverse-test-" + Guid.NewGuid().ToString("N"));
    public void Dispose() { if (Directory.Exists(_cache)) Directory.Delete(_cache, true); }

    /// <summary>Scripted GitHub: routes by path prefix, supports ETags and a rate-limit switch.</summary>
    private sealed class FakeGitHub : HttpMessageHandler
    {
        public readonly Dictionary<string, Func<HttpRequestMessage, HttpResponseMessage>> Routes = new();
        public readonly List<string> Seen = [];
        public bool RateLimited;

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
        {
            var path = request.RequestUri!.PathAndQuery;
            Seen.Add(path);
            if (RateLimited)
            {
                var r = new HttpResponseMessage(HttpStatusCode.Forbidden);
                r.Headers.Add("x-ratelimit-remaining", "0");
                r.Headers.Add("x-ratelimit-reset", "2000000000");
                return Task.FromResult(r);
            }
            var route = Routes.Where(kv => path.StartsWith(kv.Key)).OrderByDescending(kv => kv.Key.Length).Select(kv => kv.Value).FirstOrDefault();
            return Task.FromResult(route?.Invoke(request) ?? new HttpResponseMessage(HttpStatusCode.NotFound));
        }
    }

    private static HttpResponseMessage Json(string body, string? etag = null, string? link = null)
    {
        var r = new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(body, Encoding.UTF8, "application/json") };
        if (etag != null) r.Headers.ETag = new System.Net.Http.Headers.EntityTagHeaderValue(etag);
        if (link != null) r.Headers.Add("Link", link);
        return r;
    }

    private static string RepoJson(string name, bool fork = false) => $$"""
        {"full_name":"snapkitty/{{name}}","description":"{{name}} compiler","language":"Rust","topics":["wasm"],
         "size":120,"archived":false,"fork":{{(fork ? "true" : "false")}},"default_branch":"main",
         "created_at":"2024-01-01T00:00:00Z","pushed_at":"2026-09-01T00:00:00Z","license":{"spdx_id":"MIT"},"open_issues_count":3}
        """;

    private FakeGitHub Standard()
    {
        var gh = new FakeGitHub();
        gh.Routes["/users/snapkitty/repos?per_page=100"] = _ => Json($"[{RepoJson("alpha")},{RepoJson("forked", fork: true)}]",
            link: "<https://api.github.com/users/snapkitty/repos?page=2>; rel=\"next\", <https://api.github.com/users/snapkitty/repos?page=2>; rel=\"last\"");
        gh.Routes["/users/snapkitty/repos?page=2"] = _ => Json($"[{RepoJson("beta")}]");
        foreach (var n in new[] { "alpha", "beta" })
        {
            gh.Routes[$"/repos/snapkitty/{n}/languages"] = _ => Json("""{"Rust":9000,"Shell":100}""");
            gh.Routes[$"/repos/snapkitty/{n}/git/trees/"] = _ => Json("""{"tree":[{"path":"src","type":"tree"},{"path":"src/lower.rs","type":"blob","size":900},{"path":"vendor/x","type":"commit"}]}""");
            gh.Routes[$"/repos/snapkitty/{n}/releases"] = _ => Json("""[{"tag_name":"v1.0.0","published_at":"2026-08-01T00:00:00Z","prerelease":false},{"tag_name":"draft","published_at":null,"prerelease":false}]""");
            gh.Routes[$"/repos/snapkitty/{n}/commits"] = _ => Json("""[{},{},{},{}]""");
            gh.Routes[$"/repos/snapkitty/{n}/pulls"] = _ => Json("""[{}]""");
            gh.Routes[$"/repos/snapkitty/{n}/contributors"] = _ => Json("""[{},{}]""");
        }
        return gh;
    }

    [Fact]
    public async Task Ingests_all_pages_skips_forks_and_maps_fields()
    {
        var gh = Standard();
        var ingest = new GitHubIngest(new HttpClient(gh), _cache, token: null);
        var result = await ingest.IngestOwnerAsync("snapkitty");

        Assert.Empty(result.Failures);
        Assert.Equal(["alpha", "beta"], result.Repositories.Select(r => r.Id.Name));
        var a = result.Repositories[0];
        Assert.Equal(9000, a.Languages["Rust"]);
        Assert.Equal(2, a.OpenIssues); // 3 minus 1 open PR
        Assert.Equal(1, a.OpenPullRequests);
        Assert.Equal(4, a.CommitsLast90Days);
        Assert.Equal(2, a.Contributors);
        Assert.Equal("MIT", a.License);
        Assert.Single(a.Releases); // unpublished draft ignored
        Assert.Equal(2, a.Tree.Count); // submodule entry excluded
    }

    [Fact]
    public async Task One_failing_repository_does_not_stop_the_rest()
    {
        var gh = Standard();
        gh.Routes["/repos/snapkitty/alpha/languages"] = _ => new HttpResponseMessage(HttpStatusCode.InternalServerError);
        var result = await new GitHubIngest(new HttpClient(gh), _cache, null).IngestOwnerAsync("snapkitty");
        Assert.Equal("snapkitty/alpha", Assert.Single(result.Failures).Repo);
        Assert.Equal("beta", Assert.Single(result.Repositories).Id.Name);
    }

    [Fact]
    public async Task Empty_repository_yields_an_empty_tree()
    {
        var gh = Standard();
        gh.Routes["/repos/snapkitty/alpha/git/trees/"] = _ => new HttpResponseMessage(HttpStatusCode.Conflict);
        var result = await new GitHubIngest(new HttpClient(gh), _cache, null).IngestOwnerAsync("snapkitty");
        Assert.Empty(result.Failures);
        Assert.Empty(result.Repositories[0].Tree);
    }

    [Fact]
    public async Task Unchanged_resources_are_served_from_cache_via_etag()
    {
        var gh = Standard();
        int full = 0;
        gh.Routes["/repos/snapkitty/alpha/languages"] = req =>
        {
            if (req.Headers.IfNoneMatch.Any(t => t.Tag == "\"v1\"")) return new HttpResponseMessage(HttpStatusCode.NotModified);
            full++;
            return Json("""{"Rust":1234}""", etag: "\"v1\"");
        };
        var first = new GitHubIngest(new HttpClient(gh), _cache, null);
        await first.IngestOwnerAsync("snapkitty");
        var second = new GitHubIngest(new HttpClient(gh), _cache, null);
        var result = await second.IngestOwnerAsync("snapkitty");

        Assert.Equal(1, full);
        Assert.True(second.NotModified >= 1);
        Assert.Equal(1234, result.Repositories[0].Languages["Rust"]);
    }

    [Fact]
    public async Task Rate_limit_is_reported_not_thrown()
    {
        var gh = Standard();
        gh.RateLimited = true;
        var result = await new GitHubIngest(new HttpClient(gh), _cache, null).IngestOwnerAsync("snapkitty");
        Assert.True(result.RateLimited);
        Assert.Empty(result.Repositories);
    }

    [Fact]
    public async Task Offline_refresh_keeps_last_good_snapshot()
    {
        var gh = Standard();
        var good = await new GitHubIngest(new HttpClient(gh), _cache, null).IngestOwnerAsync("snapkitty");
        var saved = SnapshotStore.Merge(null, good, "test", DateTimeOffset.UnixEpoch).Snapshot;

        gh.RateLimited = true;
        var bad = await new GitHubIngest(new HttpClient(gh), _cache + "-2", null).IngestOwnerAsync("snapkitty");
        var merged = SnapshotStore.Merge(saved, bad, "test", DateTimeOffset.UnixEpoch.AddDays(1));

        Assert.True(merged.KeptPrevious);
        Assert.Equal(2, merged.Snapshot.Repositories.Count);
        Assert.Equal(2, merged.Stale.Count);
    }

    [Fact]
    public void Partial_refresh_keeps_failed_repositories_as_stale()
    {
        var prev = new Snapshot { TakenAt = default, Source = "t", Repositories = [TestRepos.Make("a"), TestRepos.Make("b")] };
        var fresh = new IngestResult([TestRepos.Make("a", commits: 99)], [new IngestFailure("snapkitty/b", "500")], false);
        var merged = SnapshotStore.Merge(prev, fresh, "t", default);
        Assert.Equal(99, merged.Snapshot.Repositories.Single(r => r.Id.Name == "a").CommitsLast90Days);
        Assert.Equal(["snapkitty/b"], merged.Stale);
    }

    [Fact]
    public void Deleted_repositories_drop_out_on_a_clean_refresh()
    {
        var prev = new Snapshot { TakenAt = default, Source = "t", Repositories = [TestRepos.Make("a"), TestRepos.Make("gone")] };
        var merged = SnapshotStore.Merge(prev, new IngestResult([TestRepos.Make("a")], [], false), "t", default);
        Assert.Equal(["a"], merged.Snapshot.Repositories.Select(r => r.Id.Name));
    }

    [Fact]
    public async Task Token_is_sent_but_redacted_from_errors()
    {
        var gh = Standard();
        string? auth = null;
        gh.Routes["/repos/snapkitty/alpha/languages"] = req => { auth = req.Headers.Authorization?.ToString(); return Json("{}"); };
        await new GitHubIngest(new HttpClient(gh), _cache, "ghp_abcdefghijklmnopqrstuvwxyz0123").IngestOwnerAsync("snapkitty");
        Assert.Equal("Bearer ghp_abcdefghijklmnopqrstuvwxyz0123", auth);
        Assert.Equal("failed with [REDACTED] here", GitHubIngest.Redact("failed with ghp_abcdefghijklmnopqrstuvwxyz0123 here"));
    }

    [Fact]
    public void Link_header_parsing_finds_next()
    {
        Assert.Equal("https://x/p2", GitHubIngest.NextLink("<https://x/p2>; rel=\"next\", <https://x/p9>; rel=\"last\""));
        Assert.Null(GitHubIngest.NextLink("<https://x/p1>; rel=\"prev\""));
        Assert.Null(GitHubIngest.NextLink(null));
    }
}
