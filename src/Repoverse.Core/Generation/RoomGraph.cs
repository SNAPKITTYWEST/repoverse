using Repoverse.Core.Model;

namespace Repoverse.Core.Generation;

public enum RoomPurpose { Lobby, Corridor, Stairwell, Workshop, InspectionLab, Archive, ProofVault, Assembly, Storage, Toolroom, Commons }

/// <summary>Software concept a world object stands for (§53).</summary>
public enum ArtifactKind { SourceTerminal, TestBench, ProofVault, ArchiveShelf, BuildStation, DataRack, ConfigPanel, AssetCrate, NotebookDesk, HardwareBench }

public sealed record Artifact(string Path, ArtifactKind Kind, long SizeBytes, string Language);

public sealed record Room
{
    public required int Id { get; init; }
    public required string Name { get; init; }
    public required RoomPurpose Purpose { get; init; }
    public required int Floor { get; init; }
    /// <summary>Repository directories this room represents; several when small dirs were merged.</summary>
    public IReadOnlyList<string> Directories { get; init; } = [];
    public int FileCount { get; init; }
    public IReadOnlyList<Artifact> Artifacts { get; init; } = [];
}

public sealed record Door(int A, int B);

/// <summary>Logical interior (§88): rooms and their connections, before any geometry exists.</summary>
public sealed record RoomGraph(IReadOnlyList<Room> Rooms, IReadOnlyList<Door> Doors)
{
    public int EntranceId => Rooms.First(r => r.Purpose == RoomPurpose.Lobby).Id;

    /// <summary>Rooms not reachable from the entrance. Empty for a valid graph (§89).</summary>
    public IReadOnlyList<Room> Unreachable()
    {
        var adj = Rooms.ToDictionary(r => r.Id, _ => new List<int>());
        foreach (var d in Doors) { adj[d.A].Add(d.B); adj[d.B].Add(d.A); }
        var seen = new HashSet<int> { EntranceId };
        var queue = new Queue<int>(seen);
        while (queue.Count > 0)
            foreach (var n in adj[queue.Dequeue()])
                if (seen.Add(n)) queue.Enqueue(n);
        return Rooms.Where(r => !seen.Contains(r.Id)).ToList();
    }
}

public static class RoomGraphBuilder
{
    public static bool IsIgnored(string path)
    {
        foreach (var segment in path.Split('/'))
            if (WorldConstants.IgnoredDirectories.Contains(segment, StringComparer.OrdinalIgnoreCase))
                return true;
        return false;
    }

    public static RoomPurpose PurposeFor(string directory)
    {
        var d = directory.ToLowerInvariant();
        var leaf = d[(d.LastIndexOf('/') + 1)..];
        return leaf switch
        {
            "test" or "tests" or "spec" or "specs" or "__tests__" or "testing" => RoomPurpose.InspectionLab,
            "doc" or "docs" or "documentation" or "papers" or "notes" => RoomPurpose.Archive,
            "proof" or "proofs" or "formal" or "theories" or "verification" => RoomPurpose.ProofVault,
            ".github" or "ci" or ".circleci" or "workflows" or "deploy" => RoomPurpose.Assembly,
            "assets" or "data" or "fixtures" or "demo" or "screenshots" or "resources" => RoomPurpose.Storage,
            "scripts" or "tools" or "bin" or "cmd" => RoomPurpose.Toolroom,
            _ => RoomPurpose.Workshop,
        };
    }

    public static ArtifactKind KindFor(string path)
    {
        var p = path.ToLowerInvariant();
        var name = p[(p.LastIndexOf('/') + 1)..];
        if (name.Contains("test") || name.Contains("spec") || p.Split('/')[..^1].Any(d => d is "test" or "tests" or "spec" or "specs" or "__tests__"))
            return ArtifactKind.TestBench;
        if (name is "makefile" or "cargo.toml" or "package.json" or "cmakelists.txt" or "dockerfile" or "project.toml"
            || name.EndsWith(".csproj") || name.EndsWith(".cabal") || name.EndsWith(".sln") || p.Contains(".github/workflows/"))
            return ArtifactKind.BuildStation;
        var ext = Path.GetExtension(name);
        return ext switch
        {
            ".lean" or ".v" or ".agda" or ".idr" or ".thy" or ".tla" => ArtifactKind.ProofVault,
            ".md" or ".rst" or ".txt" or ".pdf" or ".tex" => ArtifactKind.ArchiveShelf,
            ".json" or ".csv" or ".parquet" or ".sqlite" or ".db" or ".vox" => ArtifactKind.DataRack,
            ".toml" or ".yaml" or ".yml" or ".ini" or ".cfg" or ".lock" => ArtifactKind.ConfigPanel,
            ".png" or ".jpg" or ".svg" or ".zip" or ".wav" or ".glb" => ArtifactKind.AssetCrate,
            ".ipynb" => ArtifactKind.NotebookDesk,
            ".sv" or ".vhd" or ".vhdl" or ".metal" or ".asm" or ".s" => ArtifactKind.HardwareBench,
            _ => ArtifactKind.SourceTerminal,
        };
    }

