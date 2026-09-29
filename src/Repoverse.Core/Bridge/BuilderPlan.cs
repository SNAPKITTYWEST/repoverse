using System.Text.Json;
using System.Text.RegularExpressions;
using Repoverse.Core.Model;
using Repoverse.Core.Voxels;
namespace Repoverse.Core.Bridge;

public sealed record BuildOrigin(int X,int Y,int Z);
public sealed record BuildOperation(string Name,JsonElement Args);
public sealed record BuilderPlan(int SchemaVersion,string Id,string Actor,string Units,BuildOrigin Origin,BuildOperation[] Operations);
public sealed record BuilderState(string Actor,string PlanId,double X,double Y,double Z,int Voxels);
public readonly record struct BuildVoxel(int X,int Y,int Z,ushort Packed);

/// <summary>Data-only Twin builder contract. Validate and rasterize completely before editing the world.</summary>
public static class BuilderRasterizer
{
    public const int MaxVoxels=32768;
    public static IReadOnlyList<BuildVoxel> Rasterize(BuilderPlan plan)
    {
        if(plan.SchemaVersion!=1 || plan.Units!="voxel" || string.IsNullOrWhiteSpace(plan.Id) || plan.Id.Length>128 || string.IsNullOrWhiteSpace(plan.Actor) || plan.Actor.Length>64
            || plan.Origin is null || plan.Operations is null || plan.Operations.Length is <1 or >128)throw new ArgumentException("Invalid builder plan.");
        if(Math.Abs((long)plan.Origin.X)>1_000_000 || Math.Abs((long)plan.Origin.Z)>1_000_000)throw new ArgumentException("Invalid build origin.");
        var result=new Dictionary<(int,int,int),BuildVoxel>();long candidates=0;
        foreach(var operation in plan.Operations)
        {
            if(operation is null || operation.Args.ValueKind!=JsonValueKind.Object)throw new ArgumentException("Invalid build operation.");
            var a=operation.Args;
            double N(string key,double fallback,double min,double max) {
                double v=fallback;
                if(a.TryGetProperty(key,out var p) && (p.ValueKind!=JsonValueKind.Number || !p.TryGetDouble(out v)))throw new ArgumentException("Invalid "+key);
                if(!double.IsFinite(v)||v<min||v>max)throw new ArgumentException("Invalid "+key);return v;
            }
            double cx=plan.Origin.X+N("x",0,-256,256), cy=plan.Origin.Y+N("y",1,-256,256), cz=plan.Origin.Z+N("z",0,-256,256);
            double rx,ry,rz,r=0,tube=0,h=0;
            switch(operation.Name) {
                case "add_box":rx=N("width",1,.02,64)/2;ry=N("height",1,.02,64)/2;rz=N("depth",1,.02,64)/2;break;
                case "add_sphere":rx=ry=rz=r=N("radius",.7,.02,64);break;
                case "add_cylinder":rx=rz=r=N("radius",.5,.02,64);ry=(h=N("height",2,.02,64))/2;break;
                case "add_cone":rx=rz=r=N("radius",.6,.02,64);ry=(h=N("height",1.5,.02,64))/2;break;
                case "add_torus":r=N("radius",1,.02,64);tube=N("tube",.3,.02,64);if(tube>r)throw new ArgumentException("Invalid torus tube");rx=ry=r+tube;rz=tube;break;
                default:throw new ArgumentException("Unsupported build shape: "+operation.Name);
            }
            // Sample voxel centers in the same Y-up coordinates as the preview. Torus lies in XY.
            int xmin=(int)Math.Ceiling(cx-rx-.5),xmax=(int)Math.Floor(cx+rx-.5),ymin=(int)Math.Ceiling(cy-ry-.5),ymax=(int)Math.Floor(cy+ry-.5),zmin=(int)Math.Ceiling(cz-rz-.5),zmax=(int)Math.Floor(cz+rz-.5);
            if(cx-rx < -1_000_000 || cx+rx>1_000_000 || cz-rz < -1_000_000 || cz+rz>1_000_000 || cy-ry<1 || cy+ry>(WorldGenerator.MaxChunkY+1)*WorldConstants.ChunkSize)throw new ArgumentException("Build exceeds world bounds.");
            candidates+=(long)Math.Max(0,xmax-xmin+1)*Math.Max(0,ymax-ymin+1)*Math.Max(0,zmax-zmin+1);
            if(candidates>262144)throw new ArgumentException("Build sampling budget exceeded.");
            var color=a.TryGetProperty("color",out var c)?(c.ValueKind==JsonValueKind.String?c.GetString():null):"#5ad1c4";
            if(color is null || !Regex.IsMatch(color,"^#[0-9a-fA-F]{6}$"))throw new ArgumentException("Invalid build color.");
            // Quantize to the existing ten material families; save schema retains packed voxel materials.
            int red=Convert.ToInt32(color.Substring(1,2),16),green=Convert.ToInt32(color.Substring(3,2),16),blue=Convert.ToInt32(color.Substring(5,2),16);
            var material=red>green&&red>blue?MaterialFamily.Ceramic:green>red&&green>blue?MaterialFamily.Wood:blue>red?MaterialFamily.Glass:MaterialFamily.Concrete;
            ushort packed=new Voxel(BlockType.Wall,material).Packed;
            for(int x=xmin;x<=xmax;x++)for(int y=ymin;y<=ymax;y++)for(int z=zmin;z<=zmax;z++) {
                double dx=x+.5-cx,dy=y+.5-cy,dz=z+.5-cz;
                bool inside=operation.Name switch {
                    "add_sphere"=>dx*dx+dy*dy+dz*dz<=r*r,
                    "add_cylinder"=>dx*dx+dz*dz<=r*r,
                    "add_cone"=>dx*dx+dz*dz<=Math.Pow(r*(.5-dy/h),2),
                    "add_torus"=>Math.Pow(Math.Sqrt(dx*dx+dy*dy)-r,2)+dz*dz<=tube*tube,
                    _=>true
                };
                if(!inside)continue;result[(x,y,z)]=new(x,y,z,packed);
                if(result.Count>MaxVoxels)throw new ArgumentException("Build voxel budget exceeded.");
            }
        }
        if(result.Count==0)throw new ArgumentException("Shapes are too small for this voxel grid. Increase their size.");
        return result.Values.ToArray();
    }
}
