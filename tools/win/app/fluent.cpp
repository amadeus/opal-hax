// fluent: see fluent.hpp. Metrics and colours are WinUI 3's (Microsoft.UI.Xaml ThemeResources: Common_themeresources_any,
// ToggleSwitch/Slider/ComboBox/Button/CheckBox/TextBox/SelectorBar/ScrollBar styles, CommunityToolkit SettingsCard).
#include "fluent.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "icons/fluent_icons.h"
#include "imgui_internal.h"

namespace fluent {

Fonts F;
static Colors gC;
static bool gDark = true;
static float gScale = 1.f;

static ImU32 rgba(int r, int g, int b, float a = 1.f) { return IM_COL32(r, g, b, (int)std::lround(a * 255)); }
static ImU32 withAlpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)std::lround(((c >> 24) & 0xFF) * a) << 24); }
static ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

void setScale(float s) { gScale = s > 0 ? s : 1.f; }
float dp(float v) { return std::round(v * gScale * 2.f) / 2.f; }
bool isDark() { return gDark; }
const Colors& C() { return gC; }

void setTheme(bool dark, ImU32 accentDark, ImU32 accentLight) {
    gDark = dark;
    Colors& c = gC;
    if(dark) {
        c.bg = rgba(32, 32, 32);         // SolidBackgroundFillColorBase (Mica fallback)
        c.footer = rgba(28, 28, 28);     // SolidBackgroundFillColorSecondary
        c.card = rgba(43, 43, 43);       // CardBackgroundFillColorDefault over the base
        c.cardBorder = rgba(0, 0, 0, 0.1f);
        c.divider = rgba(255, 255, 255, 0.0837f);
        c.text = rgba(255, 255, 255);
        c.text2 = rgba(255, 255, 255, 0.786f);
        c.text3 = rgba(255, 255, 255, 0.545f);
        c.textDisabled = rgba(255, 255, 255, 0.363f);
        c.control = rgba(255, 255, 255, 0.0605f);
        c.controlHover = rgba(255, 255, 255, 0.0837f);
        c.controlPressed = rgba(255, 255, 255, 0.0326f);
        c.controlDisabled = rgba(255, 255, 255, 0.0419f);
        c.controlStroke = rgba(255, 255, 255, 0.0698f);
        c.controlStroke2 = rgba(255, 255, 255, 0.093f);
        c.strongStroke = rgba(255, 255, 255, 0.544f);
        c.strongFill = rgba(255, 255, 255, 0.544f);
        c.altFill = rgba(0, 0, 0, 0.1f);
        c.altFillHover = rgba(255, 255, 255, 0.0419f);
        c.altFillPressed = rgba(255, 255, 255, 0.0698f);
        c.subtleHover = rgba(255, 255, 255, 0.0605f);
        c.subtlePressed = rgba(255, 255, 255, 0.0419f);
        c.thumb = rgba(69, 69, 69);  // ControlSolidFillColorDefault
        c.flyout = rgba(44, 44, 44);
        c.flyoutBorder = rgba(0, 0, 0, 0.2f);
        c.inputActive = rgba(30, 30, 30, 0.7f);
        c.critical = rgba(255, 153, 164);
        c.accent = accentDark ? accentDark : rgba(76, 194, 255);
        c.onAccent = rgba(0, 0, 0);
        c.previewBg = rgba(24, 24, 24);
    } else {
        c.bg = rgba(243, 243, 243);
        c.footer = rgba(238, 238, 238);
        c.card = rgba(251, 251, 251);
        c.cardBorder = rgba(0, 0, 0, 0.0578f);
        c.divider = rgba(0, 0, 0, 0.0803f);
        c.text = rgba(0, 0, 0, 0.896f);
        c.text2 = rgba(0, 0, 0, 0.606f);
        c.text3 = rgba(0, 0, 0, 0.446f);
        c.textDisabled = rgba(0, 0, 0, 0.361f);
        c.control = rgba(255, 255, 255, 0.7f);
        c.controlHover = rgba(249, 249, 249, 0.5f);
        c.controlPressed = rgba(249, 249, 249, 0.3f);
        c.controlDisabled = rgba(249, 249, 249, 0.3f);
        c.controlStroke = rgba(0, 0, 0, 0.0578f);
        c.controlStroke2 = rgba(0, 0, 0, 0.1622f);
        c.strongStroke = rgba(0, 0, 0, 0.446f);
        c.strongFill = rgba(0, 0, 0, 0.446f);
        c.altFill = rgba(0, 0, 0, 0.0241f);
        c.altFillHover = rgba(0, 0, 0, 0.0578f);
        c.altFillPressed = rgba(0, 0, 0, 0.0924f);
        c.subtleHover = rgba(0, 0, 0, 0.0373f);
        c.subtlePressed = rgba(0, 0, 0, 0.0241f);
        c.thumb = rgba(255, 255, 255);
        c.flyout = rgba(249, 249, 249);
        c.flyoutBorder = rgba(0, 0, 0, 0.0578f);
        c.inputActive = rgba(255, 255, 255);
        c.critical = rgba(196, 43, 28);
        c.accent = accentLight ? accentLight : rgba(0, 95, 184);
        c.onAccent = rgba(255, 255, 255);
        c.previewBg = rgba(230, 230, 230);
    }
    c.accentHover = withAlpha(c.accent, 0.9f);
    c.accentPressed = withAlpha(c.accent, 0.8f);
}