    public static string LanguageFor(string path) => Path.GetExtension(path).ToLowerInvariant() switch
    {
        ".cs" => "C#", ".rs" => "Rust", ".py" => "Python", ".hs" => "Haskell", ".jl" => "Julia",
        ".js" or ".mjs" or ".cjs" => "JavaScript", ".ts" or ".tsx" => "TypeScript", ".swift" => "Swift",
        ".c" or ".h" => "C", ".cpp" or ".cc" or ".hpp" => "C++", ".go" => "Go", ".lean" => "Lean",
        ".asm" or ".s" => "Assembly", ".metal" => "Metal", ".m" => "Objective-C", ".pl" => "Prolog",
        ".html" => "HTML", ".css" => "CSS", ".md" => "Markdown", ".json" => "JSON", ".toml" => "TOML",
        ".yml" or ".yaml" => "YAML", ".idr" => "Idris", ".zig" => "Zig", ".java" => "Java", ".kt" => "Kotlin",
        _ => "Other",
    };

    /// <summary>
    /// Groups the tree into rooms by top-level directory (then second level for large ones),
    /// merges tiny directories into a Commons room, and lays rooms out over floors joined
    /// by a stairwell. Every room hangs off its floor's corridor, so the graph is connected
    /// by construction; <see cref="RoomGraph.Unreachable"/> verifies it.
    /// </summary>
    public static RoomGraph Build(CanonicalRepository repo, int roomsPerFloor)
    {
        var files = repo.Tree
            .Where(e => e.Kind == TreeEntryKind.File && !IsIgnored(e.Path))
            .OrderBy(e => e.Path, StringComparer.Ordinal)
            .ToList();

        // Bucket by top-level directory; split big buckets one level deeper.
        var buckets = new SortedDictionary<string, List<TreeEntry>>(StringComparer.Ordinal);
        void Add(string key, TreeEntry e)
        {
            if (!buckets.TryGetValue(key, out var list)) buckets[key] = list = [];
            list.Add(e);
        }
        var byTop = files.GroupBy(f => f.Path.Contains('/') ? f.Path[..f.Path.IndexOf('/')] : "");
        foreach (var group in byTop)
        {
            var items = group.ToList();
            bool split = group.Key != "" && items.Count > WorldConstants.MaxArtifactsPerRoom;
            foreach (var e in items)
            {
                if (!split) { Add(group.Key, e); continue; }
                var rest = e.Path[(group.Key.Length + 1)..];
                Add(rest.Contains('/') ? $"{group.Key}/{rest[..rest.IndexOf('/')]}" : group.Key, e);
            }
        }

        var roomDirs = new List<(string Name, List<string> Dirs, List<TreeEntry> Files)>();
        var commons = (Name: "Commons", Dirs: new List<string>(), Files: new List<TreeEntry>());
        foreach (var (dir, list) in buckets)
        {
            if (dir == "" || list.Count < WorldConstants.MinFilesForOwnRoom) { commons.Dirs.Add(dir == "" ? "/" : dir); commons.Files.AddRange(list); }
            else roomDirs.Add((dir, [dir], list));
        }
        // Semantic compression: keep the biggest rooms, fold the tail into Commons.
        var budget = WorldConstants.MaxRoomsPerBuilding - 1;
        foreach (var extra in roomDirs.OrderByDescending(r => r.Files.Count).ThenBy(r => r.Name, StringComparer.Ordinal).Skip(budget).ToList())
        {
            roomDirs.Remove(extra);
            commons.Dirs.AddRange(extra.Dirs);
            commons.Files.AddRange(extra.Files);
        }
        if (commons.Files.Count > 0) roomDirs.Add(commons);

        var rooms = new List<Room>();
        var doors = new List<Door>();
        int nextId = 0;
        var lobby = new Room { Id = nextId++, Name = "Lobby", Purpose = RoomPurpose.Lobby, Floor = 0 };
        rooms.Add(lobby);

        int floors = Math.Max(1, (int)Math.Ceiling(roomDirs.Count / (double)roomsPerFloor));
        int prevStair = -1;
        for (int f = 0; f < floors; f++)
        {
            var corridor = new Room { Id = nextId++, Name = $"Corridor {f}", Purpose = RoomPurpose.Corridor, Floor = f };
            var stair = new Room { Id = nextId++, Name = $"Stairwell {f}", Purpose = RoomPurpose.Stairwell, Floor = f };
            rooms.Add(corridor); rooms.Add(stair);
            doors.Add(new Door(corridor.Id, stair.Id));
            if (f == 0) doors.Add(new Door(lobby.Id, corridor.Id));
            if (prevStair >= 0) doors.Add(new Door(prevStair, stair.Id));
            prevStair = stair.Id;

            foreach (var (name, dirs, list) in roomDirs.Skip(f * roomsPerFloor).Take(roomsPerFloor))
            {
                var room = new Room
                {
                    Id = nextId++,
                    Name = name,
                    Purpose = name == "Commons" ? RoomPurpose.Commons : PurposeFor(name),
                    Floor = f,
                    Directories = dirs,
                    FileCount = list.Count,
                    Artifacts = list
                        .OrderByDescending(e => KindFor(e.Path) == ArtifactKind.BuildStation)
                        .ThenByDescending(e => e.SizeBytes)
                        .ThenBy(e => e.Path, StringComparer.Ordinal)
                        .Take(WorldConstants.MaxArtifactsPerRoom)
                        .Select(e => new Artifact(e.Path, KindFor(e.Path), e.SizeBytes, LanguageFor(e.Path)))
                        .ToList(),
                };
                rooms.Add(room);
                doors.Add(new Door(corridor.Id, room.Id));
            }
        }
        return new RoomGraph(rooms, doors);
    }
}
