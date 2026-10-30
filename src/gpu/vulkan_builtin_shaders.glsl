// The Vulkan renderer's own full-screen shaders (src/gpu/vulkan_renderer.cpp), in GLSL: compiled
// offline with glslc by tools/builtin_shaders_to_spirv.py (one entry per #ifdef). glslang's SPIR-V
// suits the LG V30's 2018 Adreno driver, which rejects DXC's for these. Positions are in Vulkan's
// clip space (y points down).
#version 450
#extension GL_EXT_samplerless_texture_functions : require

#if defined(FULLSCREEN_VS) || defined(PRESENT_VS)
#ifdef PRESENT_VS
layout(location = 0) out vec2 uv;
#endif
void main()
{
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
#ifdef PRESENT_VS
    uv = corner;
#endif
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
#endif

// Swap: the front buffer drawn to the window.
#ifdef PRESENT_PS
layout(set = 0, binding = 0) uniform texture2D frontBuffer;
layout(set = 0, binding = 1) uniform sampler linearClamp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(texture(sampler2D(frontBuffer, linearClamp), uv).rgb, 1.0);
}
#endif

// Depth resolves: the depth plane (24-bit unorm) into an R32_SFLOAT texture.
#ifdef DEPTH_COPY_PS
layout(set = 0, binding = 0) uniform texture2D depthTexture;
layout(location = 0) out float depth;
void main()
{
    depth = texelFetch(depthTexture, ivec2(gl_FragCoord.xy), 0).r;
}
#endif
