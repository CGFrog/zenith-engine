#include "editor/theme/PixelIconBitmaps.hpp"

namespace {
using namespace palette;

constexpr std::array<IconBitmap, editorIconCount> bitmaps{{
    // ToolTweak: the select cursor.
    {iconWhite,
     {"o.........", "oo........", "o3o.......", "o33o......", "o311o.....", "o3111o....", "o31111o...", "o311oooo..",
      "o1oo1o....", "oo..o1o..."}},
    // ToolMove: four arrows.
    {iconTeal,
     {"....oo....", "...o33o...", "....11....", ".o..11..o.", "o31111113o", "o21111112o", ".o..11..o.", "....11....",
      "...o22o...", "....oo...."}},
    // ToolRotate: a circling arrow.
    {iconGreen,
     {"...oooo...", "..o3333o..", ".o3oooo3o.", "o3o...oooo", "o1o...o11o", "o1o....oo.", "o1o.......", ".o1o..oo..",
      "..o1111o..", "...oooo..."}},
    // ToolScale: a box with an outward arrow.
    {iconOrange,
     {".....ooooo", "......o33o", ".....o3o1o", "....o3o.oo", "oooooo....", "o3333o....", "o3111o....", "o3111o....",
      "o2222o....", "oooooo...."}},
    // Camera.
    {iconBlue,
     {"..........", "..........", "oooooooo.o", "o333333ooo", "o31ww113oo", "o31ww111oo", "o3111111oo", "o2222222oo",
      "oooooooo.o", ".........."}},
    // Sun.
    {iconYellow,
     {"o...oo...o", ".o..33..o.", "...o33o...", "..o3333o..", "oo311113oo", "oo311112oo", "..o2112o..", "...o22o...",
      ".o..22..o.", "o...oo...o"}},
    // PointLight: a bulb.
    {iconTeal,
     {"...oooo...", "..o3333o..", ".o333311o.", ".o311111o.", ".o311112o.", "..o1112o..", "...o12o...", "...owwo...",
      "...o22o...", "....oo...."}},
    // SpotLight: a cone.
    {iconTeal,
     {"...oooo...", "...o33o...", "..o3113o..", "..o3111o..", ".o311111o.", ".o311112o.", "o31111112o", "o21111122o",
      "oooooooooo", ".........."}},
    // Environment: a globe.
    {iconBlue,
     {"...oooo...", "..o3w33o..", ".o3w3311o.", "o33w31111o", "o1ww11111o", "o11w11121o", "o111w1122o", ".o11w122o.",
      "..o2w22o..", "...oooo..."}},
    // Fog: a cloud over mist.
    {iconSlate,
     {"..........", "...ooo....", "..o333oo..", ".oo33333o.", "o33333333o", "o31111113o", "o21111112o", ".oooooooo.",
      "..........", "..o.o.o.o."}},
    // ReflectionProbe: a mirrored sphere.
    {iconSlate,
     {"...oooo...", "..o3333o..", ".o3ww311o.", "o3ww31111o", "o3w311111o", "o31111112o", "o31111122o", ".o111122o.",
      "..o2222o..", "...oooo..."}},
    // IrradianceVolume: a grid of probes.
    {iconPurple,
     {"oooooooooo", "o33333333o", "o3w11w11wo", "o31111111o", "o31111111o", "o1w11w11wo", "o11111111o", "o11111111o",
      "o2w22w22wo", "oooooooooo"}},
    // Decal: a sticker with a folded corner.
    {iconOrange,
     {"ooooooo...", "o333333o..", "o311111wo.", "o311111wwo", "o31111111o", "o31111111o", "o31111111o", "o21111112o",
      "o22222222o", "oooooooooo"}},
    // Particles: sparkles.
    {iconYellow,
     {".o........", "ooo...o...", ".o...o3o..", "....o333o.", ".....o3o..", "..o...o...", ".o3o......", "o333o...o.",
      ".o3o...ooo", "..o.....o."}},
    // AudioEmitter: a speaker with waves.
    {iconPurple,
     {".....o....", "....o3o.o.", ".ooo31o..o", "o3331o.o.o", "o3111o.o.o", "o2111o.o.o", "o2221o.o.o", ".ooo21o..o",
      "....o2o.o.", ".....o...."}},
    // AudioListener: headphones.
    {iconPurple,
     {"..oooooo..", ".o333333o.", "o3o....o3o", "o1o....o1o", "oooo..oooo", "o33o..o33o", "o11o..o11o", "o11o..o11o",
      "o22o..o22o", "oooo..oooo"}},
    // Physics: a solid ball.
    {iconRed,
     {"...oooo...", "..o3333o..", ".o3ww311o.", "o3w311111o", "o31111111o", "o31111111o", "o21111112o", ".o211122o.",
      "..o2222o..", "...oooo..."}},
    // Mesh: a cube.
    {iconOrange,
     {"...oooo...", ".oo3333oo.", "o33333333o", "oo333333oo", "o1oo33oo2o", "o111oo222o", "o111o2222o", "o111o2222o",
      ".oo1o22oo.", "...oooo..."}},
    // Empty: axis ticks around an origin.
    {iconSlate,
     {"....oo....", "....33....", "....11....", "..........", "o311oo113o", "o211oo112o", "..........", "....11....",
      "....22....", "....oo...."}},
    // Scene: a clapperboard.
    {iconSlate,
     {"oooooooooo", "o3w3w3w3wo", "oooooooooo", "o11111111o", "o11111111o", "o11111111o", "o11111111o", "o21111112o",
      "o22222222o", "oooooooooo"}},
    // Folder.
    {iconOrange,
     {"..........", "ooooo.....", "o333oooooo", "o33333333o", "o31111111o", "o31111111o", "o31111111o", "o21111112o",
      "o22222222o", "oooooooooo"}},
    // Texture: a picture with a moon over hills.
    {iconBlue,
     {"oooooooooo", "o333333wwo", "o333333wwo", "o33o33333o", "o3o1o33o3o", "oo111oo1oo", "o11111111o", "o11111111o",
      "o22222222o", "oooooooooo"}},
    // AudioClip: a note.
    {iconPurple,
     {"....oo....", "....o3o...", "....o33o..", "....o1o3o.", "....o1oo3o", "....o1o.oo", ".oooo1o...", "o3331o....",
      "o2211o....", ".oooo....."}},
    // Collection: a lidded box.
    {iconWhite,
     {"..........", ".oooooooo.", "o33333333o", "oooooooooo", "o31111113o", "o31oooo13o", "o31111113o", "o21111112o",
      "o22222222o", "oooooooooo"}},
    // Prefab: a cube with a linked face.
    {iconBlue,
     {"...oooo...", ".oo3333oo.", "o33333333o", "oo333333oo", "o1oowwoo2o", "o11ow2o22o", "o11ow2o22o", "o111oo222o",
      ".oo1o22oo.", "...oooo..."}},
    // Components: a puzzle piece.
    {iconTeal,
     {"...oo.....", "..o33o....", "ooo33oooo.", "o3333333o.", "o3111111oo", "o31111113o", "o31111111o", "o3111111oo",
      "o2222222o.", "ooooooooo."}},
    // Outliner: a list.
    {iconTeal,
     {"ooo.oooooo", "o3o.o3333o", "ooo.oooooo", "ooo.oooooo", "o1o.o1111o", "ooo.oooooo", "ooo.oooooo", "o2o.o2222o",
      "ooo.oooooo", ".........."}},
    // Properties: sliders.
    {iconTeal,
     {"..oo......", "oo33oooooo", "..oo......", "......oo..", "oooooo11oo", "......oo..", "....oo....", "oooo22oooo",
      "....oo....", ".........."}},
    // Viewport: an eye.
    {iconBlue,
     {"..........", "...oooo...", ".oo3333oo.", "o333oo333o", "o31owwo11o", "o21owwo12o", ".oo2222oo.", "...oooo...",
      "..........", ".........."}},
    // Statistics: a bar chart.
    {iconGreen,
     {"......ooo.", "......o3o.", "...oooo1o.", "...o3oo1o.", "oooo1oo1o.", "o3oo1oo1o.", "o1oo1oo1o.", "o1oo1oo1o.",
      "o2oo2oo2o.", "ooooooooo."}},
    // Build: a hammer.
    {iconOrange,
     {".oooooo...", "o333333o..", "o222222o..", ".ooo1ooo..", "...o1o....", "...o1o....", "...o1o....", "...o1o....",
      "...o2o....", "...ooo...."}},
    // Error: a red orb with a cross.
    {iconRed,
     {"...oooo...", "..o3333o..", ".o3w33w3o.", "o333ww333o", "o311ww111o", "o21w11w12o", ".o211112o.", "..o2222o..",
      "...oooo...", ".........."}},
    // Warning: an amber triangle with a mark.
    {iconYellow,
     {"....oo....", "...o33o...", "...o33o...", "..o3ww3o..", "..o3ww3o..", ".o31ww13o.", ".o311113o.", "o311ww111o",
      "o22222222o", "oooooooooo"}},
    // Info: a teal orb with an i.
    {iconTeal,
     {"...oooo...", "..o3ww3o..", ".o33ww33o.", "o33333333o", "o311ww113o", "o311ww111o", "o211ww112o", ".o21ww12o.",
      "..o2222o..", "...oooo..."}},
    // Success: a green orb with a check.
    {iconGreen,
     {"...oooo...", "..o3333o..", ".o33333wo.", "o3333ww33o", "o3w3ww333o", "o31www111o", "o211w1112o", ".o211112o.",
      "..o2222o..", "...oooo..."}},
    // Play.
    {iconGreen,
     {".oo.......", ".o3oo.....", ".o333oo...", ".o31133oo.", ".o3111113o", ".o2111112o", ".o21122oo.", ".o222oo...",
      ".o2oo.....", ".oo......."}},
    // Pause.
    {iconTeal,
     {".ooo..ooo.", ".o3o..o3o.", ".o3o..o3o.", ".o1o..o1o.", ".o1o..o1o.", ".o1o..o1o.", ".o1o..o1o.", ".o2o..o2o.",
      ".o2o..o2o.", ".ooo..ooo."}},
    // Stop.
    {iconRed,
     {"..........", ".oooooooo.", ".o333333o.", ".o311111o.", ".o311111o.", ".o311111o.", ".o211112o.", ".o222222o.",
      ".oooooooo.", ".........."}},
    // Close: a cross.
    {iconSlate,
     {"..........", ".oo....oo.", ".o3o..o3o.", "..o3oo3o..", "...o11o...", "...o11o...", "..o2oo2o..", ".o2o..o2o.",
      ".oo....oo.", ".........."}},
}};

ThemeColor scaled(ThemeColor color, float factor) {
    return {color.r * factor, color.g * factor, color.b * factor, color.a};
}

ThemeColor lightened(ThemeColor color, float amount) {
    return {color.r + (1.0f - color.r) * amount, color.g + (1.0f - color.g) * amount,
            color.b + (1.0f - color.b) * amount, color.a};
}
} // namespace

const IconBitmap& iconBitmap(EditorIcon icon) {
    return bitmaps[static_cast<size_t>(icon)];
}

std::optional<ThemeColor> iconPixelColor(const IconBitmap& bitmap, char pixel) {
    switch (pixel) {
    case 'o':
        return iconOutline;
    case '1':
        return bitmap.hue;
    case '2':
        return scaled(bitmap.hue, 0.62f);
    case '3':
        return lightened(bitmap.hue, 0.45f);
    case 'w':
        return iconWhite;
    default:
        return std::nullopt;
    }
}
