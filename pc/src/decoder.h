#pragma once
#include <d3d11.h>

#include <functional>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

// FFmpeg + D3D11VA donanımsal çözücü. Çözülen kareler GPU'da kalır (kopya yok).
class Decoder {
public:
    ~Decoder() { close(); }

    // `lock`: render thread ile D3D11 context'ini paylaşmak için ortak kilit
    bool open(AVCodecID codec, ID3D11Device* device, std::recursive_mutex* lock);
    void close();

    // Paketi çözer; her çıkan kare için on_frame çağrılır (karenin sahipliği çağrılana geçer).
    void decode(const AVPacket* pkt, const std::function<void(AVFrame*)>& on_frame);

private:
    AVCodecContext* ctx_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
};
