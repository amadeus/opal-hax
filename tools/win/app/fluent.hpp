// fluent: Windows 11 (WinUI 3 / Fluent) controls for the Dear ImGui popover, drawn to WinUI's own metrics and colour
// tokens (ToggleSwitch, Slider, ComboBox, Button, CheckBox, TextBox, SelectorBar, settings cards, ScrollViewer with
// smooth wheel scrolling and an overlay scroll bar). Light and dark theme, system accent colour, DPI scaled.
// No Windows headers: also built on macOS by preview/ to screenshot the UI.
#pragma once
#include <cstddef>

#include "imgui.h"

namespace fluent {

struct Colors {
    ImU32 bg, footer, card, cardBorder, divider;
    ImU32 text, text2, text3, textDisabled;
    ImU32 control, controlHover, controlPressed, controlDisabled, controlStroke, controlStroke2;
    ImU32 strongStroke, strongFill, altFill, altFillHover, altFillPressed;
    ImU32 subtleHover, subtlePressed;
    ImU32 thumb, flyout, flyoutBorder, inputActive, critical;
    ImU32 accent, accentHover, accentPressed, onAccent;
    ImU32 previewBg;
};

struct Fonts {
    ImFont *body = nullptr, *strong = nullptr, *caption = nullptr, *icons = nullptr, *iconsSmall = nullptr;
};

// theme: accentDark is the accent used on dark backgrounds (Windows' "AccentLight2"), accentLight the one on light
// backgrounds ("AccentDark1"); 0 = Windows' default blue
void setTheme(bool dark, ImU32 accentDark = 0, ImU32 accentLight = 0);
bool isDark();
const Colors& C();
extern Fonts F;

void setScale(float s);  // DPI scale (1 = 96 dpi)
float dp(float v);       // device-independent pixels -> pixels

// rebuilds the font atlas: call before NewFrame. emFactor = (ascender - descender) / unitsPerEm of the text font
// (Segoe UI: 1.33), so sizes match WinUI's point sizes (Body 14, Caption 12).
bool loadFonts(const char* regularTtf, const char* semiboldTtf, float emFactor);
void applyStyle();  // ImGui style for popups, text input and the window (after setTheme/setScale)

// ---- controls (each one is a single ImGui item at the cursor; return true when the value changed / was clicked)
bool ToggleSwitch(const char* id, bool* v, bool onOffLabel = true);
float ToggleSwitchWidth(bool onOffLabel = true);
bool Slider(const char* id, float* v, float lo, float hi, float width, const char* fmt = nullptr, float step = 0, float fmtScale = 1);
bool Combo(const char* id, int* current, const char* const items[], int n, float width);
float ComboWidth(const char* const items[], int n);
bool Button(const char* label, ImVec2 size = ImVec2(0, 0), bool accent = false);
bool ToggleButton(const char* label, bool selected, ImVec2 size);
bool CheckBox(const char* label, bool* v);
bool IconButton(const char* id, const char* icon, bool toggled, float size);
bool TextBox(const char* id, const char* hint, char* buf, size_t n, float width);
bool SelectorBar(const char* id, const char* const items[], int n, int* sel, float width);

// ---- layout
// ScrollViewer: wheel notches scroll smoothly, touchpads scroll directly; thin scroll bar that widens on hover
void BeginScroll(const char* id, ImVec2 size, float wheelLines = 3);
void EndScroll();
void Header(const char* text, bool first = false);  // section header above a group of cards
void BeginCard();                                   // settings card: background + border drawn behind its rows
void EndCard();
void Gap(float dpv);
// a card row: icon + title (+ description) on the left, a control of the given size on the right. RowBegin leaves the
// cursor where the control goes; RowEnd moves below the row.
void RowBegin(const char* icon, const char* title, const char* desc, ImVec2 controlSize, bool sub = false);
void RowEnd();
void RowDivider();
bool ToggleRow(const char* icon, const char* title, const char* desc, bool* v);
bool ComboRow(const char* icon, const char* title, const char* desc, int* current, const char* const items[], int n);
bool SliderRow(const char* icon, const char* title, const char* desc, float* v, float lo, float hi, const char* fmt = nullptr,
               float step = 0, float fmtScale = 1);
// sub row inside an expanded card: label on the left, slider filling the rest
bool SubSlider(const char* label, float* v, float lo, float hi, const char* fmt = nullptr, float step = 0, float fmtScale = 1);
bool SubToggle(const char* label, bool* v);
// card content area with the card's padding (for custom content); returns the width available
float SubBegin(float height);
void SubEnd(float height);
// plain text helpers
void Text(const char* s, ImU32 col = 0, ImFont* font = nullptr);
ImVec2 TextSize(const char* s, ImFont* font = nullptr);

}  // namespace fluent