bool loadFonts(const char* regularTtf, const char* semiboldTtf, float emFactor) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar textRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2030, 0x203A, 0x2190, 0x2193, 0x2212, 0x2212, 0};
    static const ImWchar iconRanges[] = {ICON_MIN, ICON_MAX, 0};
    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    F.body = io.Fonts->AddFontFromFileTTF(regularTtf, dp(14) * emFactor, &cfg, textRanges);
    F.strong = io.Fonts->AddFontFromFileTTF(semiboldTtf, dp(14) * emFactor, &cfg, textRanges);
    F.caption = io.Fonts->AddFontFromFileTTF(regularTtf, dp(12) * emFactor, &cfg, textRanges);
    ImFontConfig ic;
    ic.FontDataOwnedByAtlas = false;
    ic.PixelSnapH = true;
    F.icons = io.Fonts->AddFontFromMemoryTTF((void*)kFluentIconsTtf, (int)sizeof kFluentIconsTtf, dp(20), &ic, iconRanges);
    F.iconsSmall = io.Fonts->AddFontFromMemoryTTF((void*)kFluentIconsTtf, (int)sizeof kFluentIconsTtf, dp(14), &ic, iconRanges);
    if(!F.body || !F.strong || !F.caption) {
        io.Fonts->Clear();
        ImFontConfig d;
        d.SizePixels = dp(14) * 1.2f;
        F.body = F.strong = F.caption = io.Fonts->AddFontDefault(&d);
        F.icons = io.Fonts->AddFontFromMemoryTTF((void*)kFluentIconsTtf, (int)sizeof kFluentIconsTtf, dp(20), &ic, iconRanges);
        F.iconsSmall = io.Fonts->AddFontFromMemoryTTF((void*)kFluentIconsTtf, (int)sizeof kFluentIconsTtf, dp(14), &ic, iconRanges);
    }
    io.FontDefault = F.body;
    return io.Fonts->Build();
}

void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(dp(12), dp(12));
    st.WindowRounding = 0;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.PopupRounding = dp(4);
    st.FrameRounding = dp(4);
    st.FramePadding = ImVec2(dp(10), dp(6));
    st.ItemSpacing = ImVec2(0, 0);
    st.ItemInnerSpacing = ImVec2(dp(8), dp(4));
    st.ScrollbarSize = 0;
    st.TouchExtraPadding = ImVec2(0, 0);
    st.AntiAliasedLines = st.AntiAliasedFill = true;
    ImVec4* col = st.Colors;
    auto v = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    for(int i = 0; i < ImGuiCol_COUNT; i++) col[i] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_Text] = v(gC.text);
    col[ImGuiCol_TextDisabled] = v(gC.text3);
    col[ImGuiCol_WindowBg] = v(gC.bg);
    col[ImGuiCol_PopupBg] = v(gC.flyout);  // tool tips (the combo box draws its own flyout)
    col[ImGuiCol_Border] = v(gDark ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 28));
    col[ImGuiCol_FrameBg] = v(gC.control);
    col[ImGuiCol_FrameBgHovered] = v(gC.controlHover);
    col[ImGuiCol_FrameBgActive] = v(gC.inputActive);
    col[ImGuiCol_TextSelectedBg] = v(withAlpha(gC.accent, 0.45f));
    col[ImGuiCol_NavCursor] = v(gC.text);
    col[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.3f);
}

// ------------------------------------------------------------------ helpers

static float approach(float cur, float target, float speed) {
    float d = target - cur, step = ImGui::GetIO().DeltaTime * speed;
    return std::fabs(d) <= step ? target : cur + (d > 0 ? step : -step);
}
static float ease(float t) { return t * t * (3 - 2 * t); }

ImVec2 TextSize(const char* s, ImFont* font) {
    font = font ? font : F.body;
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, s);
}

static void drawText(ImDrawList* dl, ImFont* font, ImVec2 p, ImU32 col, const char* s, float wrap = 0) {
    dl->AddText(font, font->FontSize, ImVec2(std::round(p.x), std::round(p.y)), col, s, nullptr, wrap);
}

static void drawIcon(ImDrawList* dl, const char* icon, ImVec2 center, ImU32 col, ImFont* font = nullptr) {
    font = font ? font : F.icons;
    // centre the glyph's ink box (an icon's advance and line box aren't centred on it)
    unsigned int cp = 0;
    ImTextCharFromUtf8(&cp, icon, nullptr);
    const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
    ImVec2 p;
    if(g) {
        p = ImVec2(center.x - (g->X0 + g->X1) / 2, center.y - (g->Y0 + g->Y1) / 2);
    } else {
        ImVec2 sz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, icon);
        p = ImVec2(center.x - sz.x / 2, center.y - sz.y / 2);
    }
    drawText(dl, font, p, col, icon);
}

