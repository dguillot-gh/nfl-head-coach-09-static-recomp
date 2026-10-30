// The Vulkan renderer's geometry shader (src/gpu/vulkan_renderer.cpp). Compiled offline to SPIR-V by
// tools/builtin_shaders_to_spirv.py into vulkan_builtin_shaders.inc (no compiler on the phone).
// The full-screen shaders are in vulkan_builtin_shaders.glsl.

// ---- Rectangle lists (devices with geometry shaders) ------------------------------------------

struct Vertex
{
    // The 16 interpolators as separate variables, as tools/shaders_to_spirv.py makes the game's shaders.
    float4 i0 : TEXCOORD0;
    float4 i1 : TEXCOORD1;
    float4 i2 : TEXCOORD2;
    float4 i3 : TEXCOORD3;
    float4 i4 : TEXCOORD4;
    float4 i5 : TEXCOORD5;
    float4 i6 : TEXCOORD6;
    float4 i7 : TEXCOORD7;
    float4 i8 : TEXCOORD8;
    float4 i9 : TEXCOORD9;
    float4 i10 : TEXCOORD10;
    float4 i11 : TEXCOORD11;
    float4 i12 : TEXCOORD12;
    float4 i13 : TEXCOORD13;
    float4 i14 : TEXCOORD14;
    float4 i15 : TEXCOORD15;
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
    d.i0 = input[a].i0 + input[b].i0 - input[c].i0;
    d.i1 = input[a].i1 + input[b].i1 - input[c].i1;
    d.i2 = input[a].i2 + input[b].i2 - input[c].i2;
    d.i3 = input[a].i3 + input[b].i3 - input[c].i3;
    d.i4 = input[a].i4 + input[b].i4 - input[c].i4;
    d.i5 = input[a].i5 + input[b].i5 - input[c].i5;
    d.i6 = input[a].i6 + input[b].i6 - input[c].i6;
    d.i7 = input[a].i7 + input[b].i7 - input[c].i7;
    d.i8 = input[a].i8 + input[b].i8 - input[c].i8;
    d.i9 = input[a].i9 + input[b].i9 - input[c].i9;
    d.i10 = input[a].i10 + input[b].i10 - input[c].i10;
    d.i11 = input[a].i11 + input[b].i11 - input[c].i11;
    d.i12 = input[a].i12 + input[b].i12 - input[c].i12;
    d.i13 = input[a].i13 + input[b].i13 - input[c].i13;
    d.i14 = input[a].i14 + input[b].i14 - input[c].i14;
    d.i15 = input[a].i15 + input[b].i15 - input[c].i15;
    stream.Append(input[c]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(d);
}
