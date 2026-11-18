#include "renderer.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "log.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

static const char kShader[] = R"(
cbuffer Constants : register(b0) {
    float2 uv_scale;
    float  y_offset;
    float  y_scale;
    float4 coef;
    float  brightness;
    float3 pad;
};
Texture2D<float>  tex_y  : register(t0);
Texture2D<float2> tex_uv : register(t1);
SamplerState smp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID) {
    // Tek üçgenle tam ekran
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p * uv_scale;
    return o;
}

float4 ps_main(VSOut i) : SV_Target {
    float  y = (tex_y.Sample(smp, i.uv) - y_offset) * y_scale;
    float2 c = tex_uv.Sample(smp, i.uv) - 0.5;
    float3 rgb = float3(y + coef.x * c.y,
                        y - coef.y * c.x - coef.z * c.y,
                        y + coef.w * c.x);
    return float4(saturate(rgb) * brightness, 1);
}
)";

static ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "kopru", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        LOGE("Shader derlenemedi: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return blob;
}

static HRESULT create_device(IDXGIAdapter* adapter, ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    return D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                             flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
}

static bool supports_h264_decode(ID3D11Device* dev) {
    ComPtr<ID3D11VideoDevice> vd;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vd)))) return false;
    UINT n = vd->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < n; ++i) {
        GUID g;
        if (SUCCEEDED(vd->GetVideoDecoderProfile(i, &g)) && g == D3D11_DECODER_PROFILE_H264_VLD_NOFGT) return true;
    }
    return false;
}

// Ekranın bağlı olduğu ve H.264 çözebilen kartı tercih et (dizüstünde genelde dahili GPU:
// çizilen görüntü kartlar arası kopyalanmaz). Olmazsa H.264 çözebilen herhangi bir kart.
static bool pick_device(HWND hwnd, ComPtr<ID3D11Device>& out_dev, ComPtr<ID3D11DeviceContext>& out_ctx) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    int best_score = -1;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        bool drives_monitor = false;
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o, output.Reset()) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == mon) drives_monitor = true;
        }

        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        if (FAILED(create_device(adapter.Get(), dev, ctx))) continue;
        bool h264 = supports_h264_decode(dev.Get());
        LOGI("GPU adayı: %s | ekran: %s | H.264 çözme: %s", to_utf8(ad.Description).c_str(),
             drives_monitor ? "evet" : "hayır", h264 ? "evet" : "hayır");

        int score = (h264 ? 2 : 0) + (drives_monitor ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            out_dev = dev;
            out_ctx = ctx;
        }
    }
    return out_dev != nullptr;
}

bool Renderer::init(HWND hwnd) {
    HRESULT hr = S_OK;
    if (!pick_device(hwnd, device_, ctx_)) {
        hr = create_device(nullptr, device_, ctx_);
        if (FAILED(hr)) {
            LOGE("D3D11 cihazı oluşturulamadı (0x%08lx)", hr);
            return false;
        }
    }

    // Çözücü başka thread'den aynı context'i kullanıyor
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgi_dev;
    device_.As(&dxgi_dev);
    dxgi_dev->SetMaximumFrameLatency(1);  // GPU kuyruğunda en fazla 1 kare: düşük gecikme

    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    LOGI("GPU: %s", to_utf8(ad.Description).c_str());

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            tearing_ = allow;
    }
    swap_flags_ = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<UINT>(1, UINT(rc.right - rc.left));
    bb_h_ = std::max<UINT>(1, UINT(rc.bottom - rc.top));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swap_flags_;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        LOGE("Swap chain oluşturulamadı (0x%08lx)", hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!create_rtv()) return false;

    auto vs = compile("vs_main", "vs_4_0");
    auto ps = compile("ps_main", "ps_4_0");
    if (!vs || !ps) return false;
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&bd, nullptr, &cbuf_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&smp, &sampler_);
    return true;
}

bool Renderer::create_rtv() {
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_));
}

void Renderer::resize(UINT w, UINT h) {
    w = std::max<UINT>(1, w);
    h = std::max<UINT>(1, h);
    if (w == bb_w_ && h == bb_h_) return;
    std::lock_guard lk(lock_);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    ctx_->Flush();
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swap_flags_))) {
        LOGE("ResizeBuffers başarısız");
    }
    bb_w_ = w;
    bb_h_ = h;
    create_rtv();
}

