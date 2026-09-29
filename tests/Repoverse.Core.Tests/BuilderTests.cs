using System.Text.Json;
using Repoverse.Core.Bridge;
using Repoverse.Core.Generation;
using Repoverse.Core.Persistence;
namespace Repoverse.Core.Tests;
public class BuilderTests {
 static BuilderPlan Plan(string operations)=>JsonSerializer.Deserialize<BuilderPlan>("{\"schemaVersion\":1,\"id\":\"test\",\"actor\":\"twin-builder\",\"units\":\"voxel\",\"origin\":{\"x\":0,\"y\":9,\"z\":0},\"operations\":"+operations+"}",Repoverse.Core.Generation.Json.Options)!;
 static WorldSession Session()=>new(ManifestBuilder.Build(TestRepos.Slice(),new Seed(42)),0);
 [Fact] public void Build_survives_save_and_reconnect(){var s=Session();var p=Plan("[{\"name\":\"add_box\",\"args\":{\"width\":2,\"height\":2,\"depth\":2,\"y\":1}}]");var b=s.Build(p);Assert.Equal(8,b.Voxels);var saved=SaveGame.Parse(JsonSerializer.Serialize(s.Capture(),Repoverse.Core.Generation.Json.Options));var restored=Session();restored.Restore(saved);Assert.Equal(b,Assert.Single(restored.Builders));Assert.Equal(s.Capture().Edits.Count,restored.Capture().Edits.Count);restored.Focus(0,0);Assert.False(restored.Sample(0,9,0)!.Value.IsAir);}
 [Fact] public void Invalid_later_operation_does_not_partially_edit(){var s=Session();Assert.Throws<ArgumentException>(()=>s.Build(Plan("[{\"name\":\"add_box\",\"args\":{}},{\"name\":\"eval\",\"args\":{}}]")));Assert.Empty(s.Capture().Edits);Assert.Empty(s.Builders);}
 [Theory] [InlineData("add_sphere")] [InlineData("add_cylinder")] [InlineData("add_cone")] [InlineData("add_torus")]
 public void Solid_shapes_rasterize(string shape){var p=Plan("[{\"name\":\""+shape+"\",\"args\":{\"radius\":3,\"tube\":1,\"height\":4,\"y\":5}}]");Assert.NotEmpty(BuilderRasterizer.Rasterize(p));}
 [Theory] [InlineData("{\"width\":\"bad\"}")] [InlineData("{\"color\":12}")] [InlineData("{\"width\":64,\"height\":64,\"depth\":64,\"y\":40}")] [InlineData("{\"y\":-20}")]
 public void Invalid_or_oversized_builds_rejected(string args){Assert.Throws<ArgumentException>(()=>BuilderRasterizer.Rasterize(Plan("[{\"name\":\"add_box\",\"args\":"+args+"}]")));}
}
