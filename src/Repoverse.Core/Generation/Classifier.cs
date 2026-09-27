using Repoverse.Core.Model;

namespace Repoverse.Core.Generation;

/// <summary>
/// Deterministic, rule-based semantic classification. Each rule contributes weighted
/// evidence; the highest total wins and the winning evidence becomes the provenance.
/// Purpose signals (topics, description, file layout) outweigh language (§80).
/// </summary>
public static class Classifier
{
    /// <summary>A rule returns how many independent signals matched; weight applies per signal, capped at 3.</summary>
    private sealed record Rule(SemanticCategory Category, int Weight, string Name, Func<CanonicalRepository, int> Match);

    /// <summary>
    /// Counts keywords present as whole words in name, description and topics. Text and keywords are
    /// both reduced to space-separated alphanumeric tokens, so "Guppy-to-IR" matches "ir" and
    /// "multi-agent" matches "multi agent". A trailing '*' makes a keyword a word prefix.
    /// </summary>
    private static int Mentions(CanonicalRepository r, params string[] words)
    {
        var text = Tokens($"{r.Id.Name} {r.Description} {string.Join(' ', r.Topics)}");
        return words.Count(w => w.EndsWith('*')
            ? text.Contains(Tokens(w[..^1]).TrimEnd(), StringComparison.Ordinal)
            : text.Contains(Tokens(w), StringComparison.Ordinal));
    }

    private static string Tokens(string s) => " " + TokenSplit.Replace(s.ToLowerInvariant(), " ").Trim() + " ";
    private static readonly System.Text.RegularExpressions.Regex TokenSplit = new("[^a-z0-9]+");

    private static int One(bool b) => b ? 1 : 0;

    private static bool HasFile(CanonicalRepository r, Func<string, bool> pred) =>
        r.Tree.Any(e => e.Kind == TreeEntryKind.File && pred(e.Path.ToLowerInvariant()));

    private static bool HasExt(CanonicalRepository r, params string[] exts) =>
        HasFile(r, p => exts.Any(x => p.EndsWith(x, StringComparison.Ordinal)));

    private static bool Lang(CanonicalRepository r, params string[] langs) =>
        r.PrimaryLanguage is { } l && langs.Contains(l, StringComparer.OrdinalIgnoreCase);

