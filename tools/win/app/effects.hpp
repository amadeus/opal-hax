// effects: Opal's video chain on the GPU (Direct3D 11 compute shaders), transcribed from Opal.app's
// OpalCameraVideoService (MetalPetal / Core Image on the Mac) and checked against Opal's own Metal shaders
// (tools/win/verify/shader-check.m). Order, as on the Mac, at the camera frame size with 8-bit intermediates:
//
//   NV12 -> RGB (BT.709 full range)
//   touch-up -> skin tone -> highlights/shadows -> pixellate        (FiltersTabPipeline)
//   bokeh                                                            (CameraPipeline)
//   Face Lock crop -> Zoom crop -> spotlight -> mirror               (AppearancePipeline)
//   logo / footer text overlay
//   look (13 LUTs, blended by strength) -> gesture drawing
//   -> 1920x1080 -> NV12 (full-range code values, like Opal writes)
//
// The person mask for bokeh / touch-up / skin is Opal's SegmenterFilter: two joint-bilateral passes at 256x144.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "framing.hpp"

namespace opal {

struct EffectParams {
    // FiltersTabPipeline
    bool touchUp = false;
    float touchUpAmount = 0.152f;  // OCTouchUp 0.1..0.5
    bool skin = false;
    float skinAmount = 0.2f;   // OCSkinAdjustment 0..0.5 (CIFaceBalance strength = 2x)
    float skinWarmth = 0.5f;   // (K - 2500) / 4500
    bool highlightsShadows = false;
    float highlights = 0, shadows = 0;  // -1..1 (0 when that one is off)
    bool pixellate = false;
    int pixellateLevel = 0;       // OCPixellate 0..4
    int resolutionMultiple = 3;   // 360p 1, 720p 2, 1080p 3, 1440p 4, 2160p 6
    // CameraPipeline
    bool bokeh = false;
    float bokehRadius = 7.6f;  // OCBokeh 0.4..11, pixels of the camera frame
    // AppearancePipeline
    Framing::Result framing;
    bool mirror = true;
    // look
    int look = -1;  // index into looks(); -1 = none
    float lookStrength = 1.f;
    // face for the skin-tone measurement (normalized), from the face detector
    bool hasFace = false;
    Face face{};
};

struct Look {
    std::string name;  // Opal's short name (N2, I2, D1, ...)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lut;
};

class Effects {
   public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::string& lutDir, const std::string& cacheDir, std::string& err);
    const std::vector<Look>& looks() const { return looks_; }

    // camera frame (NV12, w x h), segmentation (2 planes of mw x mh bytes: background, person; may be null)
    // wantNV12: also convert the output for the virtual camera; fetch it with readNV12() (blocks until the GPU is done)
    bool process(const uint8_t* nv12, int w, int h, const uint8_t* mask, int mw, int mh, const EffectParams& p, bool wantNV12);
    bool readNV12(std::vector<uint8_t>& out);
    void reloadSkin() { skinNeedsReload_ = true; }  // Opal re-measures the face's skin colour when skin tone is turned on

    ID3D11ShaderResourceView* preview() const { return outSRV_.Get(); }
    // overlays (premultiplied BGRA, output size), drawn with Direct2D: A = logo/text (before the look, like Opal),
    // B = gesture drawing (after the look)
    IDXGISurface* overlaySurface(int which) const;
    void setOverlayEnabled(bool a, bool b) { overlayA_ = a, overlayB_ = b; }

    static const int kOutW = 1920, kOutH = 1080;

   private:
    template <typename T>
    using Com = Microsoft::WRL::ComPtr<T>;
    struct Tex {
        Com<ID3D11Texture2D> t;
        Com<ID3D11ShaderResourceView> srv;
        Com<ID3D11UnorderedAccessView> uav;
        int w = 0, h = 0;
    };
    bool makeTex(Tex& t, int w, int h, DXGI_FORMAT fmt, UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    bool ensureFrame(int w, int h);
    bool loadLuts(const std::string& dir);
    void measureSkin(const Tex& skinTex, const Face& f);

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    Com<ID3D11Buffer> cb_;
    Com<ID3D11SamplerState> linear_, point_;
    // shaders
    Com<ID3D11ComputeShader> csToRgb_, csTouchOverlay_, csGauss_, csTouchComposite_, csExtractSkin_, csFaceBalance_, csDown2_, csHS_, csPixellate_,
        csResizeGuide_, csSurfaceBlur_, csBokehPre_, csBokehAlpha_, csBokehBC_, csCrop_, csUncrop_, csSpotlight_, csMirror_, csOverlay_, csLook_,
        csFinal_, csToY_, csToUV_;
    // input
    int inW_ = 0, inH_ = 0;
    Com<ID3D11Texture2D> inY_, inUV_, inM0_, inM1_;
    Com<ID3D11ShaderResourceView> inYSRV_, inUVSRV_, inM0SRV_, inM1SRV_;
    // frame-size working textures (8-bit like Opal) and bokeh (16-bit float like Opal)
    Tex fr_[2], aux_[3], half_[3], crop_, skin_, pre_, vert_, diag_;
    // mask (256x144)
    Tex guide_, maskA_, mask_;
    // output
    Tex out_, outY_, outUV_, overlay_[2];
    Com<ID3D11ShaderResourceView> outSRV_;
    Com<ID3D11Texture2D> stY_, stUV_;
    Com<ID3D11ShaderResourceView> curveSRV_;
    std::vector<Look> looks_;
    bool overlayA_ = false, overlayB_ = false, ready_ = false, readbackPending_ = false;
    // skin tone measurement (CIKMeans of the face's skin pixels)
    bool skinNeedsReload_ = true, skinWasOn_ = false;
    double origI_ = 0, origQ_ = 0;
};

}  // namespace opal
