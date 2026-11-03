#include "overlay.hpp"

#include <cmath>

namespace opal {

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool Overlay::init(IDXGISurface* surfaceA, IDXGISurface* surfaceB, int w, int h) {
    w_ = w;
    h_ = h;
    if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if(FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surfaceA, &props, &rtA_)) || FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surfaceB, &props, &rtB_)))
        return false;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw_.GetAddressOf()))) return false;
    dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 52.f, L"", &line1_);
    dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 38.f, L"", &line2_);
    dw_->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 160.f, L"", &emoji_);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    return true;
}

bool Overlay::loadLogo(const std::string& path) {
    logo_.Reset();
    logoPath_ = path;
    if(path.empty() || !wic_) return false;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    if(FAILED(wic_->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    if(FAILED(dec->GetFrame(0, &frame)) || FAILED(wic_->CreateFormatConverter(&conv))) return false;
    if(FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return false;
    return SUCCEEDED(rtA_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &logo_));
}

void Overlay::draw(const AppSettings& s, const std::deque<StrokePoint>& stroke, double now, double peaceAt, float peaceX, float peaceY, float cropX,
                   float cropY, float cropW, float cropH, bool& visibleA, bool& visibleB) {
    visibleA = visibleB = false;
    if(!rtA_ || !rtB_) return;
    bool peace = now - peaceAt < 1.6;
    bool textOn = s.text && (!s.textLine1.empty() || !s.textLine2.empty()), logoOn = s.logo && !s.logoPath.empty();
    visibleA = textOn || logoOn;
    visibleB = !stroke.empty() || peace;
    if(s.logo && s.logoPath != logoPath_) loadLogo(s.logoPath);

    // A: footer text + logo (output space, not mirrored or zoomed, like Opal's overlays after the mirror)
    if(visibleA || wasVisibleA_) {
        wasVisibleA_ = visibleA;
        rtA_->BeginDraw();
        rtA_->Clear(D2D1::ColorF(0, 0, 0, 0));
        Com<ID2D1SolidColorBrush> white, shade;
        rtA_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
        rtA_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), &shade);
        if(textOn) {
            auto l1 = widen(s.textLine1), l2 = widen(s.textLine2);
            float x = 64, y = h_ - 64.f - 130.f;
            rtA_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 24, y - 16, x + 760, y + 130), 18, 18), shade.Get());
            rtA_->DrawText(l1.c_str(), (UINT32)l1.size(), line1_.Get(), D2D1::RectF(x, y, x + 740, y + 64), white.Get());
            rtA_->DrawText(l2.c_str(), (UINT32)l2.size(), line2_.Get(), D2D1::RectF(x, y + 64, x + 740, y + 120), white.Get());
        }
        if(s.logo && logo_) {
            auto sz = logo_->GetSize();
            float maxH = 140.f, scale = std::min(maxH / sz.height, 360.f / sz.width);
            float lw = sz.width * scale, lh = sz.height * scale;
            float x = s.logoSideSwapped ? 48.f : w_ - 48.f - lw, y = 48.f;
            rtA_->DrawBitmap(logo_.Get(), D2D1::RectF(x, y, x + lw, y + lh));
        }
        rtA_->EndDraw();
    }

    // B: gesture drawing + peace burst (camera-space points through the crop and mirror)
    if(visibleB || wasVisibleB_) {
        wasVisibleB_ = visibleB;
        auto map = [&](float x, float y) {
            float u = (x - cropX) / cropW, v = (y - cropY) / cropH;
            if(s.mirror) u = 1.f - u;
            return D2D1::Point2F(u * w_, v * h_);
        };
        rtB_->BeginDraw();
        rtB_->Clear(D2D1::ColorF(0, 0, 0, 0));
        Com<ID2D1SolidColorBrush> white, ink;
        rtB_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
        rtB_->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.85f, 0.2f, 1), &ink);
        for(size_t i = 1; i < stroke.size(); i++) {
            if(!stroke[i].penDown) continue;
            float age = (float)(now - stroke[i].t);
            ink->SetOpacity(std::max(0.f, 1.f - age / 3.f));
            rtB_->DrawLine(map(stroke[i - 1].x, stroke[i - 1].y), map(stroke[i].x, stroke[i].y), ink.Get(), 12.f);
        }
        if(peace) {
            float t = (float)(now - peaceAt);
            auto p = map(peaceX, peaceY);
            white->SetOpacity(std::max(0.f, 1.f - t / 1.6f));
            const wchar_t* glyph = L"\u270C\uFE0F";
            float rise = t * 180.f;
            rtB_->DrawText(glyph, 2, emoji_.Get(), D2D1::RectF(p.x - 120, p.y - 200 - rise, p.x + 120, p.y - rise), white.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
        rtB_->EndDraw();
    }
}

}  // namespace opal
