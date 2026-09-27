using System.Diagnostics;
using static Repoverse.Core.WorldConstants;

namespace Repoverse.Core.Voxels;

/// <summary>
/// An axis-aligned face rectangle. <see cref="Axis"/> is the normal axis (0=X,1=Y,2=Z),
/// <see cref="Positive"/> its direction; (U,V) span the other two axes in cyclic order.
/// Coordinates are chunk-local voxel corners.
/// </summary>
public readonly record struct Quad(int Axis, bool Positive, int Layer, int U, int V, int Width, int Height, ushort MeshKey);

public sealed record ChunkMesh(ChunkCoord Coord, IReadOnlyList<Quad> Quads, int VisibleFaces, TimeSpan BuildTime)
{
    public int Triangles => Quads.Count * 2;
    /// <summary>One draw call per distinct material key (§14).</summary>
    public int MaterialGroups => Quads.Select(q => q.MeshKey).Distinct().Count();
}

/// <summary>
/// Greedy meshing (§14): emits only faces between a visible voxel and a non-opaque
/// neighbour, then merges coplanar faces with the same material key into maximal rectangles.
/// Faces between identical see-through blocks (window next to window) are culled.
/// </summary>
public static class GreedyMesher
{
    private const int S = ChunkSize;

    /// <param name="neighbour">World-space sampler for voxels outside the chunk; null result = not loaded, treated as air.</param>
    public static ChunkMesh Build(Chunk chunk, Func<int, int, int, Voxel?>? neighbour = null)
    {
        var sw = Stopwatch.StartNew();
        var quads = new List<Quad>();
        int faces = 0;
        if (chunk.IsEmpty) return new ChunkMesh(chunk.Coord, quads, 0, sw.Elapsed);

        var mask = new ushort[S * S];
        Span<int> pos = stackalloc int[3];
        Span<int> npos = stackalloc int[3];

        for (int axis = 0; axis < 3; axis++)
        {
            int u = (axis + 1) % 3, v = (axis + 2) % 3;
            foreach (var positive in new[] { false, true })
            {
                for (int layer = 0; layer < S; layer++)
                {
                    // Build the face mask for this slice. 0 = no face; otherwise MeshKey + 1.
                    for (int j = 0; j < S; j++)
                        for (int i = 0; i < S; i++)
                        {
                            pos[axis] = layer; pos[u] = i; pos[v] = j;
                            var here = chunk[pos[0], pos[1], pos[2]];
                            ushort m = 0;
                            if (!here.IsAir)
                            {
                                npos[0] = pos[0]; npos[1] = pos[1]; npos[2] = pos[2];
                                npos[axis] += positive ? 1 : -1;
                                var there = Sample(chunk, npos, neighbour);
                                if (!there.IsOpaque && there.Type != here.Type) { m = (ushort)(here.MeshKey + 1); faces++; }
                            }
                            mask[j * S + i] = m;
                        }

                    // Merge the mask into maximal rectangles.
                    for (int j = 0; j < S; j++)
                        for (int i = 0; i < S;)
                        {
                            var m = mask[j * S + i];
                            if (m == 0) { i++; continue; }
                            int w = 1;
                            while (i + w < S && mask[j * S + i + w] == m) w++;
                            int h = 1;
                            for (; j + h < S; h++)
                            {
                                bool rowOk = true;
                                for (int k = 0; k < w && rowOk; k++) rowOk = mask[(j + h) * S + i + k] == m;
                                if (!rowOk) break;
                            }
                            for (int dy = 0; dy < h; dy++)
                                for (int dx = 0; dx < w; dx++)
                                    mask[(j + dy) * S + i + dx] = 0;
                            quads.Add(new Quad(axis, positive, layer + (positive ? 1 : 0), i, j, w, h, (ushort)(m - 1)));
                            i += w;
                        }
                }
            }
        }
        return new ChunkMesh(chunk.Coord, quads, faces, sw.Elapsed);
    }

    private static Voxel Sample(Chunk chunk, ReadOnlySpan<int> p, Func<int, int, int, Voxel?>? neighbour)
    {
        if ((uint)p[0] < S && (uint)p[1] < S && (uint)p[2] < S) return chunk[p[0], p[1], p[2]];
        if (neighbour == null) return Voxel.Air;
        return neighbour(chunk.Coord.OriginX + p[0], chunk.Coord.OriginY + p[1], chunk.Coord.OriginZ + p[2]) ?? Voxel.Air;
    }

    /// <summary>Quad corners in world space, counter-clockwise when viewed from the face normal.</summary>
    public static (float X, float Y, float Z)[] Corners(ChunkCoord c, Quad q)
    {
        int u = (q.Axis + 1) % 3, v = (q.Axis + 2) % 3;
        var o = new[] { c.OriginX, c.OriginY, c.OriginZ };
        (float, float, float) At(int du, int dv)
        {
            var p = new float[3];
            p[q.Axis] = o[q.Axis] + q.Layer;
            p[u] = o[u] + q.U + du;
            p[v] = o[v] + q.V + dv;
            return (p[0], p[1], p[2]);
        }
        var quad = new[] { At(0, 0), At(q.Width, 0), At(q.Width, q.Height), At(0, q.Height) };
        if (!q.Positive) Array.Reverse(quad);
        return quad;
    }
}
