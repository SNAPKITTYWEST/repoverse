namespace Repoverse.Core;

/// <summary>All tunable world dimensions in one place (§64). Units are voxels unless stated.</summary>
public static class WorldConstants
{
    public const int ChunkSize = 32;
    public const int ChunkVolume = ChunkSize * ChunkSize * ChunkSize;

    /// <summary>One building plot plus its surrounding road.</summary>
    public const int PlotSize = 48;
    public const int RoadWidth = 6;
    public const int MaxFootprint = PlotSize - 2 * RoadWidth; // 36
    public const int MinFootprint = 10;

    /// <summary>Plots per district side; a district holds DistrictSide² buildings before overflow.</summary>
    public const int DistrictSide = 12;
    public const int DistrictSpan = DistrictSide * PlotSize;
    public const int DistrictGridColumns = 4;

    public const int GroundLevel = 8;
    public const int FloorHeight = 6; // 1 slab + 5 air
    public const int MaxFloors = 12;
    public const int CorridorWidth = 3;
    public const int DoorHeight = 3;

    /// <summary>Directories with fewer files merge into a shared room (§78 semantic compression).</summary>
    public const int MinFilesForOwnRoom = 2;
    public const int MaxRoomsPerBuilding = 48;
    public const int MaxArtifactsPerRoom = 24;

    public const int ActivityWindowDays = 90;
    public const int StreamRadiusChunks = 6;
    public const int StreamHysteresisChunks = 2;

    public const double SimulationTicksPerSecond = 10;

    public static readonly string[] IgnoredDirectories =
    [
        "node_modules", "target", "bin", "obj", "build", "dist", "out", ".git", ".idea", ".vs",
        "__pycache__", ".venv", "venv", "vendor", ".gradle", ".next", ".cache", "coverage",
    ];
}
