using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Repoverse.Core.Bridge;
using Repoverse.Core.Generation;
using Repoverse.Core.Persistence;

// Local-only service. One Unreal client owns a session; reconnect starts a new stream.
var builder = WebApplication.CreateBuilder(args);
string Required(string name) => builder.Configuration[name] ?? throw new ArgumentException($"--{name} is required");
var manifest = JsonSerializer.Deserialize<WorldManifest>(File.ReadAllText(Required("manifest")), Repoverse.Core.Generation.Json.Options)
    ?? throw new InvalidDataException("Empty manifest");
var token = Required("token");
if (token.Length < 32) throw new ArgumentException("Use a random token of at least 32 characters.");
var tokenHash = SHA256.HashData(Encoding.UTF8.GetBytes(token));
int port = int.Parse(builder.Configuration["port"] ?? "38473");
if (port is < 1024 or > 65535) throw new ArgumentException("Invalid port");
builder.WebHost.UseUrls($"http://127.0.0.1:{port}");
builder.WebHost.ConfigureKestrel(k => k.Limits.MaxRequestBodySize = 64 * 1024);
builder.Services.ConfigureHttpJsonOptions(o => {
    o.SerializerOptions.PropertyNamingPolicy = Repoverse.Core.Generation.Json.Options.PropertyNamingPolicy;
    foreach (var c in Repoverse.Core.Generation.Json.Options.Converters) o.SerializerOptions.Converters.Add(c);
});
var app = builder.Build();
var gate = new SemaphoreSlim(1,1);
var session = new WorldSession(manifest);
var savePath = Path.GetFullPath(builder.Configuration["save"] ?? "repoverse-save.json");
var sourceRoot = builder.Configuration["source-root"];
var sourceRepo = builder.Configuration["source-repo"];
var sources = sourceRoot == null || sourceRepo == null ? null : new LocalSourceReader(manifest, sourceRepo, sourceRoot);
app.Use(async (context,next) => {
    // No CORS: web pages cannot use this service. Authenticate even read-only requests.
    var supplied = SHA256.HashData(Encoding.UTF8.GetBytes(context.Request.Headers["X-Repoverse-Token"].ToString()));
    if (context.Request.Headers.ContainsKey("Origin") || !CryptographicOperations.FixedTimeEquals(tokenHash,supplied)) {
        context.Response.StatusCode=403; return;
    }
    await gate.WaitAsync(context.RequestAborted);
    try { await next(context); }
    catch (Exception e) when (e is ArgumentException or InvalidDataException or JsonException or FormatException or IOException) {
        context.Response.StatusCode=400;
        await context.Response.WriteAsJsonAsync(new { error=e.Message });
    }
    finally { gate.Release(); }
});
app.MapPost("/v1/session", () => {
    var player = session.Player;
    var saved = session.Capture();
    session = new WorldSession(manifest); session.Restore(saved);
    return new { schemaVersion=1, worldSeed=manifest.WorldSeed, player, destinations=session.Destinations, objects=session.Objects(), builders=session.Builders };
});
app.MapPost("/v1/focus", (FocusRequest r) => {var p=session.Focus(r.X,r.Z);return new {p.Unloaded,p.Chunks,p.Remaining,p.Resident,builders=session.Builders};});
app.MapPost("/v1/build", (BuilderPlan plan) => session.Build(plan));
app.MapPost("/v1/save", (PlayerState player) => {
    session.SetPlayer(player);
    Directory.CreateDirectory(Path.GetDirectoryName(savePath)!);
    SaveGame.Write(savePath,session.Capture());
    return new { saved=true };
});
app.MapPost("/v1/load", () => { session.Restore(SaveGame.Read(savePath)); return session.Player; });
app.MapPost("/v1/edit", (EditRequest r) => {session.Edit(r.X,r.Y,r.Z,r.Packed);return new { edited=true };});
app.MapGet("/v1/source", (string repo, string path, int offset = 0) => sources is null
    ? Results.BadRequest(new { error="Local source inspection is not configured. Use --source-root and --source-repo." })
    : Results.Ok(sources.Read(repo,path,offset)));
app.MapGet("/v1/health", () => new { schemaVersion=1, resident=session.Resident });
await app.RunAsync();
record FocusRequest(double X,double Z);
record EditRequest(int X,int Y,int Z,ushort Packed);
