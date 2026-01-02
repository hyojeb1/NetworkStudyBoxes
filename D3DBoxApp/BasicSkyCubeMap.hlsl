// =======================================================
// CubeMap Skybox Shader  (DX11, TextureCube)
// - VS: POSITION(float3)만 입력
// - PS: TextureCube(t0), Sampler(s0)
// - 2025-11, for D3D11 + SimpleMath matrices
// =======================================================

cbuffer CBVS : register(b0)
{
    matrix gWorld;
    matrix gViewProj;
}

struct VS_IN
{
    float3 pos : POSITION;
};

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float3 texDir : TEXCOORD0;
};

VS_OUT VSMain(VS_IN i)
{
    VS_OUT o;

    // 스카이박스: 카메라 위치 제거는 C++에서 gViewProj 구성 시 처리됨
    float4 wpos = mul(float4(i.pos, 1.0f), gWorld);
    o.pos = mul(wpos, gViewProj);

    // 큐브맵 샘플 방향(정규화는 PS에서)
    o.texDir = i.pos;

    return o;
}

// -------------------------------------------------------
// 리소스 바인딩: C++에서 반드시 t0/s0로 바인딩
//   m_Context->PSSetShaderResources(0, 1, &m_SkySRV); // t0
//   m_Context->PSSetSamplers(0, 1, &m_SkySampler);     // s0
// -------------------------------------------------------
TextureCube skyTex : register(t0);
SamplerState samp : register(s0);

// 필요 시 LOD 강제(미프맵 없거나 검게 보일 때 0으로 강제)
//#define SKYBOX_FORCE_MIP0

float4 PSMain(VS_OUT i) : SV_Target
{
    float3 dir = normalize(i.texDir);
    
    dir = dir.xzy;

#ifdef SKYBOX_FORCE_MIP0
    float4 col = skyTex.SampleLevel(samp, dir, 0);
#else
    float4 col = skyTex.Sample(samp, dir);
#endif

    // 필요하면 감마 보정(렌더타겟이 sRGB면 보통 불필요)
    // col.rgb = pow(col.rgb, 2.2); // 예: 소스가 sRGB인데 RT가 Linear일 때

    return col;
}
