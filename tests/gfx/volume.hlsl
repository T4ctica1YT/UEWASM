// Filling a volume texture the way Unreal does (colour-grading LUTs, volumetric fog, light grids): one instanced quad
// per slice, the geometry shader sending each instance to its slice through SV_RenderTargetArrayIndex.
struct VSIn { float2 pos : POSITION; uint layer : SV_InstanceID; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; uint layer : TEXCOORD1; };
struct GSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float layer : TEXCOORD1; uint rt : SV_RenderTargetArrayIndex; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 0, 1);
    o.uv = i.pos * float2(0.5, -0.5) + 0.5;
    o.layer = i.layer;
    return o;
}

[maxvertexcount(3)]
void GSMain(triangle VSOut i[3], inout TriangleStream<GSOut> s)
{
    for (int k = 0; k < 3; k++) {
        GSOut o;
        o.pos = i[k].pos;
        o.uv = i[k].uv;
        o.layer = i[k].layer;
        o.rt = i[k].layer;
        s.Append(o);
    }
}

float4 PSMain(GSOut i) : SV_Target
{
    return float4(i.uv, i.layer / 31.0, 1);
}

// shows the middle of the volume: red and green follow x and y, blue is the slice (about 0.5 there)
Texture3D Vol : register(t0);
SamplerState Samp : register(s0);
struct ShowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

ShowOut VSShow(uint id : SV_VertexID)
{
    ShowOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

float4 PSShow(ShowOut i) : SV_Target
{
    return Vol.Sample(Samp, float3(i.uv, 0.5));
}
