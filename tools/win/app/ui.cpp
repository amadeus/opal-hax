// ui: see ui.hpp. Windows 11 Settings-style cards (fluent.hpp) inside a tray flyout.
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fluent.hpp"
#include "icons/fluent_icons.h"
#include "imgui_internal.h"

namespace opal {

using namespace fluent;

namespace {

struct Ctx {
    UiHost& h;
    AppSettings& s;
    void changed() {
        if(h.changed) h.changed();
    }
};

// a card with a switch, plus a slider underneath while it is on
void toggleWithSlider(Ctx& c, const char* icon, const char* title, const char* desc, bool* on, float* v, float lo, float hi, const char* label,
                      const char* fmt = nullptr, float step = 0, float scale = 1) {
    BeginCard();
    if(ToggleRow(icon, title, desc, on)) c.changed();
    if(*on && v) {
        RowDivider();
        ImGui::PushID(title);
        if(SubSlider(label, v, lo, hi, fmt, step, scale)) c.changed();
        ImGui::PopID();
    }
    EndCard();
}

void toggleCard(Ctx& c, const char* icon, const char* title, const char* desc, bool* on) {
    BeginCard();
    if(ToggleRow(icon, title, desc, on)) c.changed();
    EndCard();
}

void valueRow(const char* icon, const char* title, const char* value, ImU32 col = 0) {
    ImVec2 ts = TextSize(value);
    float maxW = dp(220);
    RowBegin(icon, title, nullptr, ImVec2(std::min(ts.x, maxW), ts.y));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(F.body, F.body->FontSize, p, col ? col : C().text2, value, nullptr, 0,
                                        nullptr);
    RowEnd();
}

void cameraTab(Ctx& c) {
    auto& s = c.s;
    Header("Looks", true);
    BeginCard();
    if(c.h.looks.empty()) {
        bool off = false;
        RowBegin(ICON_COLOR, "Looks", "Not installed (resources\\luts is missing)", ImVec2(0, 0));
        RowEnd();
        (void)off;
    } else {
        if(ToggleRow(ICON_COLOR, "Apply a look", "Colour styles from Opal", &s.filtersOn)) c.changed();
        if(s.filtersOn) {
            RowDivider();
            float cw = SubBegin(0);
            int cols = 4, n = (int)c.h.looks.size();
            float g = dp(6), bw = std::floor((cw - (cols - 1) * g) / cols), bh = dp(32);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            p0.y += dp(12);
            for(int i = 0; i < n; i++) {
                ImGui::SetCursorScreenPos(ImVec2(p0.x + (i % cols) * (bw + g), p0.y + (i / cols) * (bh + g)));
                const std::string& name = c.h.looks[i];
                if(ToggleButton(name.c_str(), name == s.filter, ImVec2(bw, bh))) {
                    s.filter = name;
                    c.changed();
                }
            }
            int rows = (n + cols - 1) / cols;
            SubEnd(dp(12) + rows * bh + (rows - 1) * g + dp(4));
            if(SubSlider("Strength", &s.filterStrength, 0.f, 1.f, "%.0f%%", 0.01f, 100)) c.changed();
        }
    }
    EndCard();

    Header("Framing");
    toggleCard(c, ICON_FLIP_HORIZONTAL, "Mirror", "Flip the picture like a mirror", &s.mirror);
    toggleWithSlider(c, ICON_ZOOM_IN, "Zoom", nullptr, &s.zoom, &s.zoomAmount, 0.f, 1.f, "Amount", "%.0f%%", 0.01f, 100);
    toggleWithSlider(c, ICON_VIDEO_PERSON, "Face lock", "Keep your face in the frame", &s.faceLock, &s.faceLockAmount, 0.f, 1.f, "Amount", "%.0f%%",
                     0.01f, 100);

    Header("Background");
    BeginCard();
    if(ToggleRow(ICON_BLUR, "Bokeh", "Blur the background", &s.bokeh)) {
        if(s.bokeh) s.pixellate = false;
        c.changed();
    }
    if(s.bokeh) {
        RowDivider();
        if(SubSlider("Amount", &s.bokehRadius, 0.4f, 11.f, "%.1f", 0.1f)) c.changed();
    }
    EndCard();
    BeginCard();
    if(ToggleRow(ICON_GRID_DOTS, "Pixellate", "Pixellate the whole picture", &s.pixellate)) {
        if(s.pixellate) s.bokeh = false;
        c.changed();
    }
    if(s.pixellate) {
        RowDivider();
        float lv = (float)s.pixellateLevel;
        if(SubSlider("Block size", &lv, 0.f, 4.f, "%.0f", 1.f)) {
            s.pixellateLevel = (int)std::lround(lv);
            c.changed();
        }
    }
    EndCard();
    toggleCard(c, ICON_LIGHTBULB_PERSON, "Spotlight", "Darken everything around you", &s.spotlight);
}

void effectsTab(Ctx& c) {
    auto& s = c.s;
    Header("Face", true);
    toggleWithSlider(c, ICON_SPARKLE, "Touch up", "Smooth skin", &s.touchUp, &s.touchUpAmount, 0.1f, 0.5f, "Amount", "%.0f%%", 0.01f, 200);
    toggleWithSlider(c, ICON_EMOJI, "Skin adjustment", "Even out your skin tone", &s.skin, &s.skinAmount, 0.f, 0.5f, "Amount", "%.0f%%", 0.01f, 200);

    Header("Light");
    toggleWithSlider(c, ICON_WEATHER_SUNNY, "Highlights", nullptr, &s.highlights, &s.highlightsAmount, -1.f, 1.f, "Amount", "%.0f", 0.01f, 100);
    toggleWithSlider(c, ICON_WEATHER_MOON, "Shadows", nullptr, &s.shadows, &s.shadowsAmount, -1.f, 1.f, "Amount", "%.0f", 0.01f, 100);
    BeginCard();
    if(ToggleRow(ICON_DATA_HISTOGRAM, "Histogram", "Brightness of the camera picture", &s.histogram)) c.changed();
    if(s.histogram && c.h.histogram) {
        RowDivider();
        float w = SubBegin(0), hh = dp(56);
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.y += dp(12);
        float mx = 1e-6f;
        for(int i = 0; i < 64; i++) mx = std::max(mx, c.h.histogram[i]);
        float bw = w / 64.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for(int i = 0; i < 64; i++) {
            float v = c.h.histogram[i] / mx * hh;
            dl->AddRectFilled(ImVec2(p.x + i * bw, p.y + hh - v), ImVec2(p.x + (i + 1) * bw - 1, p.y + hh), C().strongFill);
        }
        SubEnd(dp(12) + hh + dp(12));
    }
    EndCard();

    Header("Overlays");
    BeginCard();
    if(ToggleRow(ICON_TEXT_FONT, "Text", "Two lines at the bottom of the picture", &s.text)) c.changed();
    if(s.text) {
        RowDivider();
        float w = SubBegin(0);
        ImVec2 p = ImGui::GetCursorScreenPos();
        char b1[128], b2[128];
        snprintf(b1, sizeof b1, "%s", s.textLine1.c_str());
        snprintf(b2, sizeof b2, "%s", s.textLine2.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + dp(12)));
        if(TextBox("##l1", "Line 1", b1, sizeof b1, w)) {
            s.textLine1 = b1;
            c.changed();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + dp(12) + dp(32) + dp(8)));
        if(TextBox("##l2", "Line 2", b2, sizeof b2, w)) {
            s.textLine2 = b2;
            c.changed();
        }
        SubEnd(dp(12) + dp(32) * 2 + dp(8) + dp(12));
    }
    EndCard();
    BeginCard();
    if(ToggleRow(ICON_IMAGE, "Logo", "An image in the top corner", &s.logo)) c.changed();
    if(s.logo) {
        RowDivider();
        float w = SubBegin(0);
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string name = s.logoPath.empty() ? std::string("No image chosen") : s.logoPath.substr(s.logoPath.find_last_of("\\/") + 1);
        ImGui::GetWindowDrawList()->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + dp(200)), true);
        ImGui::GetWindowDrawList()->AddText(F.body, F.body->FontSize, ImVec2(p.x, p.y + dp(14)), C().text2, name.c_str());
        ImGui::GetWindowDrawList()->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + dp(14) + F.body->FontSize + dp(8)));
        if(Button("Choose image...") && c.h.chooseLogo) c.h.chooseLogo();
        ImGui::SetCursorScreenPos(ImVec2(p.x + TextSize("Choose image...").x + dp(24) + dp(16), p.y + dp(14) + F.body->FontSize + dp(8)));
        if(CheckBox("Left side", &s.logoSideSwapped)) c.changed();
        SubEnd(dp(14) + F.body->FontSize + dp(8) + dp(32) + dp(12));
    }
    EndCard();

    Header("Gestures");
    toggleCard(c, ICON_HAND_DRAW, "Draw", "Point your index finger to draw", &s.gestureDraw);
    toggleCard(c, ICON_EMOJI_HAND, "Peace sign", "Make a peace sign for confetti", &s.gesturePeace);
    toggleCard(c, ICON_HAND_RIGHT, "Pinch to zoom", "Pinch to turn zoom on or off", &s.gesturePinch);
}

