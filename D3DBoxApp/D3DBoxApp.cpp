#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Windowsx.h>

#include <algorithm>
#include <vector>
#include <queue>
#include <wrl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <SimpleMath.h>
#include <WICTextureLoader.h>
#include <DDSTextureLoader.h>
#include <chrono> 
#include <filesystem>
#include "AsyncClient.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::SimpleMath;


// ===========================================================
// Camera
// ===========================================================
struct Camera
{
    Matrix  m_View;
    Matrix  m_Proj;
    float   m_Yaw = XMConvertToRadians(0.0f);
    float   m_Pitch = XMConvertToRadians(0.0f);
    float   m_Radius = 10.0f;
    Vector3 m_Target = Vector3::Zero;

    void Init(float width, float height)
    {
        m_Proj = Matrix::CreatePerspectiveFieldOfView(
            XMConvertToRadians(60.0f),
            width / height,
            0.1f, 1000.0f);
        UpdateView();
    }

    void UpdateView()
    {
        m_Pitch = std::clamp(m_Pitch, XMConvertToRadians(-89.0f), XMConvertToRadians(89.0f));
        m_Radius = std::clamp(m_Radius, 2.0f, 200.0f);

        float x = m_Radius * cosf(m_Pitch) * cosf(m_Yaw);
        float z = m_Radius * cosf(m_Pitch) * sinf(m_Yaw);
        float y = m_Radius * sinf(m_Pitch);

        m_View = Matrix::CreateLookAt(Vector3(x, y, z), m_Target, Vector3::Up);
    }

    void OnMouseRotate(float dx, float dy)
    {
        m_Yaw += dx * 0.005f;
        m_Pitch -= dy * 0.005f;
        UpdateView();
    }

    void OnWheelZoom(int delta)
    {
        m_Radius *= (delta > 0) ? 0.9f : 1.1f;
        UpdateView();
    }

    Vector3 GetEyePos() const
    {
        float x = m_Radius * cosf(m_Pitch) * cosf(m_Yaw);
        float z = m_Radius * cosf(m_Pitch) * sinf(m_Yaw);
        float y = m_Radius * sinf(m_Pitch);
        return Vector3(x, y, z);
    }
};

// ===========================================================
// Box : 회전 없이 선형 이동만 (셀 중심 간 이동)
// ===========================================================
struct Box
{
    Vector3 m_Pos = Vector3::Zero;     // 현재 위치(셀 중심)
    Vector3 m_Target = Vector3::Zero;  // 목표 위치
    Vector3 m_Dir = Vector3::Zero;     // 단위 방향
    bool    m_Moving = false;
    float   m_Speed = 5.0f;            // cells/sec
    float   m_CellSize = 1.0f;
    Matrix  m_World = Matrix::Identity;

    void Init(const Vector3& start, float cellSize)
    {
        m_Pos = m_Target = start;
        m_CellSize = cellSize;
        m_Moving = false;
        m_World = Matrix::CreateScale(m_CellSize, 1.0f, m_CellSize) * Matrix::CreateTranslation(m_Pos);
    }

    void SetTarget(const Vector3& target)
    {
        if (m_Moving) return;
        Vector3 delta = target - m_Pos;
        if (delta.Length() < 1e-4f) return;
        m_Target = target;
        m_Dir = delta; m_Dir.Normalize();
        m_Moving = true;
    }

    void Update(float dt)
    {
        if (!m_Moving) return;

        float dist = (m_Target - m_Pos).Length();
        float step = m_Speed * dt;

        if (step >= dist)
        {
            m_Pos = m_Target;
            m_Moving = false;
        }
        else
        {
            m_Pos += m_Dir * step;
        }

        m_World = Matrix::CreateScale(m_CellSize, 1.0f, m_CellSize) * Matrix::CreateTranslation(m_Pos);
    }
};

// ===========================================================
// Vertex / Constant Buffers
// ===========================================================
struct VertexPC { Vector3 pos; Vector3 col; };
struct VertexPTN { Vector3 pos; Vector2 uv; Vector3 normal; };
struct VertexP { Vector3 pos; };

struct CBVS { Matrix gWorld; Matrix gViewProj; };
struct CBPS { Vector3 lightPos; float lightRange; Vector3 lightColor; float pad; Vector3 eyePos; float specPower; };

// ===========================================================
// App
// ===========================================================
struct App
{
    // 네트워크 클라이언트
    std::unique_ptr<boost::asio::io_context> m_IO;
    std::shared_ptr<AsyncClient> m_Client;
    std::thread m_NetThread;

