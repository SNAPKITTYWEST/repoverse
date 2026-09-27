using Repoverse.Core.Model;

namespace Repoverse.Core.Generation;

public enum ActivityLevel { Dormant, Low, Moderate, High }
public enum ConstructionState { Unreleased, Prerelease, Released, Archived }

public readonly record struct PlotAddress(District District, int Slot);

/// <summary>Voxel-space rectangle on the ground plane; Y is implied by <see cref="WorldConstants.GroundLevel"/>.</summary>
public readonly record struct Footprint(int X, int Z, int Width, int Depth)
{
    public int MaxX => X + Width - 1;
    public int MaxZ => Z + Depth - 1;
}

/// <summary>What the world generator needs to build one repository's structure (§7, §9).</summary>
public sealed record BuildingDefinition
{
    public required RepoId Repo { get; init; }
    public required string Description { get; init; }
    public required Derived<SemanticCategory> Category { get; init; }
    public required Derived<District> District { get; init; }
    public required Derived<BuildingArchetype> Archetype { get; init; }
    public required MaterialFamily Material { get; init; }
    public required PlotAddress Plot { get; init; }
    public required Footprint Footprint { get; init; }
    public required int Floors { get; init; }
    public required int RoomsPerSide { get; init; }
    public required int RoomDepth { get; init; }
    public required Derived<ActivityLevel> Activity { get; init; }
    public required Derived<ConstructionState> Construction { get; init; }
    public required bool Landmark { get; init; }
    /// <summary>Agent seats reserved for Part II's simulation, from contributor count.</summary>
    public required int Population { get; init; }
    public required string Seed { get; init; }
    public required RoomGraph Interior { get; init; }
    public int Height => Floors * WorldConstants.FloorHeight + 1;
}

public sealed record DependencyEdge(RepoId From, RepoId To);

public sealed record WorldManifest
{
    public const int CurrentSchema = 1;
    public const string GeneratorVersion = "repoverse-gen/1";

    public int SchemaVersion { get; init; } = CurrentSchema;
    public string Generator { get; init; } = GeneratorVersion;
    public required string WorldSeed { get; init; }
    /// <summary>repo key → snapshot id each building was generated from.</summary>
    public required IReadOnlyDictionary<string, string> Snapshots { get; init; }
    public required IReadOnlyList<BuildingDefinition> Buildings { get; init; }
    public required IReadOnlyList<DependencyEdge> Dependencies { get; init; }

    public BuildingDefinition? Find(RepoId id) => Buildings.FirstOrDefault(b => b.Repo.Key == id.Key);
}
