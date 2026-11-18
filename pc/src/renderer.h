#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <mutex>

struct AVFrame;

// Direct3D 11 ile çizim. NV12 kareyi shader'da RGB'ye çevirir, en-boy oranını korur.
// init() dışındaki tüm fonksiyonlar render thread'inden çağrılmalı.
class Renderer {
public:
    bool init(HWND hwnd);
    ID3D11Device* device() const { return device_.Get(); }
    std::recursive_mutex& context_lock() { return lock_; }

    void upload(const AVFrame* frame);  // çözücünün dokusunu kendi dokumuza kopyalar (GPU içi)
    void draw();
    void resize(UINT width, UINT height);

private:
    bool create_rtv();

    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cbuf_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> tex_;
    ComPtr<ID3D11ShaderResourceView> srv_y_, srv_uv_;

    std::recursive_mutex lock_;
    bool tearing_ = false;
    UINT swap_flags_ = 0;
    UINT bb_w_ = 0, bb_h_ = 0;
    UINT tex_w_ = 0, tex_h_ = 0;
    DXGI_FORMAT tex_fmt_ = DXGI_FORMAT_UNKNOWN;
    int frame_w_ = 0, frame_h_ = 0;

    struct Constants {
        float uv_scale[2];
        float y_offset;
        float y_scale;
        float coef[4];  // R=Cr*c0, G=-Cb*c1-Cr*c2, B=Cb*c3
    } consts_{};
};