void Text(const char* s, ImU32 col, ImFont* font) {
    ImGui::PushFont(font ? font : F.body);
    ImGui::PushStyleColor(ImGuiCol_Text, col ? col : gC.text);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Gap(float dpv) { ImGui::Dummy(ImVec2(0, dp(dpv))); }

// control box with WinUI's elevation border (1 px stroke, a slightly stronger edge at the bottom in light theme / top in dark)
static void controlBox(ImDrawList* dl, ImRect r, ImU32 fill, float round) {
    dl->AddRectFilled(r.Min, r.Max, fill, round);
    dl->AddRect(r.Min, r.Max, gC.controlStroke, round, 0, 1.f);
    if(gDark)
        dl->AddLine(ImVec2(r.Min.x + round, r.Min.y + 0.5f), ImVec2(r.Max.x - round, r.Min.y + 0.5f), gC.controlStroke2, 1.f);
    else
        dl->AddLine(ImVec2(r.Min.x + round, r.Max.y - 0.5f), ImVec2(r.Max.x - round, r.Max.y - 0.5f), gC.controlStroke2, 1.f);
}

static void fmtValue(char* out, size_t n, const char* fmt, float v, float scale) {
    if(!fmt) {
        out[0] = 0;
        return;
    }
    snprintf(out, n, fmt, v * scale);
    // "-0" -> "0"
    if(out[0] == '-' && std::fabs(v * scale) < 0.5f && strchr(fmt, '.') == nullptr) memmove(out, out + 1, strlen(out));
}

// ------------------------------------------------------------------ ToggleSwitch

float ToggleSwitchWidth(bool onOffLabel) {
    float lw = onOffLabel ? std::max(TextSize("On").x, TextSize("Off").x) + dp(12) : 0;
    return lw + dp(40);
}

bool ToggleSwitch(const char* id, bool* v, bool onOffLabel) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(id);
    float lw = onOffLabel ? std::max(TextSize("On").x, TextSize("Off").x) + dp(12) : 0;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = dp(20), w = dp(40);
    float rowH = std::max(h, F.body->FontSize);
    ImRect bb(p, ImVec2(p.x + lw + w, p.y + rowH));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if(pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(gid);
    }
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* t = st->GetFloatRef(gid, *v ? 1.f : 0.f);
    *t = approach(*t, *v ? 1.f : 0.f, 7.f);
    float e = ease(*t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if(onOffLabel) drawText(dl, F.body, ImVec2(p.x, p.y + (rowH - F.body->FontSize) / 2), gC.text, *v ? "On" : "Off");
    ImRect tr(ImVec2(p.x + lw, p.y + (rowH - h) / 2), ImVec2(p.x + lw + w, p.y + (rowH - h) / 2 + h));
    ImU32 offFill = held ? gC.altFillPressed : hovered ? gC.altFillHover : gC.altFill;
    ImU32 onFill = held ? gC.accentPressed : hovered ? gC.accentHover : gC.accent;
    dl->AddRectFilled(tr.Min, tr.Max, lerpCol(offFill, onFill, e), h / 2);
    if(e < 1) dl->AddRect(tr.Min, tr.Max, withAlpha(gC.strongStroke, 1 - e), h / 2, 0, 1.f);
    // knob: 12 px, 14 when hovered, stretched to 17 x 14 while pressed
    float kh = hovered || held ? dp(14) : dp(12), kw = held ? dp(17) : kh;
    float cx0 = tr.Min.x + dp(10), cx1 = tr.Max.x - dp(10);
    float cx = cx0 + (cx1 - cx0) * e, cy = (tr.Min.y + tr.Max.y) / 2;
    ImU32 knob = lerpCol(gC.text2, gC.onAccent, e);
    dl->AddRectFilled(ImVec2(cx - kw / 2, cy - kh / 2), ImVec2(cx + kw / 2, cy + kh / 2), knob, kh / 2);
    return pressed;
}

// ------------------------------------------------------------------ Slider

bool Slider(const char* id, float* v, float lo, float hi, float width, const char* fmt, float step, float fmtScale) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = dp(32), r = dp(10);
    ImRect bb(p, ImVec2(p.x + width, p.y + h));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    ImGui::ButtonBehavior(bb, gid, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    bool changed = false;
    float x0 = bb.Min.x + r, x1 = bb.Max.x - r;
    if(held && x1 > x0) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.f, 1.f);
        float nv = lo + t * (hi - lo);
        if(step > 0) nv = lo + std::round((nv - lo) / step) * step;
        nv = std::clamp(nv, std::min(lo, hi), std::max(lo, hi));
        if(nv != *v) {
            *v = nv;
            changed = true;
            ImGui::MarkItemEdited(gid);
        }
    }
    float t = hi != lo ? std::clamp((*v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
    float cx = x0 + (x1 - x0) * t, cy = bb.Min.y + h / 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float th = dp(4);
    dl->AddRectFilled(ImVec2(x0 - dp(2), cy - th / 2), ImVec2(x1 + dp(2), cy + th / 2), gC.strongFill, th / 2);
    dl->AddRectFilled(ImVec2(x0 - dp(2), cy - th / 2), ImVec2(cx, cy + th / 2), gC.accent, th / 2);
    // thumb: 20 px disc with a 12 px accent centre (14 hovered, 10 pressed)
    dl->AddCircleFilled(ImVec2(cx, cy), r, gC.thumb, 32);
    dl->AddCircle(ImVec2(cx, cy), r - 0.5f, gC.controlStroke2, 32, 1.f);
    float* ir = ImGui::GetStateStorage()->GetFloatRef(gid, dp(6));
    *ir = approach(*ir, held ? dp(5) : hovered ? dp(7) : dp(6), dp(40));
    dl->AddCircleFilled(ImVec2(cx, cy), *ir, held ? gC.accentPressed : hovered ? gC.accentHover : gC.accent, 32);
    // value tool tip while dragging
    if(held && fmt) {
        char b[32];
        fmtValue(b, sizeof b, fmt, *v, fmtScale);
        ImVec2 ts = TextSize(b);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 tp(cx - ts.x / 2 - dp(8), bb.Min.y - ts.y - dp(16));
        ImRect tb(tp, ImVec2(tp.x + ts.x + dp(16), tp.y + ts.y + dp(10)));
        fg->AddRectFilled(ImVec2(tb.Min.x, tb.Min.y + dp(1)), ImVec2(tb.Max.x, tb.Max.y + dp(2)), IM_COL32(0, 0, 0, 40), dp(4));
        fg->AddRectFilled(tb.Min, tb.Max, gC.flyout, dp(4));
        fg->AddRect(tb.Min, tb.Max, gDark ? IM_COL32(0, 0, 0, 80) : IM_COL32(0, 0, 0, 30), dp(4));
        drawText(fg, F.body, ImVec2(tb.Min.x + dp(8), tb.Min.y + dp(5)), gC.text, b);
    }
    return changed;
}

// ------------------------------------------------------------------ Button / ToggleButton / IconButton / CheckBox

static bool buttonImpl(const char* label, ImVec2 size, bool accent) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    std::string text(label, end);
    ImVec2 ts = TextSize(text.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    if(size.x <= 0) size.x = (size.x < 0 ? ImGui::GetContentRegionAvail().x + size.x : ts.x + dp(24));
    if(size.y <= 0) size.y = dp(32);
    ImRect bb(p, ImVec2(p.x + size.x, p.y + size.y));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rr = dp(4);
    ImU32 tc;
    if(accent) {
        dl->AddRectFilled(bb.Min, bb.Max, held ? gC.accentPressed : hovered ? gC.accentHover : gC.accent, rr);
        dl->AddRect(bb.Min, bb.Max, gDark ? IM_COL32(0, 0, 0, 36) : IM_COL32(255, 255, 255, 20), rr);
        tc = held ? withAlpha(gC.onAccent, 0.7f) : gC.onAccent;
    } else {
        controlBox(dl, bb, held ? gC.controlPressed : hovered ? gC.controlHover : gC.control, rr);
        tc = held ? gC.text2 : gC.text;
    }
    drawText(dl, F.body, ImVec2(bb.Min.x + (size.x - ts.x) / 2, bb.Min.y + (size.y - ts.y) / 2), tc, text.c_str());
    return pressed;
}

bool Button(const char* label, ImVec2 size, bool accent) { return buttonImpl(label, size, accent); }
bool ToggleButton(const char* label, bool selected, ImVec2 size) { return buttonImpl(label, size, selected); }

bool IconButton(const char* id, const char* icon, bool toggled, float size) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImRect bb(p, ImVec2(p.x + size, p.y + size));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rr = dp(4);
    ImU32 ic = gC.text;
    if(toggled) {
        dl->AddRectFilled(bb.Min, bb.Max, held ? gC.accentPressed : hovered ? gC.accentHover : gC.accent, rr);
        ic = gC.onAccent;
    } else if(hovered || held) {
        dl->AddRectFilled(bb.Min, bb.Max, held ? gC.subtlePressed : gC.subtleHover, rr);
    }
    drawIcon(dl, icon, bb.GetCenter(), ic);
    return pressed;
}