void Renderer::upload(const AVFrame* f) {
    if (f->format != AV_PIX_FMT_D3D11) return;
    auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    UINT index = UINT(reinterpret_cast<intptr_t>(f->data[1]));
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);

    std::lock_guard lk(lock_);
    if (!tex_ || d.Width != tex_w_ || d.Height != tex_h_ || d.Format != tex_fmt_) {
        tex_.Reset();
        srv_y_.Reset();
        srv_uv_.Reset();
        DXGI_FORMAT fy, fuv;
        if (d.Format == DXGI_FORMAT_NV12) { fy = DXGI_FORMAT_R8_UNORM; fuv = DXGI_FORMAT_R8G8_UNORM; }
        else if (d.Format == DXGI_FORMAT_P010) { fy = DXGI_FORMAT_R16_UNORM; fuv = DXGI_FORMAT_R16G16_UNORM; }
        else { LOGE("Desteklenmeyen doku formatı %d", int(d.Format)); return; }

        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = d.Width;
        nd.Height = d.Height;
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Format = d.Format;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&nd, nullptr, &tex_))) { LOGE("Doku oluşturulamadı"); return; }

        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = fy;
        device_->CreateShaderResourceView(tex_.Get(), &sv, &srv_y_);
        sv.Format = fuv;
        device_->CreateShaderResourceView(tex_.Get(), &sv, &srv_uv_);
        tex_w_ = d.Width;
        tex_h_ = d.Height;
        tex_fmt_ = d.Format;
    }
    ctx_->CopySubresourceRegion(tex_.Get(), 0, 0, 0, 0, src, index, nullptr);

    frame_w_ = f->width;
    frame_h_ = f->height;
    // Kod çözücü dokusu hizalama için büyük olabilir (ör. 1088 genişlik): sadece gerçek alanı örnekle
    consts_.uv_scale[0] = float(frame_w_) / float(tex_w_);
    consts_.uv_scale[1] = float(frame_h_) / float(tex_h_);

    bool full = f->color_range == AVCOL_RANGE_JPEG;
    bool bt709 = f->colorspace == AVCOL_SPC_BT709;
    consts_.y_offset = full ? 0.0f : 16.0f / 255.0f;
    consts_.y_scale = full ? 1.0f : 255.0f / 219.0f;
    static const float k709_lim[4] = {1.79274f, 0.21325f, 0.53291f, 2.11240f};
    static const float k709_full[4] = {1.5748f, 0.18732f, 0.46812f, 1.8556f};
    static const float k601_lim[4] = {1.59603f, 0.39176f, 0.81297f, 2.01723f};
    static const float k601_full[4] = {1.402f, 0.344136f, 0.714136f, 1.772f};
    const float* k = bt709 ? (full ? k709_full : k709_lim) : (full ? k601_full : k601_lim);
    std::memcpy(consts_.coef, k, sizeof consts_.coef);
}

void Renderer::draw() {
    {
        std::lock_guard lk(lock_);
        const float black[4] = {0, 0, 0, 1};
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx_->ClearRenderTargetView(rtv_.Get(), black);

        if (tex_ && srv_y_ && srv_uv_) {
            IRect r = letterbox(int(bb_w_), int(bb_h_), frame_w_, frame_h_);
            D3D11_VIEWPORT vp{float(r.x), float(r.y), float(r.w), float(r.h), 0.0f, 1.0f};
            ctx_->RSSetViewports(1, &vp);

            consts_.brightness = dimmed_ ? 0.3f : 1.0f;
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(cbuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                std::memcpy(m.pData, &consts_, sizeof consts_);
                ctx_->Unmap(cbuf_.Get(), 0);
            }
            ID3D11ShaderResourceView* srvs[2] = {srv_y_.Get(), srv_uv_.Get()};
            ctx_->IASetInputLayout(nullptr);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->VSSetShader(vs_.Get(), nullptr, 0);
            ctx_->VSSetConstantBuffers(0, 1, cbuf_.GetAddressOf());  // uv_scale köşe shader'ında
            ctx_->PSSetShader(ps_.Get(), nullptr, 0);
            ctx_->PSSetShaderResources(0, 2, srvs);
            ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
            ctx_->PSSetConstantBuffers(0, 1, cbuf_.GetAddressOf());
            ctx_->Draw(3, 0);
        }
    }
    // VSync beklemeden sun: kare hazır olur olmaz DWM'e gider
    swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}
