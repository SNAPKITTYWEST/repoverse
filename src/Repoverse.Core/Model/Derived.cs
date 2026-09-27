namespace Repoverse.Core.Model;

/// <summary>Why a derived value has the value it has (§28).</summary>
public sealed record Provenance(string Rule, string Evidence)
{
    public static Provenance Override(string file) => new("manual-override", file);
    public override string ToString() => $"{Rule}: {Evidence}";
}

public sealed record Derived<T>(T Value, Provenance Why);

public enum SemanticCategory
{
    SystemsInfrastructure,
    OperatingSystem,
    Compiler,
    CryptographicCore,
    FormalVerification,
    Financial,
    Simulation,
    Hardware,
    Quantum,
    Training,
    AgentSystem,
    Database,
    Networking,
    WebInterface,
    Documentation,
    Experimental,
}

public enum District
{
    Kernel,
    Ai,
    Financial,
    FormalMethods,
    HardwareIndustrial,
    QuantumResearch,
    Agent,
    WebInterface,
    Data,
    Security,
    Archive,
    Experimental,
    Infrastructure,
    Training,
}

public enum BuildingArchetype
{
    Foundry,        // compilers, runtimes: transformation machinery
    ControlTower,   // OS / kernel / infrastructure
    Vault,          // crypto, security
    ProofChamber,   // formal methods
    Exchange,       // finance
    Laboratory,     // quantum, hardware, simulation
    Library,        // docs, training corpora
    Studio,         // web / interface
    Warehouse,      // databases
    Relay,          // networking
    Commons,        // agent systems
    Workshop,       // small / experimental
}

public enum MaterialFamily { Stone, Concrete, Glass, Metal, Wood, Ceramic, Electronics, Laboratory, Archive, Security }
