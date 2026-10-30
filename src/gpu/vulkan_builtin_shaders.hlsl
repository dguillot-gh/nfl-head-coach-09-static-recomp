// The Vulkan renderer's geometry shader (src/gpu/vulkan_renderer.cpp). Compiled offline to SPIR-V by
// tools/builtin_shaders_to_spirv.py into vulkan_builtin_shaders.inc (no compiler on the phone).
// The full-screen shaders are in vulkan_builtin_shaders.glsl.

// ---- Rectangle lists (devices with geometry shaders) ------------------------------------------

struct Vertex
{
    float4 interpolators[16] : TEXCOORD0;
    float4 position : SV_Position;
};

// Xenos rectangle lists give three corners; the fourth is opposite the right-angle corner,
// which is the vertex not on the longest edge.
[maxvertexcount(4)]
void rectangle_gs(triangle Vertex input[3], inout TriangleStream<Vertex> stream)
{
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    uint c = 2, a = 0, b = 1;
    if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
    else if (d20 >= d01) { c = 1; a = 2; b = 0; }
    Vertex d;
    d.position = input[a].position + input[b].position - input[c].position;
    [unroll] for (int i = 0; i < 16; i++)
        d.interpolators[i] = input[a].interpolators[i] + input[b].interpolators[i] - input[c].interpolators[i];
    stream.Append(input[c]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(d);
}
