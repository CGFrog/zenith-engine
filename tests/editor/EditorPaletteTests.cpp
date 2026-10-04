#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/PixelIconBitmaps.hpp"
#include "editor/theme/ThemeParameters.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr float textContrast = 4.5f;
constexpr float graphicContrast = 3.0f;

void testContrastHelpers() {
    const ThemeColor black = ThemeColor::hex(0x000000);
    const ThemeColor white = ThemeColor::hex(0xFFFFFF);
    expect(std::abs(contrastRatio(black, white) - 21.0f) < 0.01f, "black on white is 21:1");
    expect(std::abs(contrastRatio(white, white) - 1.0f) < 0.001f, "a colour on itself is 1:1");
    const ThemeColor half = composite(white.withAlpha(0.5f), black);
    expect(std::abs(half.r - 0.5f) < 0.001f && half.a == 1.0f, "compositing mixes by alpha into an opaque colour");
}

// Text the panes, fields, headers, buttons, and rows carry stays readable.
void testTextContrast() {
    using namespace palette;
    const ThemeColor rows[] = {pane,       paneAlternate,         field, fieldHovered, rowSelected,
                               rowHovered, composite(popup, pane)};
    for (const ThemeColor surface : rows) {
        for (const ThemeColor text : {ink, success, info, warning, error}) {
            expect(contrastRatio(text, surface) >= textContrast, "text and severity colours reach 4.5:1 on rows");
        }
    }
    for (const ThemeColor surface : {header, section, sectionHovered, button, buttonHovered, buttonPressed}) {
        expect(contrastRatio(ink, surface) >= textContrast, "ink reaches 4.5:1 on headers and buttons");
        expect(contrastRatio(warning, surface) >= textContrast, "warning ink reads on headers and buttons");
    }
    for (const ThemeColor surface : {header, composite(overlayPanel, pane)}) {
        expect(contrastRatio(captionInk, surface) >= textContrast, "captions read on header bars");
    }
    for (const ThemeColor surface : {pane, field, paneAlternate, header}) {
        expect(contrastRatio(inkMuted, surface) >= textContrast, "muted text reaches 4.5:1 on panes and fields");
        expect(contrastRatio(inkDisabled, surface) >= graphicContrast, "disabled text stays distinguishable");
    }
    for (const ThemeColor surface : {section, sectionHovered}) {
        expect(contrastRatio(sectionInk, surface) >= textContrast, "section titles read on their bar");
    }
    expect(contrastRatio(activeInk, buttonActive) >= textContrast, "lit buttons keep their text readable");
    for (const ThemeColor axis : {axisX, axisY, axisZ, activeObjectLabel}) {
        expect(contrastRatio(axis, composite(overlayPanel, pane)) >= graphicContrast,
               "axis and label colours stand out from the viewport's overlay panels");
    }
}

// The bevel is what makes a control read as raised: its highlight stands out
// from every raised fill, and its shade from the gap between areas.
void testBevelsReadAsEdges() {
    using namespace palette;
    for (const ThemeColor surface : {button, section, header}) {
        expect(contrastRatio(bevelHighlight, surface) >= graphicContrast, "the bevel highlight stands out");
    }
    expect(contrastRatio(buttonActiveHighlight, buttonActive) >= graphicContrast,
           "a lit button's highlight stands out from its fill");
    expect(relativeLuminance(areaGap) < relativeLuminance(pane) &&
               relativeLuminance(pane) < relativeLuminance(header) &&
               relativeLuminance(header) < relativeLuminance(button),
           "surfaces step up in lightness from the gap to the controls, as Blender's do");
}

// Every accent works as the focus ring, overline, and drag target on what it
// is drawn over; the default is Blender's blue.
void testAccentContrast() {
    using namespace palette;
    expect(themeAccents().front().accent == ThemeAccent::Blue && themeAccentInfo(ThemeAccent::Blue).color == accentBlue,
           "the default accent is the theme's blue");
    for (const ThemeAccentInfo& accent : themeAccents()) {
        for (const ThemeColor surface :
             {pane, paneAlternate, field, fieldHovered, header, button, section, rowSelected}) {
            expect(contrastRatio(accent.color, surface) >= graphicContrast,
                   "accent " + std::string(accent.key) + " stands out from panes, fields, headers, and buttons");
        }
        expect(parseThemeAccent(accent.key) == accent.accent, "accent " + std::string(accent.key) + " parses back");
    }
    expect(!parseThemeAccent("pink").has_value(), "unknown accents are rejected");
    expect(clampedThemeParameters({.accent = static_cast<ThemeAccent>(99)}).accent == ThemeAccent::Blue,
           "an unknown accent is clamped to the default");
    expect(clampedThemeParameters(ThemeParameters{}) == ThemeParameters{}, "the defaults are in range");
}

void testIconBitmaps() {
    for (size_t index = 0; index < editorIconCount; ++index) {
        const IconBitmap& bitmap = iconBitmap(static_cast<EditorIcon>(index));
        bool valid = true;
        bool outlined = false;
        for (const std::string_view row : bitmap.rows) {
            valid = valid && row.size() == iconBitmapSize;
            for (const char pixel : row) {
                valid = valid && (pixel == '.' || iconPixelColor(bitmap, pixel).has_value());
                outlined = outlined || pixel == 'o';
            }
        }
        expect(valid, "icon " + std::to_string(index) + " is a square of known pixels");
        expect(outlined, "icon " + std::to_string(index) + " has an outline");
    }
    expect(!iconPixelColor(iconBitmap(EditorIcon::Close), '.').has_value(), "'.' is transparent");
}
} // namespace

int main() {
    testContrastHelpers();
    testTextContrast();
    testBevelsReadAsEdges();
    testAccentContrast();
    testIconBitmaps();
    if (failures != 0) {
        std::cerr << failures << " editor palette test(s) failed\n";
        return 1;
    }
    std::cout << "Editor palette tests passed\n";
    return 0;
}
