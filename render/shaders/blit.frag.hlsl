// Palette-index blit. Every texture holds 8-bit indices in R8_UNORM
// (index / 255, an exact round trip). Mode 0 copies the sprite (key pixels
// discarded); mode 1 mixes it with the frame through the 256x256 table,
// reading the frame from a copy taken before this draw.

[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
Texture2D<float> SpriteTex : register(t0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
SamplerState SpriteSmp : register(s0, space2);

[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
Texture2D<float> FrameTex : register(t1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
SamplerState FrameSmp : register(s1, space2);

[[vk::combinedImageSampler]][[vk::binding(2, 2)]]
Texture2D<float> TableTex : register(t2, space2);
[[vk::combinedImageSampler]][[vk::binding(2, 2)]]
SamplerState TableSmp : register(s2, space2);

[[vk::binding(0, 3)]]
cbuffer BlitUniforms : register(b0, space3)
{
    int4 params;   // sprite top-left x, y in target pixels; key (-1 = none); mode
    int4 atlas;    // sprite position inside its texture: x, y
};

uint to_index(float v) { return (uint)(v * 255.0 + 0.5); }

float main(float4 pos : SV_Position) : SV_Target0
{
    const int2 p = int2(pos.xy);
    const int2 t = p - params.xy + atlas.xy;
    const uint s = to_index(SpriteTex.Load(int3(t, 0)));
    if (params.z >= 0 && s == (uint)params.z)
        discard;
    if (params.w == 1)
    {
        const uint d = to_index(FrameTex.Load(int3(p, 0)));
        return TableTex.Load(int3(int(d), int(s), 0));
    }
    // Keep the samplers referenced so every backend sees the same layout.
    if (params.w == 2)
        return SpriteTex.Sample(SpriteSmp, float2(0, 0)) + FrameTex.Sample(FrameSmp, float2(0, 0)) + TableTex.Sample(TableSmp, float2(0, 0));
    return (float)s / 255.0;
}
