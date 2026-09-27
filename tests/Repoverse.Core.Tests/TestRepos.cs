using Repoverse.Core.Model;

namespace Repoverse.Core.Tests;

internal static class TestRepos
{
    public static CanonicalRepository Make(string name, string? description = null, string? language = null,
        IEnumerable<string>? files = null, int commits = 5, bool archived = false, IReadOnlyList<Release>? releases = null,
        IReadOnlyList<string>? topics = null, IReadOnlyList<RepoId>? dependsOn = null, int contributors = 3)
    {
        var tree = (files ?? ["README.md", "src/main.rs", "src/lib.rs", "tests/basic.rs", "Cargo.toml"])
            .Select(p => new TreeEntry(p, TreeEntryKind.File, 2000 + p.Length * 37))
            .ToList();
        return new CanonicalRepository
        {
            Id = new RepoId("snapkitty", name),
            Description = description,
            PrimaryLanguage = language,
            Languages = language == null ? new Dictionary<string, long>() : new Dictionary<string, long> { [language] = tree.Sum(t => t.SizeBytes) },
            Topics = topics ?? [],
            CommitsLast90Days = commits,
            Archived = archived,
            Releases = releases ?? [],
            Tree = tree,
            DependsOn = dependsOn ?? [],
            Contributors = contributors,
            SnapshotId = "test",
        };
    }

    /// <summary>The ten-category slice from §56.</summary>
    public static IReadOnlyList<CanonicalRepository> Slice() =>
    [
        Make("ortho32", "Microkernel operating system", "Rust", ["kernel/boot.rs", "kernel/mm.rs", "kernel/sched.rs", "linker.ld", "Cargo.toml"]),
        Make("fpga-lab", "FPGA firmware for a RISC-V core", "SystemVerilog", ["rtl/core.sv", "rtl/alu.sv", "sim/tb.sv", "Makefile"]),
        Make("tiny-llm", "Agent framework for autonomous LLM tools", "Python", ["agent/loop.py", "agent/tools.py", "tests/test_loop.py", "pyproject.toml"]),
        Make("proofs", "Formal proofs of the scheduler", "Lean", ["Proofs/Sched.lean", "Proofs/Mem.lean", "lakefile.lean"]),
        Make("ledger", "Double-entry ledger and payment engine", "Go", ["ledger/post.go", "ledger/account.go", "api/http.go", "go.mod"]),
        Make("site", "Marketing website frontend", "TypeScript", ["src/App.tsx", "src/index.tsx", "public/index.html", "package.json"]),
        Make("corpus", "Training dataset and corpus tooling", "Python", ["data/shard0.json", "data/shard1.json", "scripts/clean.py", "README.md"]),
        Make("deployer", "Infrastructure deploy orchestration", "Go", ["cmd/deploy/main.go", "deploy/k8s.yaml", ".github/workflows/ci.yml"]),
        Make("wasmc", "Compiler from a kernel IR to wasm", "Rust", ["src/lower.rs", "src/parser.rs", "src/ir.rs", "tests/lower.rs", "Cargo.toml"]),
        Make("scratch", "misc experiments", null, ["a.txt", "b.bin"]),
    ];
}