    int m_MySessionKey = -1;

    // D3D11 Core
    ComPtr<IDXGISwapChain> m_SwapChain;
    ComPtr<ID3D11Device> m_Device;
    ComPtr<ID3D11DeviceContext> m_Context;
    ComPtr<ID3D11RenderTargetView> m_RTV;
    ComPtr<ID3D11Texture2D> m_DSVTex;
    ComPtr<ID3D11DepthStencilView> m_DSV;

    // Shaders / Layouts
    ComPtr<ID3D11VertexShader> m_VSColor, m_VSTex, m_VSSky;
    ComPtr<ID3D11PixelShader>  m_PSColor, m_PSTex, m_PSSky;
    ComPtr<ID3D11InputLayout>  m_InputLayoutColor, m_InputLayoutTex, m_InputLayoutSky;

    // Buffers
    ComPtr<ID3D11Buffer> m_CBVS, m_CBPS;
    ComPtr<ID3D11Buffer> m_GridVB;
    ComPtr<ID3D11Buffer> m_BoxVB, m_BoxIB;
    UINT m_GridVertexCount = 0, m_BoxIndexCount = 0;

    // Skybox
    ComPtr<ID3D11Buffer> m_SkyVB, m_SkyIB;
    ComPtr<ID3D11ShaderResourceView> m_SkySRV;
    ComPtr<ID3D11SamplerState> m_SkySampler;
    ComPtr<ID3D11DepthStencilState> m_SkyDSS;
    ComPtr<ID3D11RasterizerState> m_SkyRS;
    UINT m_SkyIndexCount = 0;

    // Texture & Samplers
    ComPtr<ID3D11ShaderResourceView> m_TexSRV;        // 플레이어 박스
    ComPtr<ID3D11ShaderResourceView> m_ObstacleSRV;   // 장애물 박스
    ComPtr<ID3D11SamplerState>       m_Sampler;       // WRAP, Linear
    ComPtr<ID3D11SamplerState>       m_ObstacleSampler; // CLAMP, Linear(또는 Point)

    // Scene
    Camera m_Camera;

    std::unordered_map<int, Box> m_Boxes;
    int m_MyId = -1;   // 내 블록 id (서버가 SPAWN으로 부여)

    // Grid map & obstacles
    std::vector<std::vector<int>> m_GridFlags;  // 0=empty,1=blocked
    std::vector<Box>              m_ObstacleBoxes;


    // Window / Grid
    HWND  m_hWnd = nullptr;
    UINT  m_Width = 1280, m_Height = 720;
    float m_CellSize = 1.0f;
    int   m_HalfCells = 20;

    // Input
    POINT m_LastMouse{ 0,0 };
    bool  m_RBtnDown = false;

    //std::filesystem::path m_ExeDir;
   

    App() = default;

    ~App()
    {
        if (m_IO)
            m_IO->stop();

        if (m_NetThread.joinable())
            m_NetThread.join();

        m_Client.reset();
        m_IO.reset();
    }


    bool Init(HWND hWnd)
    {
        m_hWnd = hWnd;

        RECT rc{};
        GetClientRect(hWnd, &rc);
        m_Width = rc.right - rc.left;
        m_Height = rc.bottom - rc.top;

        // -------------------------------------------------
        // SwapChain (Flip Discard Model)
        // -------------------------------------------------
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = m_Width;
        sd.BufferDesc.Height = m_Height;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        // Flip 모델 요구사항
        sd.BufferDesc.RefreshRate.Numerator = 0;
        sd.BufferDesc.RefreshRate.Denominator = 0;

        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; // Flip 모델 최소 2
        sd.OutputWindow = hWnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = 0; // ALLOW_MODE_SWITCH 사용 금지

        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &sd,
            m_SwapChain.GetAddressOf(),
            m_Device.GetAddressOf(),
            &fl,
            m_Context.GetAddressOf());

        if (FAILED(hr))
        {
            OutputDebugString(L"[D3D] Device creation failed.\n");
            return false;
        }

        // -------------------------------------------------
        // D3D Resources
        // -------------------------------------------------
        CreateRTVDSV();

        if (!CreateShaders())     return false;
        if (!CreateSkyShader())   return false;

        CreateConstantBuffer();
        CreateGridVB();
        CreateBoxMesh();

        CreateSkyMesh();
        LoadSkyTexture();
        CreateSkyRenderStates();

        LoadBoxTexture();
        LoadObstacleTextureAndSampler();

