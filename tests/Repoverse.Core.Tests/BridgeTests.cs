using Repoverse.Core.Bridge;
using Repoverse.Core.Generation;
using Repoverse.Core.Persistence;
using Repoverse.Core.Voxels;

namespace Repoverse.Core.Tests;

public class BridgeTests
{
    private static WorldSession Session(int radius = 0) => new(ManifestBuilder.Build(TestRepos.Slice(),new Seed(42)),radius);
    private static List<MeshPacket> Settle(WorldSession s,double x,double z)
    {
        var meshes=new List<MeshPacket>();
        for(int i=0;i<100;i++) {
            var p=s.Focus(x,z); meshes.AddRange(p.Chunks);
            if(p.Remaining==0)return meshes;
        }
        throw new Exception("Stream did not settle");
    }
    [Fact] public void Streaming_is_bounded_and_unloads_old_meshes()
    {
        var s=Session(); var first=Settle(s,0,0);
        Assert.NotEmpty(first);
        Assert.Equal(WorldGenerator.MaxChunkY+1,s.Resident);
        var moved=s.Focus(1000,1000);
        Assert.Equal(first.Select(m=>m.Id).Distinct().Order(),moved.Unloaded.Order());
        for(int i=0;i<20;i++) {
            Settle(s,i*500,i*500);
            Assert.InRange(s.Resident,1,WorldGenerator.MaxChunkY+1);
        }
    }
    [Fact] public void Edits_remesh_and_survive_unload_and_save_restore()
    {
        var s=Session();Settle(s,0,0);
        s.Edit(2,15,2,new Voxel(BlockType.Wall).Packed);
        Assert.NotEmpty(s.Focus(0,0).Chunks);
        s.SetPlayer(new PlayerState {X=2,Y=16,Z=2,Selection="repoverse://snapkitty/site"});
        var saved=s.Capture();
        Settle(s,2000,2000);Settle(s,0,0);
        Assert.Equal(BlockType.Wall,s.Sample(2,15,2)!.Value.Type);
        var restored=Session();restored.Restore(saved);Settle(restored,0,0);
        Assert.Equal(saved.Player,restored.Player);
        Assert.Equal(BlockType.Wall,restored.Sample(2,15,2)!.Value.Type);
        Assert.Throws<InvalidDataException>(()=>restored.Restore(saved with {WorldSeed="other"}));
    }
    [Fact] public void Mesh_packets_have_outward_core_winding_and_passable_ladders()
    {
        var c=new Chunk(new(0,0,0));c[1,1,1]=new Voxel(BlockType.Wall);c[4,1,1]=new Voxel(BlockType.Ladder);
        var packet=WorldSession.Encode(GreedyMesher.Build(c));
        Assert.Contains(packet.Sections,s=>!s.Collision && (s.Key&255)==(int)BlockType.Ladder);
        var wall=packet.Sections.Single(s=>(s.Key&255)==(int)BlockType.Wall);
        Assert.Equal(24,wall.Vertices.Length);Assert.Equal(36,wall.Indices.Length);
        for(int i=0;i<wall.Indices.Length;i+=3) {
            System.Numerics.Vector3 V(int n) {var v=wall.Vertices[n];return new(v[0],v[1],v[2]);}
            var a=V(wall.Indices[i]);var b=V(wall.Indices[i+1]);var d=V(wall.Indices[i+2]);
            Assert.True(System.Numerics.Vector3.Dot(System.Numerics.Vector3.Cross(b-a,d-a),(a+b+d)/3-new System.Numerics.Vector3(1.5f))>0);
        }
    }
    [Fact] public void Adjacent_arrivals_remesh_boundaries()
    {
        var s=Session(1); var seen=new Dictionary<string,int>();
        for(int i=0;i<100;i++) {
            var p=s.Focus(0,0,1);
            foreach(var c in p.Chunks)seen[c.Id]=seen.GetValueOrDefault(c.Id)+1;
            if(p.Remaining==0)break;
        }
        Assert.Contains(seen.Values,n=>n>1);
        Assert.Equal(9*(WorldGenerator.MaxChunkY+1),s.Resident);
    }
    [Fact] public void Invalid_coordinates_are_rejected()
    {
        var s=Session();Assert.Throws<ArgumentException>(()=>s.Focus(double.NaN,0));
        Assert.Throws<ArgumentException>(()=>s.SetPlayer(new PlayerState{Y=double.PositiveInfinity}));
        Assert.Throws<ArgumentException>(()=>s.Edit(0,0,0,0));
    }
    [Fact] public void Source_is_paged_and_restricted_to_manifest_artifacts()
    {
        var dir=Path.Combine(Path.GetTempPath(),Guid.NewGuid().ToString());Directory.CreateDirectory(dir);
        try {
            var manifest=ManifestBuilder.Build([TestRepos.Make("source",files:["README.md","../escape.txt"])],new Seed(1));
            File.WriteAllLines(Path.Combine(dir,"README.md"),Enumerable.Range(0,80).Select(i=>$"line {i}"));
            var source=new LocalSourceReader(manifest,"snapkitty/source",dir);
            var page=source.Read("snapkitty/source","README.md",32);
            Assert.Equal("line 32",page.Lines[0]);Assert.Equal(32,page.Lines.Length);Assert.True(page.HasMore);
            Assert.Throws<ArgumentException>(()=>source.Read("someone/else","README.md",0));
            Assert.Throws<ArgumentException>(()=>source.Read("snapkitty/source",".git/config",0));
            Assert.Throws<ArgumentException>(()=>source.Read("snapkitty/source","../escape.txt",0));
        } finally {Directory.Delete(dir,true);}
    }
}
