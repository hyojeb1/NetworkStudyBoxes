
cbuffer CBVS : register(b0)
{
    matrix gWorld;
    matrix gViewProj;
}

cbuffer CBPS : register(b1)
{
    float3 gLightPos;
    float gLightRange;
    float3 gLightColor;
    float gPad;
    float3 gEyePos;
    float gSpecPower;
}

Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

struct VS_IN
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD;
    float3 nrm : NORMAL;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 nrmW : TEXCOORD1;
    float3 posW : TEXCOORD2;
};

PSInput VSMain(VS_IN i)
{
    PSInput o;
    float4 posW = mul(float4(i.pos, 1), gWorld);
    o.pos = mul(posW, gViewProj);
    o.posW = posW.xyz;
    o.nrmW = mul((float3x3) gWorld, i.nrm);
    o.uv = i.uv;
    return o;
}

float4 PSMain(PSInput i) : SV_Target
{
    float3 N = normalize(i.nrmW);
    float3 L = gLightPos - i.posW;
    float dist = length(L);
    L /= dist;

    float atten = saturate(1 - dist / gLightRange);

    float3 V = normalize(gEyePos - i.posW);
    float3 H = normalize(L + V);

    float diff = max(dot(N, L), 0);
    float spec = pow(max(dot(N, H), 0), gSpecPower) * step(0.0f, diff);

    float3 texColor = gTex.Sample(gSamp, i.uv).rgb;
    float3 color = texColor * (gLightColor * (diff + 0.2f * atten) + spec);

    return float4(color, 1);
}
