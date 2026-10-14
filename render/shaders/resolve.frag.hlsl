// Palette resolve: 8-bit index frame (R8_UNORM) -> RGBA through a 256x1 palette.

[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
Texture2D<float> IndexTex : register(t0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
SamplerState IndexSmp : register(s0, space2);

[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
Texture2D<float4> PaletteTex : register(t1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
SamplerState PaletteSmp : register(s1, space2);

[[vk::binding(0, 3)]]
cbuffer ResolveUniforms : register(b0, space3)
{
    int4 scale;  // output pixels per frame pixel (x, y), unused, mode (2 = keep samplers)
};

float4 main(float4 pos : SV_Position) : SV_Target0
{
    const int2 p = int2(pos.xy) / max(scale.xy, int2(1, 1));
    const uint i = (uint)(IndexTex.Load(int3(p, 0)) * 255.0 + 0.5);
    if (scale.w == 2)
        return IndexTex.Sample(IndexSmp, float2(0, 0)) + PaletteTex.Sample(PaletteSmp, float2(0, 0));
    return PaletteTex.Load(int3(int(i), 0, 0));
}
