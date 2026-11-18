float4 Tint : register(c0);      // pixel shader
float4 Offset : register(c4);    // vertex shader
sampler2D Tex : register(s0);

struct VSIn { float2 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct PSIn { float4 color : COLOR0; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos + Offset.xy, 0.5, 1.0);
    o.color = i.color;
    o.uv = i.uv;
    return o;
}

float4 PSMain(PSIn i) : COLOR0
{
    return i.color * Tint * tex2D(Tex, i.uv);
}
