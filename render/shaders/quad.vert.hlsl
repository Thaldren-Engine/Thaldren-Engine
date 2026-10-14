// Axis-aligned pixel rectangle, generated from the vertex index (6 vertices,
// no vertex buffer). Resource layout follows SDL_gpu's conventions.

[[vk::binding(0, 1)]]
cbuffer QuadUniforms : register(b0, space1)
{
    float4 rect;    // x0, y0, x1, y1 in target pixels (x1/y1 exclusive)
    float4 target;  // width, height, unused, unused
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut main(uint id : SV_VertexID)
{
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(1, 0), float2(1, 1), float2(0, 1)
    };
    const float2 c = corners[id];
    const float2 p = lerp(rect.xy, rect.zw, c);
    VSOut o;
    o.pos = float4(p.x / target.x * 2.0 - 1.0, 1.0 - p.y / target.y * 2.0, 0.0, 1.0);
    return o;
}