        // -------------------------------------------------
        // Scene
        // -------------------------------------------------
        m_Camera.Init((float)m_Width, (float)m_Height);

        const int gridSize = m_HalfCells * 2 + 1;
        m_GridFlags.assign(gridSize, std::vector<int>(gridSize, 0));

        // -------------------------------------------------
        // Network (Asio)
        // -------------------------------------------------
        m_IO = std::make_unique<boost::asio::io_context>();
        m_Client = std::make_shared<AsyncClient>(*m_IO, "127.0.0.1", 8080);
        m_Client->Start();

        m_NetThread = std::thread([this]()
            {
                m_IO->run();
            });


        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);

        //m_ExeDir =  std::filesystem::path(path).parent_path();

        return true;
    }


    void CreateRTVDSV()
    {
        if (m_Context) m_Context->OMSetRenderTargets(0, nullptr, nullptr);
        m_RTV.Reset(); m_DSV.Reset(); m_DSVTex.Reset();

        ComPtr<ID3D11Texture2D> backBuffer;
        m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBuffer.GetAddressOf());
        m_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_RTV.GetAddressOf());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_Width; td.Height = m_Height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        m_Device->CreateTexture2D(&td, nullptr, m_DSVTex.GetAddressOf());
        m_Device->CreateDepthStencilView(m_DSVTex.Get(), nullptr, m_DSV.GetAddressOf());
    }

    bool CreateShaders()
    {
        ComPtr<ID3DBlob> vsb, psb, err;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        // Grid color
        if (FAILED(D3DCompileFromFile(L"BasicColor.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, vsb.GetAddressOf(), err.GetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, m_VSColor.GetAddressOf());

        if (FAILED(D3DCompileFromFile(L"BasicColor.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", flags, 0, psb.GetAddressOf(), err.ReleaseAndGetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, m_PSColor.GetAddressOf());

        D3D11_INPUT_ELEMENT_DESC ilColor[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(VertexPC, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(VertexPC, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        m_Device->CreateInputLayout(ilColor, _countof(ilColor),
            vsb->GetBufferPointer(), vsb->GetBufferSize(), m_InputLayoutColor.GetAddressOf());

        // Box texture+normal
        if (FAILED(D3DCompileFromFile(L"BasicTex.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, vsb.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, m_VSTex.GetAddressOf());

        if (FAILED(D3DCompileFromFile(L"BasicTex.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", flags, 0, psb.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, m_PSTex.GetAddressOf());

        D3D11_INPUT_ELEMENT_DESC ilTexN[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(VertexPTN,pos),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(VertexPTN,uv),     D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(VertexPTN,normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        m_Device->CreateInputLayout(ilTexN, _countof(ilTexN),
            vsb->GetBufferPointer(), vsb->GetBufferSize(), m_InputLayoutTex.GetAddressOf());

        return true;
    }

    bool CreateSkyShader()
    {
        ComPtr<ID3DBlob> vsb, psb, err;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        if (FAILED(D3DCompileFromFile(L"BasicSkyCubeMap.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, vsb.GetAddressOf(), err.GetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, m_VSSky.GetAddressOf());

        if (FAILED(D3DCompileFromFile(L"BasicSkyCubeMap.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", flags, 0, psb.GetAddressOf(), err.ReleaseAndGetAddressOf())))
        {
            if (err) OutputDebugStringA((char*)err->GetBufferPointer()); return false;
        }
        m_Device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, m_PSSky.GetAddressOf());

        D3D11_INPUT_ELEMENT_DESC ilSky[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(m_Device->CreateInputLayout(ilSky, _countof(ilSky),
            vsb->GetBufferPointer(), vsb->GetBufferSize(), m_InputLayoutSky.GetAddressOf())))
        {
            OutputDebugString(L"[Sky] InputLayout failed\n");
            return false;
        }
        return true;
    }

    void CreateSkyMesh()
    {
        float s = 50.0f;
        VertexP v[8] =
        {
            {{-s,-s,-s}}, {{+s,-s,-s}}, {{+s,+s,-s}}, {{-s,+s,-s}},
            {{-s,-s,+s}}, {{+s,-s,+s}}, {{+s,+s,+s}}, {{-s,+s,+s}},
        };
        D3D11_BUFFER_DESC vbd{ };
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.ByteWidth = sizeof(v);
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA vsd{ v,0,0 };
        m_Device->CreateBuffer(&vbd, &vsd, m_SkyVB.GetAddressOf());

        uint16_t idx[] =
        {
            0,3,2, 0,2,1,
            1,2,6, 1,6,5,
            5,6,7, 5,7,4,
            4,7,3, 4,3,0,
            3,7,6, 3,6,2,
            4,0,1, 4,1,5
        };
        m_SkyIndexCount = _countof(idx);
        D3D11_BUFFER_DESC ibd{ };
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ibd.ByteWidth = sizeof(idx);
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA isd{ idx,0,0 };
        m_Device->CreateBuffer(&ibd, &isd, m_SkyIB.GetAddressOf());
    }

    void LoadSkyTexture()
    {
        HRESULT hr = CreateDDSTextureFromFile(
            m_Device.Get(), L"skybox.dds",
            nullptr, m_SkySRV.GetAddressOf());
        if (FAILED(hr)) OutputDebugString(L"[Sky] skybox.dds load failed\n");

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        m_Device->CreateSamplerState(&sd, m_SkySampler.GetAddressOf());
    }

    void CreateSkyRenderStates()
    {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        m_Device->CreateDepthStencilState(&dsd, m_SkyDSS.GetAddressOf());

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_FRONT;
        rd.FrontCounterClockwise = TRUE;
        m_Device->CreateRasterizerState(&rd, m_SkyRS.GetAddressOf());
    }

    void CreateConstantBuffer()
    {
        D3D11_BUFFER_DESC bd{};
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = sizeof(CBVS);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_Device->CreateBuffer(&bd, nullptr, m_CBVS.GetAddressOf());

        D3D11_BUFFER_DESC pbd{};
        pbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        pbd.ByteWidth = sizeof(CBPS);
        pbd.Usage = D3D11_USAGE_DYNAMIC;
        pbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_Device->CreateBuffer(&pbd, nullptr, m_CBPS.GetAddressOf());
    }

    void CreateGridVB()
    {
        const int   N = m_HalfCells;
        const float s = m_CellSize;
        const float half = N * s;

        std::vector<VertexPC> v;
        v.reserve((N * 2 + 1) * 4);

        Vector3 cMajor(1, 1, 1), cMinor(0.7f, 0.7f, 0.7f);
        Vector3 cAxisX(0.8f, 0.2f, 0.2f), cAxisZ(0.2f, 0.4f, 0.8f);

        for (int i = -N; i <= N; ++i)
        {
            float x = i * s;
            Vector3 col = (i == 0) ? cAxisZ : ((i % 5 == 0) ? cMajor : cMinor);
            v.push_back({ Vector3(-half, 0, x), col });
            v.push_back({ Vector3(+half, 0, x), col });

            float z = i * s;
            col = (i == 0) ? cAxisX : ((i % 5 == 0) ? cMajor : cMinor);
            v.push_back({ Vector3(z, 0, -half), col });
            v.push_back({ Vector3(z, 0, +half), col });
        }

        m_GridVertexCount = (UINT)v.size();

        D3D11_BUFFER_DESC bd{};
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = UINT(v.size() * sizeof(VertexPC));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA sd{ v.data(), 0, 0 };
        m_Device->CreateBuffer(&bd, &sd, m_GridVB.GetAddressOf());
    }

    void CreateBoxMesh()
    {
        Vector3 p[8] =
        {
            {-0.5f, 0.0f, -0.5f}, {+0.5f, 0.0f, -0.5f},
            {+0.5f, 1.0f, -0.5f}, {-0.5f, 1.0f, -0.5f},
            {-0.5f, 0.0f, +0.5f}, {+0.5f, 0.0f, +0.5f},
            {+0.5f, 1.0f, +0.5f}, {-0.5f, 1.0f, +0.5f},
        };

        VertexPTN v24[24] =
        {
            // Front (-Z)
            {p[0], {0,1}, {0,0,-1}}, {p[1], {1,1}, {0,0,-1}},
            {p[2], {1,0}, {0,0,-1}}, {p[3], {0,0}, {0,0,-1}},
            // Right (+X)
            {p[1], {0,1}, {1,0,0}}, {p[5], {1,1}, {1,0,0}},
            {p[6], {1,0}, {1,0,0}}, {p[2], {0,0}, {1,0,0}},
            // Back (+Z)
            {p[5], {0,1}, {0,0,1}}, {p[4], {1,1}, {0,0,1}},
            {p[7], {1,0}, {0,0,1}}, {p[6], {0,0}, {0,0,1}},
            // Left (-X)
            {p[4], {0,1}, {-1,0,0}}, {p[0], {1,1}, {-1,0,0}},
            {p[3], {1,0}, {-1,0,0}}, {p[7], {0,0}, {-1,0,0}},
            // Top (+Y)
            {p[3], {0,1}, {0,1,0}}, {p[2], {1,1}, {0,1,0}},
            {p[6], {1,0}, {0,1,0}}, {p[7], {0,0}, {0,1,0}},
            // Bottom (-Y)
            {p[4], {0,0}, {0,-1,0}}, {p[5], {1,0}, {0,-1,0}},
            {p[1], {1,1}, {0,-1,0}}, {p[0], {0,1}, {0,-1,0}},
        };

        uint16_t idx[] =
        {
            0,1,2, 0,2,3,
            4,5,6, 4,6,7,
            8,9,10, 8,10,11,
            12,13,14, 12,14,15,
            16,17,18, 16,18,19,
            20,21,22, 20,22,23
        };
        m_BoxIndexCount = _countof(idx);

        D3D11_BUFFER_DESC vbd{};
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.ByteWidth = sizeof(v24);
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA vsd{ v24, 0, 0 };
        m_Device->CreateBuffer(&vbd, &vsd, m_BoxVB.GetAddressOf());

        D3D11_BUFFER_DESC ibd{};
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ibd.ByteWidth = sizeof(idx);
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA isd{ idx, 0, 0 };
        m_Device->CreateBuffer(&ibd, &isd, m_BoxIB.GetAddressOf());
    }

    void LoadBoxTexture()
    {
        CreateWICTextureFromFile(
            m_Device.Get(), m_Context.Get(),
            L"BoxTexture.png", nullptr, m_TexSRV.GetAddressOf());

        // Box용 샘플러 (WRAP, Linear)
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        m_Device->CreateSamplerState(&sd, m_Sampler.GetAddressOf());
    }

    void LoadObstacleTextureAndSampler()
    {
        HRESULT hr = CreateWICTextureFromFile(
            m_Device.Get(), m_Context.Get(),
            L"obstacle.png", nullptr, m_ObstacleSRV.GetAddressOf());
        if (FAILED(hr)) OutputDebugString(L"[Texture] obstacle.png load failed\n");

        // 장애물용 샘플러 (CLAMP, Linear) — 픽셀아트면 POINT 권장
        D3D11_SAMPLER_DESC sd2{};
        sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_Device->CreateSamplerState(&sd2, m_ObstacleSampler.GetAddressOf());
    }

    // === Utility ===
    void MapAndSetCB(const Matrix& world, const Matrix& viewProj)
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        m_Context->Map(m_CBVS.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = reinterpret_cast<CBVS*>(ms.pData);
        cb->gWorld = world.Transpose();
        cb->gViewProj = viewProj.Transpose();
        m_Context->Unmap(m_CBVS.Get(), 0);
        m_Context->VSSetConstantBuffers(0, 1, m_CBVS.GetAddressOf());
    }

    void ScreenRay(int mx, int my, Vector3& outOrigin, Vector3& outDir)
    {
        float x = (2.0f * mx / float(m_Width)) - 1.0f;
        float y = 1.0f - (2.0f * my / float(m_Height));

        Matrix invVP = (m_Camera.m_View * m_Camera.m_Proj).Invert();
        Vector3 nearW = Vector3::Transform(Vector3(x, y, 0.0f), invVP);
        Vector3 farW = Vector3::Transform(Vector3(x, y, 1.0f), invVP);

        outOrigin = nearW;
        outDir = (farW - nearW); outDir.Normalize();
    }

    bool RayHitGround(const Vector3& ro, const Vector3& rd, Vector3& outHit)
    {
        if (fabsf(rd.y) < 1e-6f) return false;
        float t = -ro.y / rd.y;
        if (t < 0) return false;
        outHit = ro + rd * t;
        return true;
    }

    Vector3 SnapToCellCenter(const Vector3& p)
    {
        float s = m_CellSize;
        float cx = floorf(p.x / s) * s + s * 0.5f;
        float cz = floorf(p.z / s) * s + s * 0.5f;

        float half = m_HalfCells * s;
        cx = std::clamp(cx, -half + s * 0.5f, half - s * 0.5f);
        cz = std::clamp(cz, -half + s * 0.5f, half - s * 0.5f);
        return { cx, 0.0f, cz };
    }

    bool WorldToGrid(const Vector3& pos, int& gx, int& gz)
    {
        float s = m_CellSize;
        float half = m_HalfCells * s;
        gx = int(floorf((pos.x + half) / s));
        gz = int(floorf((pos.z + half) / s));
        int gridSize = m_HalfCells * 2 + 1;
        return !(gx < 0 || gz < 0 || gx >= gridSize || gz >= gridSize);
    }

    Vector3 GridToWorld(int gx, int gz)
    {
        float s = m_CellSize;
        float half = m_HalfCells * s;
        float wx = -half + gx * s + s * 0.5f;
        float wz = -half + gz * s + s * 0.5f;
        return { wx, 0.0f, wz };
    }

    // === Render ===
    void RenderSkybox()
    {
        m_Context->OMSetDepthStencilState(m_SkyDSS.Get(), 0);
        m_Context->RSSetState(m_SkyRS.Get());

        Matrix viewNoTrans = m_Camera.m_View;
        viewNoTrans._41 = viewNoTrans._42 = viewNoTrans._43 = 0.0f;
        Matrix skyVP = viewNoTrans * m_Camera.m_Proj;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        m_Context->Map(m_CBVS.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        auto* cb = reinterpret_cast<CBVS*>(mapped.pData);
        cb->gWorld = Matrix::Identity.Transpose();
        cb->gViewProj = skyVP.Transpose();
        m_Context->Unmap(m_CBVS.Get(), 0);
        m_Context->VSSetConstantBuffers(0, 1, m_CBVS.GetAddressOf());

        UINT stride = sizeof(VertexP), offset = 0;
        m_Context->IASetInputLayout(m_InputLayoutSky.Get());
        m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_Context->IASetVertexBuffers(0, 1, m_SkyVB.GetAddressOf(), &stride, &offset);
        m_Context->IASetIndexBuffer(m_SkyIB.Get(), DXGI_FORMAT_R16_UINT, 0);

        ID3D11ShaderResourceView* srv = m_SkySRV.Get();
        ID3D11SamplerState* samp = m_SkySampler.Get();

        m_Context->VSSetShader(m_VSSky.Get(), nullptr, 0);
        m_Context->PSSetShader(m_PSSky.Get(), nullptr, 0);
        m_Context->PSSetShaderResources(0, 1, &srv);
        m_Context->PSSetSamplers(0, 1, &samp);

        m_Context->DrawIndexed(m_SkyIndexCount, 0, 0);

        m_Context->OMSetDepthStencilState(nullptr, 0);
        m_Context->RSSetState(nullptr);
    }


    void ProcessNetwork()
    {
        std::string line;

        while (m_Client->PopLine(line))
        {
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            // ---------------------------------
            // ASSIGN <sessionKey>
            // ---------------------------------
            if (cmd == "ASSIGN")
            {
                iss >> m_MySessionKey;
                // 내 세션 키 확정
            }

            // ---------------------------------
            // SNAPSHOT_BEGIN
            // ---------------------------------
            else if (cmd == "SNAPSHOT_BEGIN")
            {
                // 서버 기준 월드 재구성
                m_Boxes.clear();

                // ★ m_MySessionKey는 절대 초기화하지 않는다
            }

            // ---------------------------------
            // SNAPSHOT_END
            // ---------------------------------
            else if (cmd == "SNAPSHOT_END")
            {
                // 현재 단계에서는 추가 처리 없음
            }

            // ---------------------------------
            // SPAWN <sessionKey> <cellX> <cellZ>
            // ---------------------------------
            else if (cmd == "SPAWN")
            {
                int key, x, z;
                iss >> key >> x >> z;

                Vector3 pos(
                    (x + 0.5f) * m_CellSize,
                    0.0f,
                    (z + 0.5f) * m_CellSize
                );

                Box box;
                box.Init(pos, m_CellSize);

                // 동일 key면 덮어쓰기 (스냅샷/재전송 대응)
                m_Boxes[key] = box;
            }

            // ---------------------------------
            // MOVE <sessionKey> <cellX> <cellZ>
            // ---------------------------------
            else if (cmd == "MOVE")
            {
                int key, x, z;
                iss >> key >> x >> z;

                auto it = m_Boxes.find(key);
                if (it == m_Boxes.end())
                    continue;

                Vector3 pos(
                    (x + 0.5f) * m_CellSize,
                    0.0f,
                    (z + 0.5f) * m_CellSize
                );

                // 1단계: 즉시 위치 이동 (텔레포트)
                it->second.Init(pos, m_CellSize);
            }
        }
    }



    void Render()
    {
        // Clear
        float clear[4] = { 0.08f,0.09f,0.11f,1.0f };
        m_Context->OMSetRenderTargets(1, m_RTV.GetAddressOf(), m_DSV.Get());
        m_Context->ClearRenderTargetView(m_RTV.Get(), clear);
        m_Context->ClearDepthStencilView(
            m_DSV.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f, 0);

        D3D11_VIEWPORT vp{ 0,0,(FLOAT)m_Width,(FLOAT)m_Height,0,1 };
        m_Context->RSSetViewports(1, &vp);

        // Skybox
        RenderSkybox();

        // Grid
        m_Context->IASetInputLayout(m_InputLayoutColor.Get());
        m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        UINT strideC = sizeof(VertexPC), offsetC = 0;
        m_Context->IASetVertexBuffers(0, 1, m_GridVB.GetAddressOf(), &strideC, &offsetC);
        m_Context->VSSetShader(m_VSColor.Get(), nullptr, 0);
        m_Context->PSSetShader(m_PSColor.Get(), nullptr, 0);
        MapAndSetCB(Matrix::Identity, m_Camera.m_View * m_Camera.m_Proj);
        m_Context->Draw(m_GridVertexCount, 0);

        // Common state for textured draws
        m_Context->IASetInputLayout(m_InputLayoutTex.Get());
        m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride2 = sizeof(VertexPTN), offset2 = 0;
        m_Context->IASetVertexBuffers(0, 1, m_BoxVB.GetAddressOf(), &stride2, &offset2);
        m_Context->IASetIndexBuffer(m_BoxIB.Get(), DXGI_FORMAT_R16_UINT, 0);
        m_Context->VSSetShader(m_VSTex.Get(), nullptr, 0);
        m_Context->PSSetShader(m_PSTex.Get(), nullptr, 0);

        // Pixel shader CB
        D3D11_MAPPED_SUBRESOURCE ms{};
        m_Context->Map(m_CBPS.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        {
            auto* cb = reinterpret_cast<CBPS*>(ms.pData);
            cb->lightPos = m_Camera.GetEyePos();
            cb->lightRange = 20.0f;
            cb->lightColor = Vector3(1, 1, 0.8f);
            cb->eyePos = m_Camera.GetEyePos();
            cb->specPower = 32.0f;
        }
        m_Context->Unmap(m_CBPS.Get(), 0);
        m_Context->PSSetConstantBuffers(1, 1, m_CBPS.GetAddressOf());

        // Obstacles
        if (!m_ObstacleBoxes.empty())
        {
            m_Context->PSSetShaderResources(0, 1, m_ObstacleSRV.GetAddressOf());
            m_Context->PSSetSamplers(0, 1, m_ObstacleSampler.GetAddressOf());

            for (auto& obs : m_ObstacleBoxes)
            {
                MapAndSetCB(obs.m_World, m_Camera.m_View * m_Camera.m_Proj);
                m_Context->DrawIndexed(m_BoxIndexCount, 0, 0);
            }
        }

        // -----------------------------
        // Player boxes (MULTI)
        // -----------------------------
        if (!m_Boxes.empty())
        {
            m_Context->PSSetShaderResources(0, 1, m_TexSRV.GetAddressOf());
            m_Context->PSSetSamplers(0, 1, m_Sampler.GetAddressOf());

            for (auto& [id, box] : m_Boxes)
            {
                MapAndSetCB(box.m_World, m_Camera.m_View * m_Camera.m_Proj);
                m_Context->DrawIndexed(m_BoxIndexCount, 0, 0);
            }
        }

        m_SwapChain->Present(1, 0);
    }


    void Update(float dt)
    {
        ProcessNetwork();
    }


    // === Frame ===
    void UpdateAndDraw(float deltaTime)
    {
        Update(deltaTime);
        Render();
    }

  
    void SendSpawnRequestToServer(const Vector3& cellCenter)
    {
        int cellX = static_cast<int>(cellCenter.x / m_CellSize);
        int cellZ = static_cast<int>(cellCenter.z / m_CellSize);

        std::string msg =
            "SPAWN " +
            std::to_string(cellX) + " " +
            std::to_string(cellZ) + "\n";

        m_Client->Send(msg);
    }

    void SendMoveRequestToServer(const Vector3& cellCenter)
    {
        int cellX = static_cast<int>(cellCenter.x / m_CellSize);
        int cellZ = static_cast<int>(cellCenter.z / m_CellSize);

        std::string msg =
            "MOVE " +
            std::to_string(cellX) + " " +
            std::to_string(cellZ) + "\n";

        m_Client->Send(msg);
    }

   
    void OnClick(int mx, int my)
    {
        Vector3 ro, rd;
        ScreenRay(mx, my, ro, rd);

        Vector3 hit;
        if (!RayHitGround(ro, rd, hit))
            return;

        Vector3 cellCenter = SnapToCellCenter(hit);

        if (m_MySessionKey == -1)
            return; // 아직 ASSIGN 안 받음

        // C++17 호환
        if (m_Boxes.find(m_MySessionKey) != m_Boxes.end())
        {
            SendMoveRequestToServer(cellCenter);
        }
        else
        {
            SendSpawnRequestToServer(cellCenter);
        }
    }


    void ToggleObstacleAtMouse(int mx, int my)
    {
        Vector3 ro, rd; ScreenRay(mx, my, ro, rd);
        Vector3 hit; if (!RayHitGround(ro, rd, hit)) return;
        Vector3 cellPos = SnapToCellCenter(hit);

        int gx, gz;
        if (!WorldToGrid(cellPos, gx, gz)) return;

        auto& grid = m_GridFlags;
        if (grid[gz][gx] == 0)
        {
            // Add obstacle
            grid[gz][gx] = 1;
            Box obstacle; obstacle.Init(cellPos, m_CellSize);
            m_ObstacleBoxes.push_back(obstacle);
        }
        else
        {
            // Remove obstacle
            grid[gz][gx] = 0;
            m_ObstacleBoxes.erase(
                std::remove_if(m_ObstacleBoxes.begin(), m_ObstacleBoxes.end(),
                    [&](const Box& b) {
                        return (fabs(b.m_Pos.x - cellPos.x) < 1e-3f &&
                            fabs(b.m_Pos.z - cellPos.z) < 1e-3f);
                    }),
                m_ObstacleBoxes.end());
        }
    }

    void Resize(UINT w, UINT h)
    {
        if (!m_Device) return;
        m_Width = w; m_Height = h;
        m_Context->OMSetRenderTargets(0, nullptr, nullptr);
        m_RTV.Reset(); m_DSV.Reset(); m_DSVTex.Reset();
        m_SwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        CreateRTVDSV();
        m_Camera.Init((float)w, (float)h);
    }
};

// ===========================================================
// Win32
// ===========================================================
static App* g_App = nullptr;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_App && wParam != SIZE_MINIMIZED)
            g_App->Resize(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_LBUTTONDOWN:
        if (g_App)
        {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            g_App->OnClick(mx, my);
        }
        break;

    case WM_MBUTTONDOWN:
        if (g_App)
        {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            g_App->ToggleObstacleAtMouse(mx, my);
        }
        break;

    case WM_RBUTTONDOWN:
        if (g_App)
        {
            g_App->m_RBtnDown = true;
            SetCapture(hWnd);
            g_App->m_LastMouse.x = GET_X_LPARAM(lParam);
            g_App->m_LastMouse.y = GET_Y_LPARAM(lParam);
        }
        break;

    case WM_RBUTTONUP:
        if (g_App)
        {
            g_App->m_RBtnDown = false;
            ReleaseCapture();
        }
        break;

    case WM_MOUSEMOVE:
        if (g_App && g_App->m_RBtnDown)
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            float dx = float(mx - g_App->m_LastMouse.x);
            float dy = float(my - g_App->m_LastMouse.y);
            g_App->m_Camera.OnMouseRotate(dx, dy);
            g_App->m_LastMouse = { mx, my };
        }
        break;

    case WM_MOUSEWHEEL:
        if (g_App)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_App->m_Camera.OnWheelZoom(delta);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}


int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSEX wc{ sizeof(WNDCLASSEX) };
    wc.hInstance = hInst;
    wc.lpszClassName = L"DX11_AStarGrid";
    wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassEx(&wc);

    RECT rc{ 0,0,1280,720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowW(
        wc.lpszClassName, L"DX11 Grid + Obstacles + A* (F1 toggle)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);


    App app; g_App = &app;
    if (!app.Init(hWnd)) return -1;

    using clock = std::chrono::high_resolution_clock;

    auto prevTime = clock::now();

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            auto now = clock::now();
            std::chrono::duration<float> elapsed = now - prevTime;
            prevTime = now;

            float deltaTime = elapsed.count(); // seconds

            app.UpdateAndDraw(deltaTime);

            app.ProcessNetwork();
        }
    }
    return 0;
}
