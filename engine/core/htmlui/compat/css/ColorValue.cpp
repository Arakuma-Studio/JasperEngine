// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/NamedColors.h"
#include "htmlui/compat/css/StaticCalc.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace jasperengine::htmlui::compat;

namespace {

    using Vec3 = std::array<double, 3>;
    using Mat3 = std::array<std::array<double, 3>, 3>;

    constexpr double kPi = 3.14159265358979323846;

    Vec3 mul(const Mat3& m, const Vec3& v){
        return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
                m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
    }

    // CSS Color 4 §18 sample code
    const Mat3 kLinSrgbToXyz = {{{0.41239079926595934, 0.357584339383878, 0.1804807884018343},
                                 {0.21263900587151027, 0.715168678767756, 0.07219231536073371},
                                 {0.01933081871559182, 0.11919477979462598, 0.9505321522496607}}};
    const Mat3 kXyzToLinSrgb = {{{3.2409699419045226, -1.537383177570094, -0.4986107602930034},
                                 {-0.9692436362808796, 1.8759675015077202, 0.04155505740717559},
                                 {0.05563007969699366, -0.20397695888897652, 1.0569715142428786}}};
    const Mat3 kLinP3ToXyz = {{{0.4865709486482162, 0.26566769316909306, 0.1982172852343625},
                               {0.2289745640697488, 0.6917385218365064, 0.079286914093745},
                               {0.0, 0.04511338185890264, 1.043944368900976}}};
    const Mat3 kXyzToLinP3 = {{{2.4934969119414254, -0.9313836179191239, -0.40271078445071684},
                               {-0.8294889695615747, 1.7626640603183463, 0.023624685841943577},
                               {0.03584583024378447, -0.07617238926804182, 0.9568845240076872}}};
    const Mat3 kLinA98ToXyz = {{{0.5766690429101305, 0.1855582379065463, 0.1882286462349947},
                                {0.29734497525053605, 0.6273635662554661, 0.0752914584939978},
                                {0.02703136138641234, 0.07068885253582723, 0.9913375368376388}}};
    const Mat3 kXyzToLinA98 = {{{2.0415879038107465, -0.5650069742788596, -0.34473135077832956},
                                {-0.9692436362808795, 1.8759675015077202, 0.04155505740717557},
                                {0.013444280632031142, -0.11836239223101838, 1.0151749943912054}}};
    const Mat3 kLinProphotoToXyzD50 = {{{0.7977666449006423, 0.13518129740053308, 0.0313477341283922},
                                        {0.2880748288194013, 0.711835234241873, 0.00008993693872564},
                                        {0.0, 0.0, 0.8251046025104602}}};
    const Mat3 kXyzD50ToLinProphoto = {{{1.3457868816471583, -0.25557208737979464, -0.05110186497554526},
                                        {-0.5446307051249019, 1.5082477428451468, 0.02052744743642139},
                                        {0.0, 0.0, 1.2119675456389452}}};
    const Mat3 kLin2020ToXyz = {{{0.6369580483012914, 0.14461690358620832, 0.1688809751641721},
                                 {0.2627002120112671, 0.6779980715188708, 0.05930171646986196},
                                 {0.0, 0.028072693049087428, 1.060985057710791}}};
    const Mat3 kXyzToLin2020 = {{{1.7166511879712674, -0.35567078377639233, -0.25336628137365974},
                                 {-0.6666843518324892, 1.6164812366349395, 0.01576854581391113},
                                 {0.017639857445310783, -0.042770613257808524, 0.9421031212354738}}};
    const Mat3 kD50ToD65 = {{{0.955473421488075, -0.02309845494876471, 0.06325924320057072},
                             {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323},
                             {0.012314014864481998, -0.020507649298898964, 1.330365926242124}}};
    const Mat3 kD65ToD50 = {{{1.0479297925449969, 0.022946870601609652, -0.05019226628920524},
                             {0.02962780877005599, 0.9904344267538799, -0.017073799063418826},
                             {-0.009243040646204504, 0.015055191490298152, 0.7518742814281371}}};
    const Mat3 kOklabLmsToXyz = {{{1.2268798758459243, -0.5578149944602171, 0.2812561489664678},
                                  {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
                                  {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}}};
    const Mat3 kOklabToLms = {{{1.0, 0.3963377773761749, 0.2158037573299136},
                               {1.0, -0.1055613458156586, -0.0638541728258133},
                               {1.0, -0.0894841775298119, -1.2914855480194092}}};
    const Mat3 kXyzToOklabLms = {{{0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
                                  {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
                                  {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}}};
    const Mat3 kLmsToOklab = {{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
                               {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
                               {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}}};

    const Vec3 kD50White = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};

    double sign(double v){ return v < 0 ? -1.0 : 1.0; }

    double linSrgb(double c){
        const double a = std::fabs(c);
        return a <= 0.04045 ? c / 12.92 : sign(c) * std::pow((a + 0.055) / 1.055, 2.4);
    }
    double gamSrgb(double c){
        const double a = std::fabs(c);
        return a > 0.0031308 ? sign(c) * (1.055 * std::pow(a, 1.0 / 2.4) - 0.055) : 12.92 * c;
    }
    double linA98(double c){ return sign(c) * std::pow(std::fabs(c), 563.0 / 256.0); }
    double gamA98(double c){ return sign(c) * std::pow(std::fabs(c), 256.0 / 563.0); }
    double linProphoto(double c){
        const double a = std::fabs(c);
        return a <= 16.0 / 512.0 ? c / 16.0 : sign(c) * std::pow(a, 1.8);
    }
    double gamProphoto(double c){
        const double a = std::fabs(c);
        return a >= 1.0 / 512.0 ? sign(c) * std::pow(a, 1.0 / 1.8) : 16.0 * c;
    }
    constexpr double k2020Alpha = 1.09929682680944;
    constexpr double k2020Beta = 0.018053968510807;
    double lin2020(double c){
        const double a = std::fabs(c);
        return a < k2020Beta * 4.5 ? c / 4.5 : sign(c) * std::pow((a + k2020Alpha - 1.0) / k2020Alpha, 1.0 / 0.45);
    }
    double gam2020(double c){
        const double a = std::fabs(c);
        return a > k2020Beta ? sign(c) * (k2020Alpha * std::pow(a, 0.45) - (k2020Alpha - 1.0)) : 4.5 * c;
    }

    Vec3 map3(const Vec3& v, double (*f)(double)){
        return {f(v[0]), f(v[1]), f(v[2])};
    }

    double normalizeHue(double h){
        if (!std::isfinite(h))
            return 0.0;
        h = std::fmod(h, 360.0);
        if (h < 0)
            h += 360.0;
        return h;
    }

    Vec3 hslToSrgb(const Vec3& hsl){
        const double h = normalizeHue(hsl[0]);
        const double s = hsl[1] / 100.0;
        const double l = hsl[2] / 100.0;
        auto f = [&](double n){
            const double k = std::fmod(n + h / 30.0, 12.0);
            const double a = s * std::min(l, 1.0 - l);
            return l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
        };
        return {f(0), f(8), f(4)};
    }

    Vec3 srgbToHsl(const Vec3& rgb){
        const double r = rgb[0], g = rgb[1], b = rgb[2];
        const double maxv = std::max({r, g, b});
        const double minv = std::min({r, g, b});
        double h = 0, s = 0;
        const double l = (minv + maxv) / 2.0;
        const double d = maxv - minv;
        if (d != 0){
            s = (l == 0 || l == 1) ? 0 : (maxv - l) / std::min(l, 1.0 - l);
            if (maxv == r)
                h = (g - b) / d + (g < b ? 6 : 0);
            else if (maxv == g)
                h = (b - r) / d + 2;
            else
                h = (r - g) / d + 4;
            h = h * 60;
        }
        if (s < 0){
            h += 180;
            s = std::fabs(s);
        }
        return {normalizeHue(h), s * 100.0, l * 100.0};
    }

    Vec3 hwbToSrgb(const Vec3& hwb){
        double w = hwb[1] / 100.0;
        double b = hwb[2] / 100.0;
        if (w + b >= 1.0){
            const double gray = w / (w + b);
            return {gray, gray, gray};
        }
        Vec3 rgb = hslToSrgb({hwb[0], 100.0, 50.0});
        for (double& c : rgb)
            c = c * (1.0 - w - b) + w;
        return rgb;
    }

    Vec3 srgbToHwb(const Vec3& rgb){
        const Vec3 hsl = srgbToHsl(rgb);
        const double white = std::min({rgb[0], rgb[1], rgb[2]});
        const double black = 1.0 - std::max({rgb[0], rgb[1], rgb[2]});
        return {hsl[0], white * 100.0, black * 100.0};
    }

    constexpr double kKappa = 24389.0 / 27.0;
    constexpr double kEpsilon = 216.0 / 24389.0;

    Vec3 labToXyzD50(const Vec3& lab){
        const double f1 = (lab[0] + 16.0) / 116.0;
        const double f0 = lab[1] / 500.0 + f1;
        const double f2 = f1 - lab[2] / 200.0;
        const double x = std::pow(f0, 3) > kEpsilon ? std::pow(f0, 3) : (116.0 * f0 - 16.0) / kKappa;
        const double y = lab[0] > kKappa * kEpsilon ? std::pow((lab[0] + 16.0) / 116.0, 3) : lab[0] / kKappa;
        const double z = std::pow(f2, 3) > kEpsilon ? std::pow(f2, 3) : (116.0 * f2 - 16.0) / kKappa;
        return {x * kD50White[0], y * kD50White[1], z * kD50White[2]};
    }

    Vec3 xyzD50ToLab(const Vec3& xyz){
        Vec3 scaled = {xyz[0] / kD50White[0], xyz[1] / kD50White[1], xyz[2] / kD50White[2]};
        Vec3 f;
        for (int i = 0; i < 3; i++)
            f[i] = scaled[i] > kEpsilon ? std::cbrt(scaled[i]) : (kKappa * scaled[i] + 16.0) / 116.0;
        return {116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2])};
    }

    Vec3 polarToRect(const Vec3& lch){
        const double h = lch[2] * kPi / 180.0;
        return {lch[0], lch[1] * std::cos(h), lch[1] * std::sin(h)};
    }

    Vec3 rectToPolar(const Vec3& lab){
        double h = std::atan2(lab[2], lab[1]) * 180.0 / kPi;
        const double c = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
        return {lab[0], c, normalizeHue(h)};
    }

    Vec3 oklabToXyz(const Vec3& lab){
        Vec3 lms = mul(kOklabToLms, lab);
        for (double& v : lms)
            v = v * v * v;
        return mul(kOklabLmsToXyz, lms);
    }

    Vec3 xyzToOklab(const Vec3& xyz){
        Vec3 lms = mul(kXyzToOklabLms, xyz);
        for (double& v : lms)
            v = std::cbrt(v);
        return mul(kLmsToOklab, lms);
    }

    Vec3 toXyzD65(ColorSpace space, const Vec3& c){
        switch (space){
            case ColorSpace::Srgb: return mul(kLinSrgbToXyz, map3(c, linSrgb));
            case ColorSpace::SrgbLinear: return mul(kLinSrgbToXyz, c);
            case ColorSpace::DisplayP3: return mul(kLinP3ToXyz, map3(c, linSrgb));
            case ColorSpace::A98Rgb: return mul(kLinA98ToXyz, map3(c, linA98));
            case ColorSpace::ProphotoRgb: return mul(kD50ToD65, mul(kLinProphotoToXyzD50, map3(c, linProphoto)));
            case ColorSpace::Rec2020: return mul(kLin2020ToXyz, map3(c, lin2020));
            case ColorSpace::XyzD50: return mul(kD50ToD65, c);
            case ColorSpace::XyzD65: return c;
            case ColorSpace::Lab: return mul(kD50ToD65, labToXyzD50(c));
            case ColorSpace::Lch: return mul(kD50ToD65, labToXyzD50(polarToRect(c)));
            case ColorSpace::Oklab: return oklabToXyz(c);
            case ColorSpace::Oklch: return oklabToXyz(polarToRect(c));
            case ColorSpace::Hsl: return mul(kLinSrgbToXyz, map3(hslToSrgb(c), linSrgb));
            case ColorSpace::Hwb: return mul(kLinSrgbToXyz, map3(hwbToSrgb(c), linSrgb));
        }
        return c;
    }

    Vec3 fromXyzD65(ColorSpace space, const Vec3& xyz){
        switch (space){
            case ColorSpace::Srgb: return map3(mul(kXyzToLinSrgb, xyz), gamSrgb);
            case ColorSpace::SrgbLinear: return mul(kXyzToLinSrgb, xyz);
            case ColorSpace::DisplayP3: return map3(mul(kXyzToLinP3, xyz), gamSrgb);
            case ColorSpace::A98Rgb: return map3(mul(kXyzToLinA98, xyz), gamA98);
            case ColorSpace::ProphotoRgb: return map3(mul(kXyzD50ToLinProphoto, mul(kD65ToD50, xyz)), gamProphoto);
            case ColorSpace::Rec2020: return map3(mul(kXyzToLin2020, xyz), gam2020);
            case ColorSpace::XyzD50: return mul(kD65ToD50, xyz);
            case ColorSpace::XyzD65: return xyz;
            case ColorSpace::Lab: return xyzD50ToLab(mul(kD65ToD50, xyz));
            case ColorSpace::Lch: return rectToPolar(xyzD50ToLab(mul(kD65ToD50, xyz)));
            case ColorSpace::Oklab: return xyzToOklab(xyz);
            case ColorSpace::Oklch: return rectToPolar(xyzToOklab(xyz));
            case ColorSpace::Hsl: return srgbToHsl(map3(mul(kXyzToLinSrgb, xyz), gamSrgb));
            case ColorSpace::Hwb: return srgbToHwb(map3(mul(kXyzToLinSrgb, xyz), gamSrgb));
        }
        return xyz;
    }

    bool isPolar(ColorSpace space){
        return space == ColorSpace::Hsl || space == ColorSpace::Hwb || space == ColorSpace::Lch || space == ColorSpace::Oklch;
    }

    int hueIndex(ColorSpace space){
        if (space == ColorSpace::Hsl || space == ColorSpace::Hwb)
            return 0;
        if (space == ColorSpace::Lch || space == ColorSpace::Oklch)
            return 2;
        return -1;
    }

    // ---- parsing

    bool isWs(const ComponentValue& v){
        return v.isWhitespace() || v.isToken(TokenType::Comment);
    }

    // channel reference ranges for percentages (100% = ...)
    struct ChannelSpec {
        double percent = 1.0;       // value of 100%
        bool hue = false;
    };

    // a component (number, percentage, angle, none, keyword, math function) as a channel value
    struct Channel {
        bool none = false;
        bool percent = false;       // written as a percentage (legacy rgb/hsl rules)
        bool number = false;        // written as a number
        double value = 0;           // percentages already scaled by spec.percent
    };

    class Parser {
    public:
        Parser(ColorParseInfo& info, const ColorContext& context): info(info), context(context) {}

        std::optional<CssColor> parseValue(const ComponentValue& value, int depth = 0){
            if (depth > 16){
                info.error = "colors nested too deeply";
                return std::nullopt;
            }
            if (value.kind == ComponentValue::Kind::Token){
                const Token& token = value.token;
                if (token.type == TokenType::Hash)
                    return parseHex(token.value);
                if (token.type == TokenType::Ident){
                    if (equalsNoCase(token.value, "currentcolor")){
                        if (depth > 0 && context.currentColor.size() == 9)
                            return parseHex(context.currentColor.substr(1));
                        info.currentColor = true;
                        return std::nullopt;
                    }
                    if (const NamedColor* named = findNamedColor(token.value))
                        return fromRgba8(named->rgba);
                    if (const NamedColor* system = findSystemColor(token.value)){
                        info.system = true;
                        return fromRgba8(system->rgba);
                    }
                    info.error = "'" + token.value + "' is not a color";
                    return std::nullopt;
                }
                return std::nullopt;
            }
            if (value.kind != ComponentValue::Kind::Function)
                return std::nullopt;
            const std::string name = lowerAscii(value.name());
            if (name == "var"){
                ValueList replacement;
                if (!context.resolveVar || !context.resolveVar(value, replacement)){
                    info.dynamic = true;
                    info.dynamicVar = true;
                    info.error = "var() without a static value";
                    return std::nullopt;
                }
                const ComponentValue* single = nullptr;
                for (const ComponentValue& v : replacement){
                    if (isWs(v))
                        continue;
                    if (single){
                        info.error = "var() is not a single color";
                        return std::nullopt;
                    }
                    single = &v;
                }
                if (!single)
                    return std::nullopt;
                return parseValue(*single, depth + 1);
            }

            ValueList args;
            args.reserve(value.children.size());
            if (!substituteVars(value.children, args, 0))
                return std::nullopt;

            if (name == "rgb" || name == "rgba")
                return parseRgb(std::move(args), depth);
            if (name == "hsl" || name == "hsla")
                return parseHslHwb(std::move(args), ColorSpace::Hsl, depth);
            if (name == "hwb")
                return parseHslHwb(std::move(args), ColorSpace::Hwb, depth);
            if (name == "lab")
                return parseLabLike(std::move(args), ColorSpace::Lab, depth);
            if (name == "lch")
                return parseLabLike(std::move(args), ColorSpace::Lch, depth);
            if (name == "oklab")
                return parseLabLike(std::move(args), ColorSpace::Oklab, depth);
            if (name == "oklch")
                return parseLabLike(std::move(args), ColorSpace::Oklch, depth);
            if (name == "color")
                return parseColorFunction(std::move(args), depth);
            if (name == "color-mix")
                return parseColorMix(args, depth);
            if (name == "light-dark")
                return parseLightDark(args, depth);
            info.error = "'" + value.name() + "()' is not a color function";
            return std::nullopt;
        }

    private:
        static CssColor fromRgba8(uint32_t rgba){
            CssColor color;
            color.space = ColorSpace::Srgb;
            color.legacy = true;
            color.c[0] = ((rgba >> 24) & 0xFF) / 255.0;
            color.c[1] = ((rgba >> 16) & 0xFF) / 255.0;
            color.c[2] = ((rgba >> 8) & 0xFF) / 255.0;
            color.alpha = (rgba & 0xFF) / 255.0;
            return color;
        }

        std::optional<CssColor> parseHex(const std::string& digits){
            auto hexValue = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (char c : digits){
                if (hexValue(c) < 0){
                    info.error = "'#" + digits + "' is not a hex color";
                    return std::nullopt;
                }
            }
            uint32_t r = 0, g = 0, b = 0, a = 255;
            if (digits.size() == 3 || digits.size() == 4){
                r = hexValue(digits[0]) * 17;
                g = hexValue(digits[1]) * 17;
                b = hexValue(digits[2]) * 17;
                if (digits.size() == 4)
                    a = hexValue(digits[3]) * 17;
            }else if (digits.size() == 6 || digits.size() == 8){
                r = hexValue(digits[0]) * 16 + hexValue(digits[1]);
                g = hexValue(digits[2]) * 16 + hexValue(digits[3]);
                b = hexValue(digits[4]) * 16 + hexValue(digits[5]);
                if (digits.size() == 8)
                    a = hexValue(digits[6]) * 16 + hexValue(digits[7]);
            }else{
                info.error = "'#" + digits + "' is not a hex color (3, 4, 6 or 8 digits)";
                return std::nullopt;
            }
            return fromRgba8((r << 24) | (g << 16) | (b << 8) | a);
        }

        // top-level var() replaced by their static values (whitespace kept)
        bool substituteVars(const ValueList& in, ValueList& out, int depth){
            for (const ComponentValue& v : in){
                if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var")){
                    ValueList replacement;
                    if (depth > 8 || !context.resolveVar || !context.resolveVar(v, replacement)){
                        info.dynamic = true;
                        info.dynamicVar = true;
                        info.error = "var() without a static value";
                        return false;
                    }
                    if (!substituteVars(replacement, out, depth + 1))
                        return false;
                    continue;
                }
                out.push_back(v);
            }
            return true;
        }

        // splits modern syntax "a b c / d" (no commas); false when the shape is wrong
        bool splitModern(const ValueList& args, ValueList& channels,
                         std::optional<ComponentValue>& alpha, bool& sawSlash){
            sawSlash = false;
            for (const ComponentValue& v : args){
                if (isWs(v))
                    continue;
                if (v.isToken(TokenType::Comma))
                    return false;
                if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                    if (sawSlash)
                        return false;
                    sawSlash = true;
                    continue;
                }
                if (sawSlash){
                    if (alpha)
                        return false;
                    alpha = v;
                }else{
                    channels.push_back(v);
                }
            }
            return !sawSlash || alpha.has_value();
        }