bool CheckBox(const char* label, bool* v) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    std::string text(label, end);
    ImVec2 ts = TextSize(text.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float bs = dp(20), h = dp(32);
    ImRect bb(p, ImVec2(p.x + bs + dp(8) + ts.x, p.y + h));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if(pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(gid);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect box(ImVec2(p.x, p.y + (h - bs) / 2), ImVec2(p.x + bs, p.y + (h - bs) / 2 + bs));
    if(*v) {
        dl->AddRectFilled(box.Min, box.Max, held ? gC.accentPressed : hovered ? gC.accentHover : gC.accent, dp(4));
        drawIcon(dl, ICON_CHECKMARK, box.GetCenter(), gC.onAccent, F.iconsSmall);
    } else {
        dl->AddRectFilled(box.Min, box.Max, held ? gC.altFillPressed : hovered ? gC.altFillHover : gC.altFill, dp(4));
        dl->AddRect(box.Min, box.Max, gC.strongStroke, dp(4));
    }
    drawText(dl, F.body, ImVec2(box.Max.x + dp(8), p.y + (h - ts.y) / 2), gC.text, text.c_str());
    return pressed;
}

// ------------------------------------------------------------------ TextBox

bool TextBox(const char* id, const char* hint, char* buf, size_t n, float width) {
    ImGui::PushFont(F.body);
    float h = dp(32);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(dp(11), (h - F.body->FontSize) / 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, dp(4));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, gC.text2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, n);
    bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect r(p, ImVec2(p.x + width, p.y + h));
    dl->AddRect(r.Min, r.Max, gC.controlStroke, dp(4));
    // bottom edge: 1 px strong stroke, 2 px accent while focused
    dl->PushClipRect(ImVec2(r.Min.x, r.Max.y - dp(2)), r.Max, true);
    dl->AddRectFilled(ImVec2(r.Min.x, r.Max.y - dp(6)), r.Max, active ? gC.accent : gC.strongStroke, dp(4));
    dl->PopClipRect();
    if(!active) {
        dl->PushClipRect(ImVec2(r.Min.x, r.Max.y - dp(2)), ImVec2(r.Max.x, r.Max.y - dp(1)), true);
        dl->AddRectFilled(ImVec2(r.Min.x, r.Max.y - dp(6)), r.Max, hovered ? gC.controlHover : gC.control, dp(4));
        dl->PopClipRect();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    return changed;
}

// ------------------------------------------------------------------ ComboBox

float ComboWidth(const char* const items[], int n) {
    float w = 0;
    for(int i = 0; i < n; i++) w = std::max(w, TextSize(items[i]).x);
    return std::max(dp(120), w + dp(11) + dp(12) + dp(12) + dp(12));
}

static void flyoutShadow(ImDrawList* dl, ImRect r, float round) {
    // soft shadow (WinUI flyouts: ~16 px blur, 8 px down)
    int layers = 10;
    for(int i = layers; i >= 1; i--) {
        float g = dp(1.2f) * i;
        float a = (gDark ? 0.28f : 0.12f) * (1.f - (float)i / (layers + 1)) / layers * 2.2f;
        dl->AddRectFilled(ImVec2(r.Min.x - g, r.Min.y - g + dp(6)), ImVec2(r.Max.x + g, r.Max.y + g + dp(6)), IM_COL32(0, 0, 0, (int)(a * 255)),
                          round + g);
    }
}

bool Combo(const char* id, int* current, const char* const items[], int n, float width) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiID gid = win->GetID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = dp(32);
    ImRect bb(p, ImVec2(p.x + width, p.y + h));
    ImGui::ItemSize(bb);
    if(!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    char pid[48];
    snprintf(pid, sizeof pid, "##cbp%08x", gid);
    bool open = ImGui::IsPopupOpen(pid);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    controlBox(dl, bb, held || open ? gC.controlPressed : hovered ? gC.controlHover : gC.control, dp(4));
    int cur = std::clamp(*current, 0, n - 1);
    drawText(dl, F.body, ImVec2(bb.Min.x + dp(11), bb.Min.y + (h - F.body->FontSize) / 2), held || open ? gC.text2 : gC.text, items[cur]);
    drawIcon(dl, ICON_CHEVRON_DOWN, ImVec2(bb.Max.x - dp(18), bb.Min.y + h / 2 + dp(1)), gC.text2, F.iconsSmall);
    if(pressed) ImGui::OpenPopup(pid);
    bool changed = false;
    if(ImGui::IsPopupOpen(pid)) {
        float m = dp(16), pad = dp(4), itemH = dp(32), gap = dp(2);
        float fh = n * itemH + (n - 1) * gap + 2 * pad, fw = width + 2 * pad;
        // like WinUI: the list opens over the box with the selected item where the box is
        ImVec2 fpos(bb.Min.x - pad, bb.Min.y - pad - cur * (itemH + gap));
        ImGuiViewport* vp = ImGui::GetMainViewport();
        fpos.y = std::clamp(fpos.y, vp->Pos.y + dp(8), vp->Pos.y + vp->Size.y - fh - dp(8));
        ImGui::SetNextWindowPos(ImVec2(fpos.x - m, fpos.y - m));
        ImGui::SetNextWindowSize(ImVec2(fw + 2 * m, fh + 2 * m));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m + pad, m + pad));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        if(ImGui::BeginPopup(pid, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* pl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            ImRect fr(ImVec2(wp.x + m, wp.y + m), ImVec2(wp.x + m + fw, wp.y + m + fh));
            flyoutShadow(pl, fr, dp(8));
            pl->AddRectFilled(fr.Min, fr.Max, gC.flyout, dp(8));
            pl->AddRect(fr.Min, fr.Max, gDark ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 28), dp(8));
            if(gDark) pl->AddLine(ImVec2(fr.Min.x + dp(8), fr.Min.y + 0.5f), ImVec2(fr.Max.x - dp(8), fr.Min.y + 0.5f), IM_COL32(255, 255, 255, 18));
            for(int i = 0; i < n; i++) {
                ImGuiID iid = ImGui::GetID(i);
                ImVec2 ip = ImGui::GetCursorScreenPos();
                ImRect ib(ip, ImVec2(ip.x + width, ip.y + itemH));
                ImGui::ItemSize(ImVec2(width, itemH + (i + 1 < n ? gap : 0)));
                if(!ImGui::ItemAdd(ib, iid)) continue;
                bool ih, iheld;
                bool ip2 = ImGui::ButtonBehavior(ib, iid, &ih, &iheld, ImGuiButtonFlags_PressedOnRelease);
                if(i == cur || ih) pl->AddRectFilled(ib.Min, ib.Max, iheld ? gC.subtlePressed : gC.subtleHover, dp(4));
                if(i == cur) {
                    float ph = dp(16);
                    pl->AddRectFilled(ImVec2(ib.Min.x, ib.Min.y + (itemH - ph) / 2), ImVec2(ib.Min.x + dp(3), ib.Min.y + (itemH + ph) / 2), gC.accent, dp(1.5f));
                }
                drawText(pl, F.body, ImVec2(ib.Min.x + dp(11), ib.Min.y + (itemH - F.body->FontSize) / 2), gC.text, items[i]);
                if(ip2) {
                    if(*current != i) changed = true;
                    *current = i;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    return changed;
}

// ------------------------------------------------------------------ SelectorBar

bool SelectorBar(const char* id, const char* const items[], int n, int* sel, float width) {
    ImGui::PushID(id);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = width, h = dp(40);
    // items as wide as their text plus padding, spread over the bar when there is room
    float tw = 0;
    for(int i = 0; i < n; i++) tw += TextSize(items[i]).x;
    float pad = std::max(dp(8), std::min(dp(16), (W - tw) / (2 * n)));
    ImGui::ItemSize(ImRect(p, ImVec2(p.x + W, p.y + h)));
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = p.x, selCx = 0;
    for(int i = 0; i < n; i++) {
        float iw = TextSize(items[i]).x + 2 * pad;
        ImRect ib(ImVec2(x, p.y + dp(3)), ImVec2(x + iw, p.y + h - dp(3)));
        ImGuiID iid = win->GetID(i);
        x += iw;
        if(!ImGui::ItemAdd(ib, iid)) continue;
        bool hov, held;
        if(ImGui::ButtonBehavior(ib, iid, &hov, &held)) {
            if(*sel != i) changed = true;
            *sel = i;
        }
        bool s = *sel == i;
        if(hov && !s) dl->AddRectFilled(ib.Min, ib.Max, held ? gC.subtlePressed : gC.subtleHover, dp(4));
        ImVec2 ts = TextSize(items[i]);
        drawText(dl, F.body, ImVec2(ib.Min.x + pad, ib.Min.y + (ib.GetHeight() - ts.y) / 2 - dp(1)), s ? gC.text : held ? gC.text3 : gC.text2, items[i]);
        if(s) selCx = (ib.Min.x + ib.Max.x) / 2;
    }
    // selection indicator: 16 x 3 accent pill, slides between items
    ImGuiID aid = win->GetID("#ind");
    float* ax = ImGui::GetStateStorage()->GetFloatRef(aid, -1);
    if(*ax < 0) *ax = selCx;
    float d = selCx - *ax;
    *ax += d * (1 - std::exp(-ImGui::GetIO().DeltaTime * 16));
    if(std::fabs(selCx - *ax) < 0.5f) *ax = selCx;
    float stretch = std::min(dp(24), std::fabs(selCx - *ax) * 0.3f);
    float iw = dp(16) + stretch;
    dl->AddRectFilled(ImVec2(*ax - iw / 2, p.y + h - dp(6)), ImVec2(*ax + iw / 2, p.y + h - dp(3)), gC.accent, dp(1.5f));
    ImGui::PopID();
    return changed;
}

// ------------------------------------------------------------------ ScrollViewer

static float gGutter = 0;

void BeginScroll(const char* id, ImVec2 size, float wheelLines) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGuiStorage* st = &w->StateStorage;
    ImGuiID kT = w->GetID("#tgt"), kA = w->GetID("#anim");
    bool anim = st->GetBool(kA, false);
    float target = anim ? st->GetFloat(kT, w->Scroll.y) : w->Scroll.y;
    ImGuiIO& io = ImGui::GetIO();
    float maxY = w->ScrollMax.y;
    if(ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0 && !ImGui::IsAnyItemActive()) {
        float wheel = io.MouseWheel;
        // mouse wheels send whole notches (120 units); precision touchpads send small steps that should move directly
        bool precise = std::fabs(wheel) < 0.99f || std::fabs(wheel - std::round(wheel)) > 0.01f;
        float step = dp(20) * std::max(1.f, wheelLines);
        target = std::clamp(target - wheel * step, 0.f, maxY);
        if(precise) {
            ImGui::SetScrollY(target);
            anim = false;
        } else {
            anim = true;
        }
    }
    if(anim) {
        float cur = w->Scroll.y;
        cur += (target - cur) * (1 - std::exp(-io.DeltaTime * 14.f));
        if(std::fabs(target - cur) < 0.5f) cur = target, anim = false;
        ImGui::SetScrollY(cur);
    }
    st->SetBool(kA, anim);
    st->SetFloat(kT, target);
    gGutter = dp(12);
}

void EndScroll() {
    Gap(12);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if(w->ScrollMax.y > 0) {
        ImRect r = w->InnerRect;
        float viewH = r.GetHeight(), total = viewH + w->ScrollMax.y;
        float trackPad = dp(4);
        float thumbH = std::max(dp(28), (viewH - 2 * trackPad) * viewH / total);
        float range = viewH - 2 * trackPad - thumbH;
        ImRect zone(ImVec2(r.Max.x - dp(14), r.Min.y), r.Max);
        ImGuiID id = w->GetID("#sb");
        ImGuiStorage* st = &w->StateStorage;
        ImGui::ItemAdd(zone, id, nullptr, ImGuiItemFlags_NoNav);
        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(zone, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
        float t = w->Scroll.y / w->ScrollMax.y;
        float ty = r.Min.y + trackPad + range * t;
        ImGuiID kG = w->GetID("#grab");
        if(pressed) {
            float my = ImGui::GetIO().MousePos.y;
            st->SetFloat(kG, my >= ty && my <= ty + thumbH ? my - ty : thumbH / 2);
        }
        if(held && range > 0) {
            float my = ImGui::GetIO().MousePos.y;
            float nt = std::clamp((my - st->GetFloat(kG, 0) - r.Min.y - trackPad) / range, 0.f, 1.f);
            ImGui::SetScrollY(nt * w->ScrollMax.y);
            st->SetBool(w->GetID("#anim"), false);
        }
        float* ex = st->GetFloatRef(w->GetID("#ex"), 0);
        *ex = approach(*ex, hovered || held ? 1.f : 0.f, 6.f);
        float e = ease(*ex);
        // the thin indicator shows while the pointer is over the list, or for a moment after scrolling
        float* seen = st->GetFloatRef(w->GetID("#seen"), -10.f);
        float* lastY = st->GetFloatRef(w->GetID("#lastY"), w->Scroll.y);
        float now = (float)ImGui::GetTime();
        if(ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || *lastY != w->Scroll.y || held)
            *seen = now;
        *lastY = w->Scroll.y;
        float* vis = st->GetFloatRef(w->GetID("#vis"), 0);
        *vis = approach(*vis, now - *seen < 1.2f ? 1.f : 0.f, 4.f);
        float bw = dp(2) + (dp(6) - dp(2)) * e;
        float cx = r.Max.x - dp(6);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if(e > 0) dl->AddRectFilled(ImVec2(cx - dp(6), r.Min.y + dp(2)), ImVec2(cx + dp(6), r.Max.y - dp(2)), withAlpha(gC.flyout, 0.9f * e), dp(6));
        if(*vis > 0) dl->AddRectFilled(ImVec2(cx - bw / 2, ty), ImVec2(cx + bw / 2, ty + thumbH), withAlpha(gC.strongFill, *vis), bw / 2);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    gGutter = 0;
}

// ------------------------------------------------------------------ cards and rows

static struct {
    bool open = false;
    float x0 = 0, w = 0, y0 = 0;
    float rowY = 0, rowH = 0;
} gCard;

void Header(const char* text, bool first) {
    Gap(first ? 2 : 18);
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawText(ImGui::GetWindowDrawList(), F.strong, ImVec2(p.x + dp(1), p.y), gC.text, text);
    ImGui::Dummy(ImVec2(0, F.strong->FontSize));
    Gap(8);
}

void BeginCard() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    gCard.open = true;
    gCard.x0 = p.x;
    gCard.w = ImGui::GetContentRegionAvail().x - gGutter;
    gCard.y0 = p.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
}

void EndCard() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y1 = ImGui::GetCursorScreenPos().y;
    dl->ChannelsSetCurrent(0);
    ImRect r(ImVec2(gCard.x0, gCard.y0), ImVec2(gCard.x0 + gCard.w, y1));
    dl->AddRectFilled(r.Min, r.Max, gC.card, dp(4));
    dl->AddRect(r.Min, r.Max, gDark ? IM_COL32(0, 0, 0, 50) : gC.cardBorder, dp(4));
    dl->ChannelsMerge();
    gCard.open = false;
    Gap(4);
}

void RowBegin(const char* icon, const char* title, const char* desc, ImVec2 controlSize, bool sub) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float padX = dp(16);
    float x0 = gCard.x0, w = gCard.w;
    float y0 = ImGui::GetCursorScreenPos().y;
    float tx = x0 + padX;
    if(sub) tx = x0 + dp(52);
    float iconX = tx;
    if(icon && !sub) tx += dp(20) + dp(16);
    float textW = std::max(dp(40), x0 + w - padX - controlSize.x - dp(16) - tx);
    ImVec2 tsz = title ? F.body->CalcTextSizeA(F.body->FontSize, FLT_MAX, textW, title) : ImVec2(0, 0);
    ImVec2 dsz = desc ? F.caption->CalcTextSizeA(F.caption->FontSize, FLT_MAX, textW, desc) : ImVec2(0, 0);
    float th = tsz.y + (desc ? dp(1) + dsz.y : 0);
    float h = sub ? dp(48) : desc ? dp(62) : dp(52);
    h = std::max(h, std::max(th, controlSize.y) + dp(20));
    if(icon && !sub) drawIcon(dl, icon, ImVec2(iconX + dp(10), y0 + h / 2), gC.text);
    float ty = y0 + (h - th) / 2;
    if(title) drawText(dl, F.body, ImVec2(tx, ty), gC.text, title, textW);
    if(desc) drawText(dl, F.caption, ImVec2(tx, ty + tsz.y + dp(1)), gC.text2, desc, textW);
    gCard.rowY = y0;
    gCard.rowH = h;
    ImGui::SetCursorScreenPos(ImVec2(x0 + w - padX - controlSize.x, y0 + std::round((h - controlSize.y) / 2)));
}

void RowEnd() {
    ImGui::SetCursorScreenPos(ImVec2(gCard.x0, gCard.rowY + gCard.rowH));
    ImGui::Dummy(ImVec2(gCard.w, 0));
}

void RowDivider() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(gCard.x0, p.y + 0.5f), ImVec2(gCard.x0 + gCard.w, p.y + 0.5f), gC.divider, 1.f);
    ImGui::SetCursorScreenPos(ImVec2(gCard.x0, p.y + 1));
    ImGui::Dummy(ImVec2(gCard.w, 0));
}