void manualTab(Ctx& c) {
    auto& s = c.s;
    Header("Exposure", true);
    BeginCard();
    if(ToggleRow(ICON_BRIGHTNESS_HIGH, "Auto exposure", nullptr, &s.exposureAuto)) c.changed();
    RowDivider();
    if(s.exposureAuto) {
        float ec = (float)s.exposureCompensation;
        if(SubSlider("Compensation", &ec, -3.f, 3.f, "%.0f", 1.f)) {
            s.exposureCompensation = (int)std::lround(ec);
            c.changed();
        }
    } else {
        if(SubSlider("Exposure", &s.exposure, 0.f, 1.f, "%.0f%%", 0.01f, 100)) c.changed();
    }
    EndCard();

    Header("Colour");
    BeginCard();
    float sat = (float)s.saturation;
    if(SliderRow(ICON_DROP, "Saturation", nullptr, &sat, -10.f, 10.f, "%.0f", 1.f)) {
        s.saturation = (int)std::lround(sat);
        c.changed();
    }
    EndCard();
    BeginCard();
    if(ToggleRow(ICON_TEMPERATURE, "Auto white balance", nullptr, &s.whiteBalanceAuto)) c.changed();
    if(!s.whiteBalanceAuto) {
        RowDivider();
        float k = (float)s.whiteBalanceK;
        if(SubSlider("Temperature", &k, 2500.f, 7000.f, "%.0f K", 50.f)) {
            s.whiteBalanceK = (int)k;
            c.changed();
        }
    }
    EndCard();

    Header("Focus");
    BeginCard();
    static const char* modes[] = {"face", "auto", "manual"};
    static const char* labels[] = {"Face", "Auto", "Manual"};
    int mi = s.focusMode == "auto" ? 1 : s.focusMode == "manual" ? 2 : 0;
    const char* desc = mi == 0 ? "Follows your face" : mi == 1 ? "Focuses on the middle of the picture" : "You set the lens position";
    if(ComboRow(ICON_TARGET, "Focus", desc, &mi, labels, 3)) {
        s.focusMode = modes[mi];
        s.focusLock = false;
        c.changed();
    }
    RowDivider();
    if(mi == 2) {
        float lens = (float)s.lensPosition;
        if(SubSlider("Lens position", &lens, 120.f, 255.f, "%.0f", 1.f)) {
            s.lensPosition = (int)lens;
            c.changed();
        }
    } else {
        if(SubToggle("Lock focus", &s.focusLock)) {
            if(s.focusLock) s.lensPosition = c.h.lastLens;
            c.changed();
        }
    }
    EndCard();
}

