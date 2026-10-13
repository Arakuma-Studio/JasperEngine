#include "ColorSpaces.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <cmath>

namespace Rml {
namespace ColorSpaces {

namespace {

	using Vec3 = double[3];
	using Mat3 = double[3][3];

	void Multiply(const Mat3& m, const Vec3& in, Vec3& out)
	{
		const double x = in[0], y = in[1], z = in[2];
		out[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z;
		out[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z;
		out[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z;
	}

	constexpr double Pi = 3.14159265358979323846;

	// CSS Color 4, section 18 (sample code), matrices as published there.
	const Mat3 linear_srgb_to_xyz = {
		{0.41239079926595934, 0.357584339383878, 0.1804807884018343},
		{0.21263900587151027, 0.715168678767756, 0.07219231536073371},
		{0.01933081871559182, 0.11919477979462598, 0.9505321522496607},
	};
	const Mat3 xyz_to_linear_srgb = {
		{3.2409699419045226, -1.537383177570094, -0.4986107602930034},
		{-0.9692436362808796, 1.8759675015077202, 0.04155505740717559},
		{0.05563007969699366, -0.20397695888897652, 1.0569715142428786},
	};
	const Mat3 d65_to_d50 = {
		{1.0479298208405488, 0.022946793341019088, -0.05019222954313557},
		{0.029627815688159344, 0.990434484573249, -0.01707382502938514},
		{-0.009243058152591178, 0.015055144896577895, 0.7518742899580008},
	};
	const Mat3 d50_to_d65 = {
		{0.9554734527042182, -0.023098536874261423, 0.0632593086610217},
		{-0.028369706963208136, 1.0099954580058226, 0.021041398966943008},
		{0.012314001688319899, -0.020507696433477912, 1.3303659366080753},
	};
	const Mat3 xyz_to_lms = {
		{0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
		{0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
		{0.0481771893596242, 0.2642395317527308, 0.6335478284694309},
	};
	const Mat3 lms_to_oklab = {
		{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
		{1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
		{0.0259040424655478, 0.7827717124575296, -0.8086757549230774},
	};
	const Mat3 oklab_to_lms = {
		{1.0000000000000000, 0.3963377773761749, 0.2158037573099136},
		{1.0000000000000000, -0.1055613458156586, -0.0638541728258133},
		{1.0000000000000000, -0.0894841775298119, -1.2914855480194092},
	};
	const Mat3 lms_to_xyz = {
		{1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
		{-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
		{-0.0763729366746601, -0.4214933324022432, 1.5869240198367816},
	};
	const double d50_white[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};

	double ToLinear(double c)
	{
		const double a = std::abs(c);
		if (a <= 0.04045)
			return c / 12.92;
		return (c < 0 ? -1.0 : 1.0) * std::pow((a + 0.055) / 1.055, 2.4);
	}

	double FromLinear(double c)
	{
		const double a = std::abs(c);
		if (a <= 0.0031308)
			return c * 12.92;
		return (c < 0 ? -1.0 : 1.0) * (1.055 * std::pow(a, 1.0 / 2.4) - 0.055);
	}

	double NormalizeHue(double hue)
	{
		hue = std::fmod(hue, 360.0);
		if (hue < 0)
			hue += 360.0;
		return hue;
	}

	void HslToSrgb(const Vec3& hsl, Vec3& rgb)
	{
		const double h = NormalizeHue(hsl[0]);
		const double s = hsl[1], l = hsl[2];
		for (int i = 0; i < 3; i++)
		{
			const double n = (i == 0 ? 0.0 : i == 1 ? 8.0 : 4.0);
			const double k = std::fmod(n + h / 30.0, 12.0);
			const double a = s * std::min(l, 1.0 - l);
			rgb[i] = l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
		}
	}

	void SrgbToHsl(const Vec3& rgb, Vec3& hsl)
	{
		const double r = rgb[0], g = rgb[1], b = rgb[2];
		const double max = std::max({r, g, b}), min = std::min({r, g, b});
		double h = 0, s = 0;
		const double l = (min + max) / 2.0;
		const double d = max - min;
		if (d != 0)
		{
			s = (l == 0 || l == 1) ? 0 : (max - l) / std::min(l, 1.0 - l);
			if (max == r)
				h = (g - b) / d + (g < b ? 6 : 0);
			else if (max == g)
				h = (b - r) / d + 2;
			else
				h = (r - g) / d + 4;
			h *= 60;
		}
		if (s < 0)
		{
			h += 180;
			s = std::abs(s);
		}
		hsl[0] = NormalizeHue(h);
		hsl[1] = s;
		hsl[2] = l;
	}

	void HwbToSrgb(const Vec3& hwb, Vec3& rgb)
	{
		double w = hwb[1], b = hwb[2];
		if (w + b >= 1.0)
		{
			const double gray = w / (w + b);
			rgb[0] = rgb[1] = rgb[2] = gray;
			return;
		}
		const Vec3 hsl = {hwb[0], 1.0, 0.5};
		HslToSrgb(hsl, rgb);
		for (int i = 0; i < 3; i++)
			rgb[i] = rgb[i] * (1.0 - w - b) + w;
	}

	void SrgbToHwb(const Vec3& rgb, Vec3& hwb)
	{
		Vec3 hsl;
		SrgbToHsl(rgb, hsl);
		hwb[0] = hsl[0];
		hwb[1] = std::min({rgb[0], rgb[1], rgb[2]});
		hwb[2] = 1.0 - std::max({rgb[0], rgb[1], rgb[2]});
	}

	void XyzD50ToLab(const Vec3& xyz, Vec3& lab)
	{
		const double epsilon = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
		double f[3];
		for (int i = 0; i < 3; i++)
		{
			const double v = xyz[i] / d50_white[i];
			f[i] = v > epsilon ? std::cbrt(v) : (kappa * v + 16.0) / 116.0;
		}
		lab[0] = 116.0 * f[1] - 16.0;
		lab[1] = 500.0 * (f[0] - f[1]);
		lab[2] = 200.0 * (f[1] - f[2]);
	}

	void LabToXyzD50(const Vec3& lab, Vec3& xyz)
	{
		const double epsilon = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
		const double f1 = (lab[0] + 16.0) / 116.0;
		const double f0 = lab[1] / 500.0 + f1;
		const double f2 = f1 - lab[2] / 200.0;
		const double x = (f0 * f0 * f0 > epsilon) ? f0 * f0 * f0 : (116.0 * f0 - 16.0) / kappa;
		const double y = (lab[0] > kappa * epsilon) ? std::pow((lab[0] + 16.0) / 116.0, 3.0) : lab[0] / kappa;
		const double z = (f2 * f2 * f2 > epsilon) ? f2 * f2 * f2 : (116.0 * f2 - 16.0) / kappa;
		xyz[0] = x * d50_white[0];
		xyz[1] = y * d50_white[1];
		xyz[2] = z * d50_white[2];
	}

	void XyzD65ToOklab(const Vec3& xyz, Vec3& oklab)
	{
		Vec3 lms;
		Multiply(xyz_to_lms, xyz, lms);
		for (double& v : lms)
			v = std::cbrt(v);
		Multiply(lms_to_oklab, lms, oklab);
	}

	void OklabToXyzD65(const Vec3& oklab, Vec3& xyz)
	{
		Vec3 lms;
		Multiply(oklab_to_lms, oklab, lms);
		for (double& v : lms)
			v = v * v * v;
		Multiply(lms_to_xyz, lms, xyz);
	}

	void RectangularToPolar(const Vec3& lab, Vec3& lch)
	{
		lch[0] = lab[0];
		lch[1] = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		lch[2] = NormalizeHue(std::atan2(lab[2], lab[1]) * 180.0 / Pi);
	}

	void PolarToRectangular(const Vec3& lch, Vec3& lab)
	{
		const double hue = lch[2] * Pi / 180.0;
		lab[0] = lch[0];
		lab[1] = lch[1] * std::cos(hue);
		lab[2] = lch[1] * std::sin(hue);
	}

	void ToXyzD65(Space space, const Vec3& in, Vec3& xyz)
	{
		Vec3 tmp = {}, tmp2 = {};
		switch (space)
		{
		case Space::SRGB:
			for (int i = 0; i < 3; i++)
				tmp[i] = ToLinear(in[i]);
			Multiply(linear_srgb_to_xyz, tmp, xyz);
			break;
		case Space::SRGBLinear: Multiply(linear_srgb_to_xyz, in, xyz); break;
		case Space::HSL:
		case Space::HWB:
			if (space == Space::HSL)
				HslToSrgb(in, tmp2);
			else
				HwbToSrgb(in, tmp2);
			for (int i = 0; i < 3; i++)
				tmp[i] = ToLinear(tmp2[i]);
			Multiply(linear_srgb_to_xyz, tmp, xyz);
			break;
		case Space::Lab:
			LabToXyzD50(in, tmp);
			Multiply(d50_to_d65, tmp, xyz);
			break;
		case Space::LCH:
			PolarToRectangular(in, tmp2);
			LabToXyzD50(tmp2, tmp);
			Multiply(d50_to_d65, tmp, xyz);
			break;
		case Space::Oklab: OklabToXyzD65(in, xyz); break;
		case Space::Oklch:
			PolarToRectangular(in, tmp);
			OklabToXyzD65(tmp, xyz);
			break;
		case Space::XYZD50: Multiply(d50_to_d65, in, xyz); break;
		case Space::XYZD65:
			for (int i = 0; i < 3; i++)
				xyz[i] = in[i];
			break;
		}
	}

	void FromXyzD65(Space space, const Vec3& xyz, Vec3& out)
	{
		Vec3 tmp = {}, tmp2 = {};
		switch (space)
		{
		case Space::SRGB:
			Multiply(xyz_to_linear_srgb, xyz, tmp);
			for (int i = 0; i < 3; i++)
				out[i] = FromLinear(tmp[i]);
			break;
		case Space::SRGBLinear: Multiply(xyz_to_linear_srgb, xyz, out); break;
		case Space::HSL:
		case Space::HWB:
			Multiply(xyz_to_linear_srgb, xyz, tmp);
			for (int i = 0; i < 3; i++)
				tmp2[i] = FromLinear(tmp[i]);
			if (space == Space::HSL)
				SrgbToHsl(tmp2, out);
			else
				SrgbToHwb(tmp2, out);
			break;
		case Space::Lab:
			Multiply(d65_to_d50, xyz, tmp);
			XyzD50ToLab(tmp, out);
			break;
		case Space::LCH:
			Multiply(d65_to_d50, xyz, tmp);
			XyzD50ToLab(tmp, tmp2);
			RectangularToPolar(tmp2, out);
			break;
		case Space::Oklab: XyzD65ToOklab(xyz, out); break;
		case Space::Oklch:
			XyzD65ToOklab(xyz, tmp);
			RectangularToPolar(tmp, out);
			break;
		case Space::XYZD50: Multiply(d65_to_d50, xyz, out); break;
		case Space::XYZD65:
			for (int i = 0; i < 3; i++)
				out[i] = xyz[i];
			break;
		}
	}

	// A hue that has no effect on the color (achromatic colors).
	bool IsHuePowerless(Space space, const double components[3])
	{
		switch (space)
		{
		case Space::HSL: return std::abs(components[1]) < 1e-5 || components[2] <= 1e-5 || components[2] >= 1.0 - 1e-5;
		case Space::HWB: return components[1] + components[2] >= 1.0 - 1e-5;
		case Space::LCH: return components[1] < 0.0015;
		case Space::Oklch: return components[1] < 0.000004;
		default: break;
		}
		return false;
	}

	double Interpolate(double a, double b, double weight_a)
	{
		return a * weight_a + b * (1.0 - weight_a);
	}

} // namespace

bool ParseSpace(const String& name, Space& space)
{
	const String lower = StringUtilities::ToLower(name);
	struct Entry {
		const char* name;
		Space space;
	};
	static const Entry entries[] = {
		{"srgb", Space::SRGB},
		{"srgb-linear", Space::SRGBLinear},
		{"hsl", Space::HSL},
		{"hwb", Space::HWB},
		{"lab", Space::Lab},
		{"lch", Space::LCH},
		{"oklab", Space::Oklab},
		{"oklch", Space::Oklch},
		{"xyz", Space::XYZD65},
		{"xyz-d65", Space::XYZD65},
		{"xyz-d50", Space::XYZD50},
	};
	for (const Entry& entry : entries)
	{
		if (lower == entry.name)
		{
			space = entry.space;
			return true;
		}
	}
	return false;
}

bool IsPolar(Space space)
{
	return space == Space::HSL || space == Space::HWB || space == Space::LCH || space == Space::Oklch;
}

int HueIndex(Space space)
{
	return (space == Space::HSL || space == Space::HWB) ? 0 : 2;
}

Color Convert(const Color& color, Space destination)
{
	if (color.space == destination)
		return color;

	Vec3 in = {}, xyz = {}, out = {};
	for (int i = 0; i < 3; i++)
		in[i] = color.missing[i] ? 0.0 : double(color.components[i]);
	ToXyzD65(color.space, in, xyz);
	FromXyzD65(destination, xyz, out);

	Color result;
	result.space = destination;
	result.alpha = color.alpha;
	result.alpha_missing = color.alpha_missing;
	for (int i = 0; i < 3; i++)
		result.components[i] = float(out[i]);

	if (IsPolar(destination))
	{
		const int hue = HueIndex(destination);
		const bool source_hue_missing = IsPolar(color.space) && color.missing[HueIndex(color.space)];
		if (source_hue_missing || IsHuePowerless(destination, out))
		{
			result.missing[hue] = true;
			result.components[hue] = 0.f;
		}
	}
	return result;
}

Colourb ToColourb(const Color& color)
{
	const Color srgb = Convert(color, Space::SRGB);
	Colourb result;
	for (int i = 0; i < 3; i++)
	{
		const float c = srgb.missing[i] ? 0.f : srgb.components[i];
		result[i] = (byte)Math::Clamp(Math::RoundToInteger(Math::Clamp(c, 0.f, 1.f) * 255.f), 0, 255);
	}
	const float alpha = srgb.alpha_missing ? 0.f : srgb.alpha;
	result[3] = (byte)Math::Clamp(Math::RoundToInteger(Math::Clamp(alpha, 0.f, 1.f) * 255.f), 0, 255);
	return result;
}

Color FromColourb(Colourb colour)
{
	Color result;
	result.space = Space::SRGB;
	for (int i = 0; i < 3; i++)
		result.components[i] = float(colour[i]) / 255.f;
	result.alpha = float(colour[3]) / 255.f;
	return result;
}

Color Mix(Space space, HueInterpolation hue_interpolation, const Color& a, const Color& b, float weight_a, float alpha_multiplier)
{
	Color ca = Convert(a, space);
	Color cb = Convert(b, space);

	// A missing component takes the other color's value (CSS Color 4, 12.2).
	for (int i = 0; i < 3; i++)
	{
		if (ca.missing[i] && !cb.missing[i])
			ca.components[i] = cb.components[i];
		else if (cb.missing[i] && !ca.missing[i])
			cb.components[i] = ca.components[i];
	}
	if (ca.alpha_missing && !cb.alpha_missing)
		ca.alpha = cb.alpha;
	else if (cb.alpha_missing && !ca.alpha_missing)
		cb.alpha = ca.alpha;

	const double alpha_a = Math::Clamp(ca.alpha, 0.f, 1.f), alpha_b = Math::Clamp(cb.alpha, 0.f, 1.f);
	const double w = Math::Clamp(weight_a, 0.f, 1.f);
	const double alpha = Interpolate(alpha_a, alpha_b, w);
	const int hue = IsPolar(space) ? HueIndex(space) : -1;

	Color result;
	result.space = space;
	for (int i = 0; i < 3; i++)
	{
		if (i == hue)
		{
			double h1 = NormalizeHue(ca.components[i]), h2 = NormalizeHue(cb.components[i]);
			switch (hue_interpolation)
			{
			case HueInterpolation::Shorter:
				if (h2 - h1 > 180)
					h1 += 360;
				else if (h2 - h1 < -180)
					h2 += 360;
				break;
			case HueInterpolation::Longer:
				if (0 < h2 - h1 && h2 - h1 < 180)
					h1 += 360;
				else if (-180 < h2 - h1 && h2 - h1 <= 0)
					h2 += 360;
				break;
			case HueInterpolation::Increasing:
				if (h2 < h1)
					h2 += 360;
				break;
			case HueInterpolation::Decreasing:
				if (h1 < h2)
					h1 += 360;
				break;
			}
			result.components[i] = float(NormalizeHue(Interpolate(h1, h2, w)));
			result.missing[i] = ca.missing[i] && cb.missing[i];
		}
		else
		{
			// Premultiplied interpolation.
			const double value = Interpolate(double(ca.components[i]) * alpha_a, double(cb.components[i]) * alpha_b, w);
			result.components[i] = float(alpha > 0 ? value / alpha : 0.0);
			result.missing[i] = ca.missing[i] && cb.missing[i];
		}
	}
	result.alpha = float(alpha) * alpha_multiplier;
	result.alpha_missing = ca.alpha_missing && cb.alpha_missing;
	return result;
}

} // namespace ColorSpaces
} // namespace Rml