bool ToggleRow(const char* icon, const char* title, const char* desc, bool* v) {
    ImGui::PushID(title);
    RowBegin(icon, title, desc, ImVec2(ToggleSwitchWidth(), std::max(dp(20), F.body->FontSize)));
    bool r = ToggleSwitch("##t", v);
    RowEnd();
    ImGui::PopID();
    return r;
}

bool ComboRow(const char* icon, const char* title, const char* desc, int* current, const char* const items[], int n) {
    ImGui::PushID(title);
    float cw = ComboWidth(items, n);
    RowBegin(icon, title, desc, ImVec2(cw, dp(32)));
    bool r = Combo("##c", current, items, n, cw);
    RowEnd();
    ImGui::PopID();
    return r;
}

bool SliderRow(const char* icon, const char* title, const char* desc, float* v, float lo, float hi, const char* fmt, float step,
               float fmtScale) {
    ImGui::PushID(title);
    float vw = fmt ? dp(52) : 0, sw = dp(140);
    RowBegin(icon, title, desc, ImVec2(sw + vw, dp(32)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = Slider("##s", v, lo, hi, sw, fmt, step, fmtScale);
    if(fmt) {
        char b[32];
        fmtValue(b, sizeof b, fmt, *v, fmtScale);
        ImVec2 ts = TextSize(b);
        drawText(ImGui::GetWindowDrawList(), F.body, ImVec2(p.x + sw + vw - ts.x, p.y + (dp(32) - ts.y) / 2), gC.text2, b);
    }
    RowEnd();
    ImGui::PopID();
    return r;
}

bool SubSlider(const char* label, float* v, float lo, float hi, const char* fmt, float step, float fmtScale) {
    ImGui::PushID(label);
    float lx = gCard.x0 + dp(52);
    float lw = TextSize(label).x;
    float vw = fmt ? dp(52) : 0;
    float sw = gCard.x0 + gCard.w - dp(16) - (lx + lw + dp(16)) - vw;
    RowBegin(nullptr, label, nullptr, ImVec2(sw + vw, dp(32)), true);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = Slider("##s", v, lo, hi, sw, fmt, step, fmtScale);
    if(fmt) {
        char b[32];
        fmtValue(b, sizeof b, fmt, *v, fmtScale);
        ImVec2 ts = TextSize(b);
        drawText(ImGui::GetWindowDrawList(), F.body, ImVec2(p.x + sw + vw - ts.x, p.y + (dp(32) - ts.y) / 2), gC.text2, b);
    }
    RowEnd();
    ImGui::PopID();
    return r;
}

bool SubToggle(const char* label, bool* v) {
    ImGui::PushID(label);
    RowBegin(nullptr, label, nullptr, ImVec2(ToggleSwitchWidth(), std::max(dp(20), F.body->FontSize)), true);
    bool r = ToggleSwitch("##t", v);
    RowEnd();
    ImGui::PopID();
    return r;
}

float SubBegin(float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    gCard.rowY = p.y;
    gCard.rowH = height;
    ImGui::SetCursorScreenPos(ImVec2(gCard.x0 + dp(16), p.y));
    return gCard.w - dp(32);
}

void SubEnd(float height) {
    gCard.rowH = height;
    RowEnd();
}

}  // namespace fluent