        // comma-separated legacy syntax: groups of exactly one component
        bool splitLegacy(const ValueList& args, ValueList& items){
            std::optional<ComponentValue> current;
            bool expectItem = true;
            for (const ComponentValue& v : args){
                if (isWs(v))
                    continue;
                if (v.isToken(TokenType::Comma)){
                    if (expectItem)
                        return false;
                    items.push_back(*current);
                    current.reset();
                    expectItem = true;
                    continue;
                }
                if (!expectItem)
                    return false;
                current = v;
                expectItem = false;
            }
            if (expectItem)
                return false;
            items.push_back(*current);
            return true;
        }

        static bool hasComma(const ValueList& args){
            for (const ComponentValue& v : args){
                if (v.isToken(TokenType::Comma))
                    return true;
            }
            return false;
        }

        // one channel; keywords: relative-color channel names
        std::optional<Channel> channel(const ComponentValue& v, const ChannelSpec& spec, bool allowNone,
                                       const std::function<std::optional<double>(std::string_view)>& keywords){
            Channel out;
            if (v.isToken(TokenType::Number)){
                out.number = true;
                out.value = v.token.number;
                return out;
            }
            if (v.isToken(TokenType::Percentage)){
                if (spec.hue)
                    return std::nullopt;
                out.percent = true;
                out.value = v.token.number / 100.0 * spec.percent;
                return out;
            }
            if (v.isToken(TokenType::Dimension)){
                if (!spec.hue || unitCategory(v.token.unit) != UnitCategory::Angle)
                    return std::nullopt;
                out.number = true;
                out.value = v.token.number * canonicalFactor(v.token.unit).value_or(1.0);
                return out;
            }
            if (v.isToken(TokenType::Ident)){
                if (allowNone && equalsNoCase(v.token.value, "none")){
                    out.none = true;
                    return out;
                }
                if (keywords){
                    if (std::optional<double> k = keywords(v.token.value)){
                        out.number = true;
                        out.value = *k;
                        return out;
                    }
                }
                return std::nullopt;
            }
            if (v.kind == ComponentValue::Kind::Function && StaticCalc::isMathFunction(v.name())){
                CalcContext calc;
                calc.identifier = keywords;
                calc.resolveVar = context.resolveVar;
                std::optional<CalcValue> result = StaticCalc::evaluate(v, calc);
                if (!result){
                    if (calc.varUnresolved)
                        info.dynamic = info.dynamicVar = true;
                    return std::nullopt;
                }
                if (result->isNumber()){
                    out.number = true;
                    out.value = result->numberValue();
                    return out;
                }
                if (result->single() && result->terms.front().first == "%" && !spec.hue){
                    out.percent = true;
                    out.value = result->terms.front().second / 100.0 * spec.percent;
                    return out;
                }
                if (spec.hue && result->single() && result->terms.front().first == "deg"){
                    out.number = true;
                    out.value = result->terms.front().second;
                    return out;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::optional<double> alphaValue(const ComponentValue& v, bool allowNone, bool& none,
                                         const std::function<std::optional<double>(std::string_view)>& keywords){
            ChannelSpec spec;
            spec.percent = 1.0;
            std::optional<Channel> c = channel(v, spec, allowNone, keywords);
            if (!c)
                return std::nullopt;
            none = c->none;
            return std::max(0.0, std::min(1.0, c->value));
        }

        // relative color: the origin converted to `space`, its channels as keywords
        struct Origin {
            bool active = false;
            CssColor color;
            std::vector<std::pair<std::string, double>> keywords;
        };

        bool readOrigin(ValueList& args, ColorSpace space, bool legacyRgb, int depth, Origin& origin,
                        std::optional<ColorSpace>* colorFunctionSpace = nullptr){
            size_t i = 0;
            while (i < args.size() && isWs(args[i]))
                i++;
            if (i >= args.size() || !args[i].isToken(TokenType::Ident) || !equalsNoCase(args[i].token.value, "from"))
                return true;
            i++;
            while (i < args.size() && isWs(args[i]))
                i++;
            if (i >= args.size())
                return false;
            info.relative = true;
            ColorParseInfo inner;
            Parser parser(inner, context);
            std::optional<CssColor> base = parser.parseValue(args[i], depth + 1);
            if (!base){
                if (inner.currentColor || inner.dynamic){
                    info.dynamic = true;
                    info.dynamicVar = info.dynamicVar || inner.dynamicVar;
                    info.error = "relative color from a dynamic origin";
                }else{
                    info.error = inner.error;
                }
                return false;
            }
            i++;
            if (colorFunctionSpace){
                while (i < args.size() && isWs(args[i]))
                    i++;
                if (i >= args.size() || !args[i].isToken(TokenType::Ident))
                    return false;
                std::optional<ColorSpace> s = ColorValue::findSpace(args[i].token.value);
                if (!s || isPolar(*s) || *s == ColorSpace::Lab || *s == ColorSpace::Oklab)
                    return false;
                *colorFunctionSpace = s;
                space = *s;
                i++;
            }
            args.erase(args.begin(), args.begin() + (std::ptrdiff_t)i);

            origin.active = true;
            origin.color = ColorValue::convert(*base, space);
            const CssColor& c = origin.color;
            switch (space){
                case ColorSpace::Srgb:
                    if (legacyRgb)
                        origin.keywords = {{"r", c.c[0] * 255.0}, {"g", c.c[1] * 255.0}, {"b", c.c[2] * 255.0}};
                    else
                        origin.keywords = {{"r", c.c[0]}, {"g", c.c[1]}, {"b", c.c[2]}};
                    break;
                case ColorSpace::Hsl: origin.keywords = {{"h", c.c[0]}, {"s", c.c[1]}, {"l", c.c[2]}}; break;
                case ColorSpace::Hwb: origin.keywords = {{"h", c.c[0]}, {"w", c.c[1]}, {"b", c.c[2]}}; break;
                case ColorSpace::Lab:
                case ColorSpace::Oklab: origin.keywords = {{"l", c.c[0]}, {"a", c.c[1]}, {"b", c.c[2]}}; break;
                case ColorSpace::Lch:
                case ColorSpace::Oklch: origin.keywords = {{"l", c.c[0]}, {"c", c.c[1]}, {"h", c.c[2]}}; break;
                case ColorSpace::XyzD50:
                case ColorSpace::XyzD65: origin.keywords = {{"x", c.c[0]}, {"y", c.c[1]}, {"z", c.c[2]}}; break;
                default: origin.keywords = {{"r", c.c[0]}, {"g", c.c[1]}, {"b", c.c[2]}}; break;
            }
            origin.keywords.emplace_back("alpha", c.alpha);
            return true;
        }

        static std::function<std::optional<double>(std::string_view)> keywordsOf(const Origin& origin){
            if (!origin.active)
                return {};
            const auto keywords = origin.keywords;
            return [keywords](std::string_view name) -> std::optional<double> {
                for (const auto& k : keywords){
                    if (equalsNoCase(name, k.first))
                        return k.second;
                }
                return std::nullopt;
            };
        }

        std::optional<CssColor> parseRgb(ValueList args, int depth){
            Origin origin;
            if (!readOrigin(args, ColorSpace::Srgb, true, depth, origin))
                return fail("invalid relative rgb()");
            const auto keywords = keywordsOf(origin);
            CssColor color;
            color.space = ColorSpace::Srgb;
            color.legacy = true;
            ChannelSpec spec;
            spec.percent = 255.0;

            if (!origin.active && hasComma(args)){
                ValueList items;
                if (!splitLegacy(args, items) || (items.size() != 3 && items.size() != 4))
                    return fail("rgb() takes three channels and an optional alpha");
                bool percent = false;
                for (int i = 0; i < 3; i++){
                    std::optional<Channel> c = channel(items[i], spec, false, {});
                    if (!c)
                        return fail("invalid rgb() channel");
                    if (i == 0)
                        percent = c->percent;
                    else if (c->percent != percent)
                        return fail("rgb() mixes numbers and percentages");
                    color.c[i] = clamp01(c->value / 255.0);
                }
                if (items.size() == 4){
                    bool none = false;
                    std::optional<double> a = alphaValue(items[3], false, none, {});
                    if (!a)
                        return fail("invalid rgb() alpha");
                    color.alpha = *a;
                }
                return color;
            }

            ValueList channels;
            std::optional<ComponentValue> alpha;
            bool slash = false;
            if (!splitModern(args, channels, alpha, slash) || channels.size() != 3)
                return fail("rgb() takes three channels and an optional / alpha");
            for (int i = 0; i < 3; i++){
                std::optional<Channel> c = channel(channels[i], spec, true, keywords);
                if (!c)
                    return fail("invalid rgb() channel");
                color.none[i] = c->none;
                color.c[i] = c->none ? 0.0 : clamp01(c->value / 255.0);
            }
            color.alpha = origin.active ? origin.color.alpha : 1.0;
            if (alpha){
                bool none = false;
                std::optional<double> a = alphaValue(*alpha, true, none, keywords);
                if (!a)
                    return fail("invalid rgb() alpha");
                color.none[3] = none;
                color.alpha = none ? 0.0 : *a;
            }
            return color;
        }

        std::optional<CssColor> parseHslHwb(ValueList args, ColorSpace space, int depth){
            Origin origin;
            if (!readOrigin(args, space, false, depth, origin))
                return fail("invalid relative color");
            const auto keywords = keywordsOf(origin);
            const bool hsl = space == ColorSpace::Hsl;
            ChannelSpec hueSpec;
            hueSpec.hue = true;
            ChannelSpec pctSpec;
            pctSpec.percent = 100.0;
            Vec3 values = {0, 0, 0};
            CssColor color;
            color.legacy = true;
            color.space = ColorSpace::Srgb;
            double alphaResult = origin.active ? origin.color.alpha : 1.0;

            if (hsl && !origin.active && hasComma(args)){
                ValueList items;
                if (!splitLegacy(args, items) || (items.size() != 3 && items.size() != 4))
                    return fail("hsl() takes three channels and an optional alpha");
                std::optional<Channel> h = channel(items[0], hueSpec, false, {});
                std::optional<Channel> s = channel(items[1], pctSpec, false, {});
                std::optional<Channel> l = channel(items[2], pctSpec, false, {});
                if (!h || !s || !l || !s->percent || !l->percent)
                    return fail("legacy hsl() takes a hue and two percentages");
                values = {h->value, s->value, l->value};
                if (items.size() == 4){
                    bool none = false;
                    std::optional<double> a = alphaValue(items[3], false, none, {});
                    if (!a)
                        return fail("invalid hsl() alpha");
                    alphaResult = *a;
                }
            }else{
                ValueList channels;
                std::optional<ComponentValue> alpha;
                bool slash = false;
                if (!splitModern(args, channels, alpha, slash) || channels.size() != 3)
                    return fail(std::string(hsl ? "hsl" : "hwb") + "() takes three channels and an optional / alpha");
                std::optional<Channel> h = channel(channels[0], hueSpec, true, keywords);
                std::optional<Channel> s = channel(channels[1], pctSpec, true, keywords);
                std::optional<Channel> l = channel(channels[2], pctSpec, true, keywords);
                if (!h || !s || !l)
                    return fail("invalid channel");
                values = {h->none ? 0.0 : h->value, s->none ? 0.0 : s->value, l->none ? 0.0 : l->value};
                if (alpha){
                    bool none = false;
                    std::optional<double> a = alphaValue(*alpha, true, none, keywords);
                    if (!a)
                        return fail("invalid alpha");
                    alphaResult = none ? 0.0 : *a;
                }
            }
            if (hsl){
                values[1] = std::max(0.0, values[1]);
                values[2] = std::max(0.0, std::min(100.0, values[2]));
            }else{
                values[1] = std::max(0.0, std::min(100.0, values[1]));
                values[2] = std::max(0.0, std::min(100.0, values[2]));
            }
            const Vec3 rgb = hsl ? hslToSrgb(values) : hwbToSrgb(values);
            for (int i = 0; i < 3; i++)
                color.c[i] = clamp01(rgb[i]);
            color.alpha = alphaResult;
            return color;
        }

        std::optional<CssColor> parseLabLike(ValueList args, ColorSpace space, int depth){
            Origin origin;
            if (!readOrigin(args, space, false, depth, origin))
                return fail("invalid relative color");
            const auto keywords = keywordsOf(origin);
            ValueList channels;
            std::optional<ComponentValue> alpha;
            bool slash = false;
            if (!splitModern(args, channels, alpha, slash) || channels.size() != 3)
                return fail("takes three channels and an optional / alpha");
            const bool ok = space == ColorSpace::Oklab || space == ColorSpace::Oklch;
            const bool polar = space == ColorSpace::Lch || space == ColorSpace::Oklch;
            ChannelSpec specs[3];
            specs[0].percent = ok ? 1.0 : 100.0;
            if (polar){
                specs[1].percent = ok ? 0.4 : 150.0;
                specs[2].hue = true;
            }else{
                specs[1].percent = ok ? 0.4 : 125.0;
                specs[2].percent = ok ? 0.4 : 125.0;
            }
            CssColor color;
            color.space = space;
            for (int i = 0; i < 3; i++){
                std::optional<Channel> c = channel(channels[i], specs[i], true, keywords);
                if (!c)
                    return fail("invalid channel");
                color.none[i] = c->none;
                color.c[i] = c->none ? 0.0 : c->value;
            }
            if (polar)
                color.c[2] = normalizeHue(color.c[2]);
            color.alpha = origin.active ? origin.color.alpha : 1.0;
            if (alpha){
                bool none = false;
                std::optional<double> a = alphaValue(*alpha, true, none, keywords);
                if (!a)
                    return fail("invalid alpha");
                color.none[3] = none;
                color.alpha = none ? 0.0 : *a;
            }
            return color;
        }

        std::optional<CssColor> parseColorFunction(ValueList args, int depth){
            Origin origin;
            std::optional<ColorSpace> relativeSpace;
            if (!readOrigin(args, ColorSpace::Srgb, false, depth, origin, &relativeSpace))
                return fail("invalid relative color()");
            const auto keywords = keywordsOf(origin);
            std::optional<ColorSpace> space = relativeSpace;
            ValueList rest;
            for (const ComponentValue& v : args){
                if (!space && !isWs(v)){
                    if (!v.isToken(TokenType::Ident))
                        return fail("color() needs a color space");
                    space = ColorValue::findSpace(v.token.value);
                    if (!space || isPolar(*space) || *space == ColorSpace::Lab || *space == ColorSpace::Oklab)
                        return fail("unsupported color() space '" + v.token.value + "'");
                    continue;
                }
                rest.push_back(v);
            }
            if (!space)
                return fail("color() needs a color space");
            ValueList channels;
            std::optional<ComponentValue> alpha;
            bool slash = false;
            if (!splitModern(rest, channels, alpha, slash) || channels.size() != 3)
                return fail("color() takes three channels and an optional / alpha");
            ChannelSpec spec;
            spec.percent = 1.0;
            CssColor color;
            color.space = *space;
            for (int i = 0; i < 3; i++){
                std::optional<Channel> c = channel(channels[i], spec, true, keywords);
                if (!c)
                    return fail("invalid color() channel");
                color.none[i] = c->none;
                color.c[i] = c->none ? 0.0 : c->value;
            }
            color.alpha = origin.active ? origin.color.alpha : 1.0;
            if (alpha){
                bool none = false;
                std::optional<double> a = alphaValue(*alpha, true, none, keywords);
                if (!a)
                    return fail("invalid color() alpha");
                color.none[3] = none;
                color.alpha = none ? 0.0 : *a;
            }
            return color;
        }

        std::optional<CssColor> parseLightDark(const ValueList& args, int depth){
            std::vector<ValueList> parts = splitAtCommas(args);
            if (parts.size() != 2)
                return fail("light-dark() takes two colors");
            std::vector<const ComponentValue*> values;
            for (const ValueList& part : parts){
                const ComponentValue* single = nullptr;
                for (const ComponentValue& v : part){
                    if (isWs(v))
                        continue;
                    if (single)
                        return fail("light-dark() takes two colors");
                    single = &v;
                }
                if (!single)
                    return fail("light-dark() takes two colors");
                values.push_back(single);
            }
            info.lightDark = true;
            // the dark color must be valid too
            ColorParseInfo darkInfo;
            Parser dark(darkInfo, context);
            if (!dark.parseValue(*values[1], depth + 1) && !darkInfo.currentColor){
                if (darkInfo.dynamic){
                    info.dynamic = true;
                    info.dynamicVar = info.dynamicVar || darkInfo.dynamicVar;
                }
                return fail("invalid dark color in light-dark()");
            }
            return parseValue(*values[0], depth + 1);
        }

        std::optional<CssColor> parseColorMix(const ValueList& args, int depth){
            info.colorMix = true;
            std::vector<ValueList> parts = splitAtCommas(args);
            if (parts.size() != 3)
                return fail("color-mix() takes an interpolation method and two colors");
            // "in <space> [<hue> hue]"
            std::vector<const ComponentValue*> method;
            for (const ComponentValue& v : parts[0]){
                if (!isWs(v))
                    method.push_back(&v);
            }
            if (method.size() < 2 || !method[0]->isToken(TokenType::Ident) || !equalsNoCase(method[0]->token.value, "in") ||
                !method[1]->isToken(TokenType::Ident))
                return fail("color-mix() needs 'in <color space>'");
            std::optional<ColorSpace> space = ColorValue::findSpace(method[1]->token.value);
            if (!space)
                return fail("unknown color-mix() space '" + method[1]->token.value + "'");
            std::string hueMethod = "shorter";
            if (method.size() == 4){
                if (!isPolar(*space) || !method[2]->isToken(TokenType::Ident) || !method[3]->isToken(TokenType::Ident) ||
                    !equalsNoCase(method[3]->token.value, "hue"))
                    return fail("invalid color-mix() hue interpolation");
                hueMethod = lowerAscii(method[2]->token.value);
                if (hueMethod != "shorter" && hueMethod != "longer" && hueMethod != "increasing" && hueMethod != "decreasing")
                    return fail("invalid color-mix() hue interpolation");
            }else if (method.size() != 2){
                return fail("invalid color-mix() interpolation method");
            }

            CssColor colors[2];
            std::optional<double> percents[2];
            bool currentOperand = false;   // currentColor (or a color-mix() of it) as an operand
            for (int i = 0; i < 2; i++){
                const ComponentValue* colorValue = nullptr;
                for (const ComponentValue& v : parts[i + 1]){
                    if (isWs(v))
                        continue;
                    if (v.isToken(TokenType::Percentage) ||
                        (v.kind == ComponentValue::Kind::Function && StaticCalc::isMathFunction(v.name()))){
                        if (percents[i])
                            return fail("color-mix() takes one percentage per color");
                        if (v.isToken(TokenType::Percentage)){
                            percents[i] = v.token.number;
                        }else{
                            CalcContext calc;
                            calc.resolveVar = context.resolveVar;
                            std::optional<CalcValue> r = StaticCalc::evaluate(v, calc);
                            if (!r || !r->single() || r->terms.front().first != "%")
                                return fail("invalid color-mix() percentage");
                            percents[i] = r->terms.front().second;
                        }
                        if (*percents[i] < 0 || *percents[i] > 100)
                            return fail("color-mix() percentages are 0% to 100%");
                        continue;
                    }
                    if (colorValue)
                        return fail("color-mix() takes one color per argument");
                    colorValue = &v;
                }
                if (!colorValue)
                    return fail("color-mix() is missing a color");
                ColorParseInfo inner;
                Parser parser(inner, context);
                std::optional<CssColor> c = parser.parseValue(*colorValue, depth + 1);
                if (!c && (inner.currentColor || inner.currentColorMix) && !inner.dynamicVar){
                    // the other operand and the percentages are still checked
                    currentOperand = true;
                    colors[i] = CssColor{};
                    continue;
                }
                if (!c){
                    if (inner.currentColor || inner.dynamic){
                        info.dynamic = true;
                        info.dynamicVar = info.dynamicVar || inner.dynamicVar;
                        info.error = "color-mix() with a dynamic color";
                    }else{
                        info.error = inner.error;
                    }
                    return std::nullopt;
                }
                colors[i] = *c;
            }
            double p1 = 0, p2 = 0;
            if (!percents[0] && !percents[1]){
                p1 = p2 = 50;
            }else if (!percents[1]){
                p1 = *percents[0];
                p2 = 100 - p1;
            }else if (!percents[0]){
                p2 = *percents[1];
                p1 = 100 - p2;
            }else{
                p1 = *percents[0];
                p2 = *percents[1];
            }
            const double sum = p1 + p2;
            if (sum <= 0)
                return fail("color-mix() percentages add up to zero");
            if (currentOperand){
                // valid, with a value only known for an element (CSS Color 5: resolved as currentColor is)
                info.dynamic = true;
                info.currentColorMix = true;
                info.error = "color-mix() with currentcolor";
                return std::nullopt;
            }
            double alphaMultiplier = 1.0;
            if (sum < 100)
                alphaMultiplier = sum / 100.0;
            const double t = p2 / sum;

            CssColor a = ColorValue::convert(colors[0], *space);
            CssColor b = ColorValue::convert(colors[1], *space);
            // missing (none) components take the other color's
            for (int i = 0; i < 4; i++){
                double& av = i < 3 ? a.c[i] : a.alpha;
                double& bv = i < 3 ? b.c[i] : b.alpha;
                if (a.none[i] && !b.none[i])
                    av = bv;
                else if (b.none[i] && !a.none[i])
                    bv = av;
            }
            const int hue = hueIndex(*space);
            if (hue >= 0){
                double h1 = a.c[hue], h2 = b.c[hue];
                double d = h2 - h1;
                if (hueMethod == "shorter"){
                    if (d > 180) h1 += 360;
                    else if (d < -180) h2 += 360;
                }else if (hueMethod == "longer"){
                    if (d > 0 && d < 180) h1 += 360;
                    else if (d > -180 && d <= 0) h2 += 360;
                }else if (hueMethod == "increasing"){
                    if (d < 0) h2 += 360;
                }else if (hueMethod == "decreasing"){
                    if (d > 0) h1 += 360;
                }
                a.c[hue] = h1;
                b.c[hue] = h2;
            }
            CssColor out;
            out.space = *space;
            const double alpha = a.alpha * (1 - t) + b.alpha * t;
            for (int i = 0; i < 3; i++){
                if (i == hue){
                    out.c[i] = normalizeHue(a.c[i] * (1 - t) + b.c[i] * t);
                    continue;
                }
                // premultiplied interpolation
                const double pa = a.c[i] * a.alpha;
                const double pb = b.c[i] * b.alpha;
                const double mixed = pa * (1 - t) + pb * t;
                out.c[i] = alpha > 0 ? mixed / alpha : mixed;
            }
            out.alpha = alpha * alphaMultiplier;
            return out;
        }

        static double clamp01(double v){
            return std::max(0.0, std::min(1.0, v));
        }

        std::optional<CssColor> fail(const std::string& message){
            if (info.error.empty())
                info.error = message;
            return std::nullopt;
        }

        ColorParseInfo& info;
        const ColorContext& context;
    };

}

std::optional<CssColor> ColorValue::parse(const ComponentValue& value, ColorParseInfo& info, const ColorContext& context){
    Parser parser(info, context);
    return parser.parseValue(value);
}

std::optional<CssColor> ColorValue::parse(std::string_view css, ColorParseInfo* info){
    ColorParseInfo local;
    ColorParseInfo& target = info ? *info : local;
    // (the parsed value is only read here)
    Arena arena;
    ArenaScope scope(arena);
    ValueList values = trimWhitespace(parseComponentValueList(css));
    if (values.size() != 1){
        target.error = "not one color";
        return std::nullopt;
    }
    return parse(values[0], target);
}

bool ColorValue::isColorFunction(std::string_view name){
    static constexpr std::string_view names[] = {"rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch", "color",
                                                 "color-mix", "light-dark"};
    for (std::string_view n : names){
        if (equalsNoCase(name, n))
            return true;
    }
    return false;
}

bool ColorValue::isColorSyntax(const ComponentValue& value){
    if (value.isToken(TokenType::Hash))
        return true;
    if (value.isToken(TokenType::Ident)){
        return equalsNoCase(value.token.value, "currentcolor") || findNamedColor(value.token.value) ||
               findSystemColor(value.token.value);
    }
    if (value.kind == ComponentValue::Kind::Function)
        return isColorFunction(value.name());
    return false;
}

CssColor ColorValue::convert(const CssColor& color, ColorSpace space){
    CssColor result;
    result.space = space;
    result.alpha = color.alpha;
    result.none[3] = color.none[3];
    if (color.space == space){
        result = color;
        result.legacy = false;
        return result;
    }
    Vec3 c = {color.c[0], color.c[1], color.c[2]};
    // legacy sRGB and hsl/hwb share the sRGB gamma space: keep the conversion exact
    Vec3 out;
    if ((color.space == ColorSpace::Srgb || color.space == ColorSpace::Hsl || color.space == ColorSpace::Hwb) &&
        (space == ColorSpace::Srgb || space == ColorSpace::Hsl || space == ColorSpace::Hwb)){
        Vec3 rgb = color.space == ColorSpace::Srgb ? c : (color.space == ColorSpace::Hsl ? hslToSrgb(c) : hwbToSrgb(c));
        out = space == ColorSpace::Srgb ? rgb : (space == ColorSpace::Hsl ? srgbToHsl(rgb) : srgbToHwb(rgb));
    }else{
        out = fromXyzD65(space, toXyzD65(color.space, c));
    }
    for (int i = 0; i < 3; i++)
        result.c[i] = out[i];
    // an achromatic color has no hue
    const int hue = hueIndex(space);
    if (hue >= 0){
        const double chroma = (space == ColorSpace::Hsl) ? result.c[1] : (space == ColorSpace::Hwb ? 100.0 - result.c[1] - result.c[2] : result.c[1]);
        if (std::fabs(chroma) < 1e-6){
            result.c[hue] = 0.0;
            result.none[hue] = true;
        }
    }
    return result;
}

void ColorValue::toSrgb(const CssColor& color, double out[4]){
    const CssColor srgb = convert(color, ColorSpace::Srgb);
    out[0] = srgb.c[0];
    out[1] = srgb.c[1];
    out[2] = srgb.c[2];
    out[3] = color.alpha;
}

uint32_t ColorValue::toRgba8(const CssColor& color, bool* clipped){
    double rgba[4];
    toSrgb(color, rgba);
    bool outside = false;
    uint32_t result = 0;
    for (int i = 0; i < 4; i++){
        double v = rgba[i];
        if (!std::isfinite(v))
            v = 0;
        if (i < 3 && (v < -0.5 / 255.0 || v > 1.0 + 0.5 / 255.0))
            outside = true;
        v = std::max(0.0, std::min(1.0, v));
        const uint32_t byte = (uint32_t)std::floor(v * 255.0 + 0.5);
        result = (result << 8) | (byte > 255 ? 255 : byte);
    }
    if (clipped)
        *clipped = outside;
    return result;
}

std::string ColorValue::hex(uint32_t rgba){
    // "#%08x"
    static const char digits[] = "0123456789abcdef";
    char buffer[9];
    buffer[0] = '#';
    for (int i = 0; i < 8; i++)
        buffer[1 + i] = digits[(rgba >> (28 - 4 * i)) & 0xF];
    return std::string(buffer, 9);
}

std::string ColorValue::toHex(const ComponentValue& value, ColorParseInfo& info, const ColorContext& context, bool* clipped){
    std::optional<CssColor> color = parse(value, info, context);
    if (!color)
        return std::string();
    return hex(toRgba8(*color, clipped));
}

const char* ColorValue::spaceName(ColorSpace space){
    switch (space){
        case ColorSpace::Srgb: return "srgb";
        case ColorSpace::SrgbLinear: return "srgb-linear";
        case ColorSpace::DisplayP3: return "display-p3";
        case ColorSpace::A98Rgb: return "a98-rgb";
        case ColorSpace::ProphotoRgb: return "prophoto-rgb";
        case ColorSpace::Rec2020: return "rec2020";
        case ColorSpace::XyzD50: return "xyz-d50";
        case ColorSpace::XyzD65: return "xyz-d65";
        case ColorSpace::Lab: return "lab";
        case ColorSpace::Lch: return "lch";
        case ColorSpace::Oklab: return "oklab";
        case ColorSpace::Oklch: return "oklch";
        case ColorSpace::Hsl: return "hsl";
        case ColorSpace::Hwb: return "hwb";
    }
    return "srgb";
}

std::optional<ColorSpace> ColorValue::findSpace(std::string_view name){
    const std::string n = lowerAscii(name);
    if (n == "srgb") return ColorSpace::Srgb;
    if (n == "srgb-linear") return ColorSpace::SrgbLinear;
    if (n == "display-p3") return ColorSpace::DisplayP3;
    if (n == "a98-rgb") return ColorSpace::A98Rgb;
    if (n == "prophoto-rgb") return ColorSpace::ProphotoRgb;
    if (n == "rec2020") return ColorSpace::Rec2020;
    if (n == "xyz-d50") return ColorSpace::XyzD50;
    if (n == "xyz-d65" || n == "xyz") return ColorSpace::XyzD65;
    if (n == "lab") return ColorSpace::Lab;
    if (n == "lch") return ColorSpace::Lch;
    if (n == "oklab") return ColorSpace::Oklab;
    if (n == "oklch") return ColorSpace::Oklch;
    if (n == "hsl") return ColorSpace::Hsl;
    if (n == "hwb") return ColorSpace::Hwb;
    return std::nullopt;
}