void audioTab(Ctx& c) {
    auto& s = c.s;
    Header("Microphone", true);
    BeginCard();
    bool on = !s.micMuted;
    if(ToggleRow(on ? ICON_MIC : ICON_MIC_OFF, "Microphone", on ? "Opal C1 is a microphone in Windows' sound settings" : "Muted: Opal C1 sends no sound",
                 &on)) {
        s.micMuted = !on;
        c.changed();
    }
    if(on) {
        RowDivider();
        if(SubSlider("Gain", &s.micGainDb, 0.f, 30.f, "+%.0f dB", 1.f)) c.changed();
    }
    EndCard();
}

void settingsTab(Ctx& c) {
    auto& s = c.s;
    Header("Video", true);
    static const char* res[] = {"4K", "1440p", "1080p", "720p"};
    int ri = 2;
    for(int i = 0; i < 4; i++)
        if(s.resolution == res[i]) ri = i;
    BeginCard();
    if(ComboRow(ICON_VIDEO, "Camera resolution", "What the camera sends to this PC", &ri, res, 4)) {
        s.resolution = res[ri];
        c.changed();
    }
    EndCard();
    static const char* ab[] = {"Off", "50 Hz", "60 Hz", "Auto"};
    BeginCard();
    if(ComboRow(ICON_LIGHTBULB, "Anti-flicker", "Match your mains frequency", &s.antiBanding, ab, 4)) c.changed();
    EndCard();

    Header("App");
    BeginCard();
    if(ToggleRow(ICON_POWER, "Start at login", "Opal starts in the background", &s.startAtLogin)) {
        if(c.h.startAtLoginChanged) c.h.startAtLoginChanged();
        c.changed();
    }
    EndCard();

    Header("Status");
    BeginCard();
    valueRow(ICON_CAMERA, "Camera", c.h.cameraStatus.empty() ? "-" : c.h.cameraStatus.c_str());
    char b[96];
    if(c.h.chipTempC > 0) {
        RowDivider();
        snprintf(b, sizeof b, "%.1f \xc2\xb0" "C, CPU %.0f%% / %.0f%%", c.h.chipTempC, c.h.cssCpu * 100, c.h.mssCpu * 100);
        valueRow(ICON_TEMPERATURE, "Camera chip", b);
    }
    RowDivider();
    snprintf(b, sizeof b, "%.0f fps%s", c.h.fps, c.h.vcamInUse ? ", in use" : "");
    valueRow(ICON_VIDEO_PERSON, "Opal C1 camera", b);
    if(!c.h.vcamError.empty()) {
        RowDivider();
        valueRow(ICON_INFO, "Virtual camera", c.h.vcamError.c_str(), C().critical);
    }
    if(!c.h.effectsError.empty()) {
        RowDivider();
        valueRow(ICON_INFO, "Effects", c.h.effectsError.c_str(), C().critical);
    }
    EndCard();
    Gap(12);
    if(Button("Quit Opal") && c.h.quit) c.h.quit();
}

}  // namespace

