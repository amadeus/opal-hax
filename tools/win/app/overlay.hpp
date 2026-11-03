// overlay: draws Opal's overlays with Direct2D into the effects' two BGRA overlay textures: A = footer text (two lines,
// lower-left) and logo image (top corner), composited before the look like Opal's watermark/footer filters;
// B = the gesture drawing stroke and the peace-sign burst, composited after the look like Opal's sketch.
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <deque>
#include <string>

#include "gestures.hpp"
#include "settings.hpp"

namespace opal {

class Overlay {
   public:
    bool init(IDXGISurface* surfaceA, IDXGISurface* surfaceB, int w, int h);
    // visibleA / visibleB: whether anything is drawn on each (so the effects composite it). crop* = the part of the
    // camera frame shown (normalized), for placing camera-space points (the stroke) on the output.
    void draw(const AppSettings& s, const std::deque<StrokePoint>& stroke, double now, double peaceAt, float peaceX, float peaceY, float cropX, float cropY,
              float cropW, float cropH, bool& visibleA, bool& visibleB);

   private:
    template <typename T>
    using Com = Microsoft::WRL::ComPtr<T>;
    bool loadLogo(const std::string& path);
    Com<ID2D1Factory> d2d_;
    Com<ID2D1RenderTarget> rtA_, rtB_;
    Com<IDWriteFactory> dw_;
    Com<IDWriteTextFormat> line1_, line2_, emoji_;
    Com<IWICImagingFactory> wic_;
    Com<ID2D1Bitmap> logo_;
    std::string logoPath_;
    int w_ = 0, h_ = 0;
    bool wasVisibleA_ = true, wasVisibleB_ = true;
};

}  // namespace opal