    private static readonly Rule[] Rules =
    [
        new(SemanticCategory.FormalVerification, 6, "proof-sources", r => One(HasExt(r, ".lean", ".v", ".agda", ".idr", ".thy", ".tla"))),
        new(SemanticCategory.FormalVerification, 4, "mentions-proof", r => Mentions(r, "proof*", "verified", "formal", "theorem*", "lean4", "coq")),
        new(SemanticCategory.Compiler, 5, "mentions-compiler", r => Mentions(r, "compiler", "codegen", "llvm", "parser", "interpreter", "wasm", "ir", "lowering")),
        new(SemanticCategory.Compiler, 3, "grammar-files", r => One(HasExt(r, ".g4", ".y", ".lalrpop", ".ll"))),
        new(SemanticCategory.OperatingSystem, 6, "mentions-os", r => Mentions(r, "kernel", "microkernel", "operating system", "bootloader", "hypervisor")),
        new(SemanticCategory.OperatingSystem, 3, "linker-script", r => One(HasExt(r, ".ld", ".lds"))),
        new(SemanticCategory.CryptographicCore, 6, "mentions-crypto", r => Mentions(r, "crypto", "cipher", "signature", "ed25519", "zk", "zero-knowledge", "encryption")),
        new(SemanticCategory.Financial, 6, "mentions-finance", r => Mentions(r, "finance", "trading", "ledger", "payment", "bank", "exchange", "portfolio")),
        new(SemanticCategory.Quantum, 6, "mentions-quantum", r => Mentions(r, "quantum", "qubit", "qiskit", "qir", "circuit")),
        new(SemanticCategory.Hardware, 6, "hdl-sources", r => One(HasExt(r, ".v", ".sv", ".vhd", ".vhdl", ".kicad_pcb", ".sch"))),
        new(SemanticCategory.Hardware, 4, "mentions-hardware", r => Mentions(r, "fpga", "firmware", "embedded", "pcb", "risc-v", "microcontroller")),
        new(SemanticCategory.Training, 6, "mentions-training", r => Mentions(r, "dataset", "corpus", "training", "fine-tune", "finetune")),
        new(SemanticCategory.AgentSystem, 5, "mentions-agent", r => Mentions(r, "agent*", "multi-agent", "autonomous", "llm", "mcp")),
        new(SemanticCategory.Simulation, 5, "mentions-simulation", r => Mentions(r, "simulat*", "voxel", "physics", "game")),
        new(SemanticCategory.Database, 6, "mentions-database", r => Mentions(r, "database", "storage engine", "sql", "kv store")),
        new(SemanticCategory.Networking, 5, "mentions-network", r => Mentions(r, "network", "protocol", "http", "rpc", "proxy", "p2p")),
        new(SemanticCategory.WebInterface, 4, "mentions-web", r => Mentions(r, "frontend", "website", "dashboard", "ui", "react", "web app")),
        new(SemanticCategory.WebInterface, 3, "web-language", r => One(Lang(r, "TypeScript", "JavaScript", "HTML", "CSS", "Vue", "Svelte"))),
        new(SemanticCategory.Documentation, 4, "docs-only", r => One(r.Tree.Count > 0 && r.Tree.Where(e => e.Kind == TreeEntryKind.File)
            .All(e => e.Path.EndsWith(".md", StringComparison.OrdinalIgnoreCase) || e.Path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase) || e.Path.EndsWith(".pdf", StringComparison.OrdinalIgnoreCase)))),
        new(SemanticCategory.SystemsInfrastructure, 3, "systems-language", r => One(Lang(r, "Rust", "C", "C++", "Zig", "Go", "Assembly"))),
        new(SemanticCategory.SystemsInfrastructure, 4, "mentions-infra", r => Mentions(r, "infrastructure", "deploy", "orchestrat*", "ci/cd", "terraform", "runtime")),
    ];

    public static Derived<SemanticCategory> Classify(CanonicalRepository repo, Overrides? overrides = null)
    {
        if (overrides?.Category(repo.Id) is { } forced)
            return new(forced, Provenance.Override(overrides.Source));

        var scores = new Dictionary<SemanticCategory, (int Score, List<string> Evidence)>();
        foreach (var rule in Rules)
        {
            var hits = Math.Min(3, rule.Match(repo));
            if (hits == 0) continue;
            if (!scores.TryGetValue(rule.Category, out var s)) s = (0, []);
            s.Evidence.Add(hits > 1 ? $"{rule.Name}x{hits}" : rule.Name);
            scores[rule.Category] = (s.Score + rule.Weight * hits, s.Evidence);
        }

        if (scores.Count == 0)
            return new(SemanticCategory.Experimental, new("classifier:fallback", "no rule matched"));

        // Ties break on enum order so the result never depends on dictionary iteration.
        var best = scores.OrderByDescending(kv => kv.Value.Score).ThenBy(kv => (int)kv.Key).First();
        return new(best.Key, new("classifier:rules", $"{string.Join('+', best.Value.Evidence)} (score {best.Value.Score})"));
    }

    public static District DistrictFor(SemanticCategory c, bool archived) => archived ? District.Archive : c switch
    {
        SemanticCategory.OperatingSystem or SemanticCategory.Compiler => District.Kernel,
        SemanticCategory.SystemsInfrastructure or SemanticCategory.Networking => District.Infrastructure,
        SemanticCategory.CryptographicCore => District.Security,
        SemanticCategory.FormalVerification => District.FormalMethods,
        SemanticCategory.Financial => District.Financial,
        SemanticCategory.Quantum => District.QuantumResearch,
        SemanticCategory.Hardware => District.HardwareIndustrial,
        SemanticCategory.Training => District.Training,
        SemanticCategory.AgentSystem => District.Agent,
        SemanticCategory.Simulation => District.Ai,
        SemanticCategory.Database => District.Data,
        SemanticCategory.WebInterface => District.WebInterface,
        SemanticCategory.Documentation => District.Archive,
        _ => District.Experimental,
    };

    public static BuildingArchetype ArchetypeFor(SemanticCategory c) => c switch
    {
        SemanticCategory.Compiler => BuildingArchetype.Foundry,
        SemanticCategory.OperatingSystem or SemanticCategory.SystemsInfrastructure => BuildingArchetype.ControlTower,
        SemanticCategory.CryptographicCore => BuildingArchetype.Vault,
        SemanticCategory.FormalVerification => BuildingArchetype.ProofChamber,
        SemanticCategory.Financial => BuildingArchetype.Exchange,
        SemanticCategory.Quantum or SemanticCategory.Hardware or SemanticCategory.Simulation => BuildingArchetype.Laboratory,
        SemanticCategory.Training or SemanticCategory.Documentation => BuildingArchetype.Library,
        SemanticCategory.WebInterface => BuildingArchetype.Studio,
        SemanticCategory.Database => BuildingArchetype.Warehouse,
        SemanticCategory.Networking => BuildingArchetype.Relay,
        SemanticCategory.AgentSystem => BuildingArchetype.Commons,
        _ => BuildingArchetype.Workshop,
    };
}