void drawUI(UiHost& h) {
    Ctx c{h, *h.settings};
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Opal", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Colors& col = C();
    ImVec2 o = vp->Pos;
    float W = vp->Size.x, H = vp->Size.y, pad = dp(12), footerH = dp(48);

    // live preview, edge to edge (the window's own rounded corners clip it)
    float ph = std::round(W * 9.f / 16.f);
    ImRect pr(o, ImVec2(o.x + W, o.y + ph));
    if(h.preview) {
        dl->AddImage(h.preview, pr.Min, pr.Max);
    } else {
        dl->AddRectFilled(pr.Min, pr.Max, col.previewBg);
        ImVec2 cc = pr.GetCenter();
        ImVec2 isz = F.icons->CalcTextSizeA(F.icons->FontSize, FLT_MAX, 0, ICON_CAMERA);
        dl->AddText(F.icons, F.icons->FontSize * 1.5f, ImVec2(cc.x - isz.x * 0.75f, cc.y - isz.y * 1.6f), col.text3, ICON_CAMERA);
        std::string st = h.cameraStatus.empty() ? "Starting..." : h.cameraStatus;
        ImVec2 ts = TextSize(st.c_str());
        dl->AddText(F.body, F.body->FontSize, ImVec2(std::round(cc.x - ts.x / 2), std::round(cc.y + dp(4))), col.text2, st.c_str());
    }
    // tabs
    static const char* tabs[] = {"Camera", "Effects", "Manual", "Audio", "Settings"};
    ImGui::SetCursorScreenPos(ImVec2(o.x + pad - dp(4), pr.Max.y + dp(6)));
    SelectorBar("tabs", tabs, 5, &h.tab, W - 2 * pad + dp(8));

    // settings
    float sy = ImGui::GetCursorScreenPos().y + dp(4);
    ImGui::SetCursorScreenPos(ImVec2(o.x + pad, sy));
    BeginScroll("content", ImVec2(W - pad, o.y + H - footerH - sy), h.wheelLines);
    switch(h.tab) {
        case 0: cameraTab(c); break;
        case 1: effectsTab(c); break;
        case 2: manualTab(c); break;
        case 3: audioTab(c); break;
        default: settingsTab(c); break;
    }
    EndScroll();

    // footer: status + settings
    ImRect fr(ImVec2(o.x, o.y + H - footerH), ImVec2(o.x + W, o.y + H));
    dl->AddRectFilled(fr.Min, fr.Max, col.footer);
    dl->AddLine(ImVec2(fr.Min.x, fr.Min.y + 0.5f), ImVec2(fr.Max.x, fr.Min.y + 0.5f), col.divider);
    std::string st;
    if(h.cameraStreaming) {
        char b[64];
        snprintf(b, sizeof b, "Opal C1  \xc2\xb7  %.0f fps%s", h.fps, h.vcamInUse ? "  \xc2\xb7  in use" : "");
        st = b;
    } else {
        st = h.cameraStatus.empty() ? "Opal C1" : h.cameraStatus;
    }
    ImVec2 ts = TextSize(st.c_str());
    float bs = dp(36);
    dl->PushClipRect(fr.Min, ImVec2(fr.Max.x - pad - bs - dp(8), fr.Max.y), true);
    dl->AddText(F.body, F.body->FontSize, ImVec2(fr.Min.x + pad + dp(4), std::round(fr.Min.y + (footerH - ts.y) / 2)), col.text2, st.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(fr.Max.x - pad - bs, fr.Min.y + (footerH - bs) / 2));
    if(IconButton("##settings", ICON_SETTINGS, false, bs)) h.tab = 4;
    ImGui::End();
}

}  // namespace opal
