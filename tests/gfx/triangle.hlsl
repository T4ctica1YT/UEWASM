cbuffer Params : register(b0)
{
    float4 Tint;
    float2 Offset;
};

Texture2D Tex : register(t0);
SamplerState Samp : register(s0);

struct VSIn { float2 pos : POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos + Offset, 0.5, 1.0);
    o.color = i.color;
    o.uv = i.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return i.color * Tint * Tex.Sample(Samp, i.uv);
}
