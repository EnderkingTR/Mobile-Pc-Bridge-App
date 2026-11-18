#include "decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "log.h"

static void lock_fn(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->lock(); }
static void unlock_fn(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->unlock(); }

static AVPixelFormat get_format(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    LOGE("Bu video için D3D11VA donanımsal çözme kullanılamıyor");
    return AV_PIX_FMT_NONE;
}

bool Decoder::open(AVCodecID codec_id, ID3D11Device* device, std::recursive_mutex* lock) {
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOGE("Çözücü bulunamadı: %s", avcodec_get_name(codec_id));
        return false;
    }

    // Kendi D3D11 cihazımızı FFmpeg'e veriyoruz: kareler doğrudan çizebileceğimiz cihazda oluşur.
    hw_device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);
    if (GetEnvironmentVariableW(L"KOPRU_DEBUG", nullptr, 0)) av_log_set_level(AV_LOG_DEBUG);
    device->AddRef();  // FFmpeg kapanırken Release eder
    d3d->device = device;
    d3d->lock = lock_fn;
    d3d->unlock = unlock_fn;
    d3d->lock_ctx = lock;
    if (int r = av_hwdevice_ctx_init(hw_device_); r < 0) {
        LOGE("D3D11VA başlatılamadı (%d)", r);
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
    ctx_->get_format = get_format;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_count = 1;      // kare-thread'leme gecikme ekler
    ctx_->extra_hw_frames = 4;   // render thread'in tuttuğu kareler için pay
    // Telefon kodlayıcıları çoğu zaman H.264 "Baseline" profili bildirir; FFmpeg bunu
    // donanıma vermeyi reddeder. Pratikte bu akışlar FMO/ASO kullanmaz, donanımda çözülebilir.
    ctx_->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;

    if (int r = avcodec_open2(ctx_, codec, nullptr); r < 0) {
        LOGE("Çözücü açılamadı (%d)", r);
        return false;
    }
    LOGI("Çözücü: %s + D3D11VA", codec->name);
    return true;
}

void Decoder::close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_device_) av_buffer_unref(&hw_device_);
}

void Decoder::decode(const AVPacket* pkt, const std::function<void(AVFrame*)>& on_frame) {
    int r = avcodec_send_packet(ctx_, pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        char err[128];
        av_strerror(r, err, sizeof err);
        LOGW("Paket çözülemedi: %s", err);
        return;
    }
    for (;;) {
        AVFrame* frame = av_frame_alloc();
        if (avcodec_receive_frame(ctx_, frame) < 0) {
            av_frame_free(&frame);
            break;
        }
        on_frame(frame);
    }
}
