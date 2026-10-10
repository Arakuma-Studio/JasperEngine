#include "GeometryBorderStyles.h"
#include "../../Include/RmlUi/Core/Math.h"
#include <algorithm>
#include <cmath>

namespace Rml {

using Style::BorderStyle;

namespace {
	enum Edge { TOP, RIGHT, BOTTOM, LEFT };

	// A side is drawn as a strip between two rails, its inner and its outer edge, sampled in pairs from the corner split
	// at its start to the corner split at its end. Lengths along the side are measured on its centre line.
	struct RailPair {
		Vector2f inner, outer;
		Vector2f centre;
		float s = 0.f; // Length along the centre line from the start of the side.
	};
	using Rail = Vector<RailPair>;

	// The direction of each side (clockwise) and its inward normal.
	const Vector2f side_direction[4] = {{1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}, {0.f, -1.f}};
	const Vector2f side_normal[4] = {{0.f, 1.f}, {-1.f, 0.f}, {0.f, -1.f}, {1.f, 0.f}};

	float Dot(Vector2f a, Vector2f b)
	{
		return a.x * b.x + a.y * b.y;
	}
	float Cross(Vector2f a, Vector2f b)
	{
		return a.x * b.y - a.y * b.x;
	}
	float Length(Vector2f a)
	{
		return Math::SquareRoot(Dot(a, a));
	}

	Vector2f OuterRadius(const BorderMetrics& metrics, int corner)
	{
		return Vector2f(metrics.outer_radii[corner]);
	}
	bool IsRounded(Vector2f radius)
	{
		return radius.x > 0.f && radius.y > 0.f;
	}

	// The angles of a corner's arcs: from the side before the corner (edge0) to the side after it (edge1), clockwise.
	float StartAngle(int corner)
	{
		return float(corner + 2) * 0.5f * Math::RMLUI_PI;
	}
	float EndAngle(int corner)
	{
		return float(corner + 3) * 0.5f * Math::RMLUI_PI;
	}

	Vector2f ArcPoint(Vector2f center, Vector2f radius, float angle)
	{
		return center + Vector2f(Math::Cos(angle) * radius.x, Math::Sin(angle) * radius.y);
	}

	Vector2f InnerPoint(const BorderMetrics& metrics, int corner, float angle)
	{
		const Vector2f r = metrics.inner_radii[corner];
		if (r.x > 0.f && r.y > 0.f)
			return ArcPoint(metrics.positions_circle_center[corner], r, angle);
		return metrics.positions_inner[corner];
	}

	// Segments of a quarter arc, so that the curve deviates from its chords by less than a tenth of a pixel.
	int QuarterSegments(Vector2f radius)
	{
		const float r = Math::Max(radius.x, radius.y);
		if (r <= 0.5f)
			return 1;
		const float step = 2.f * std::acos(Math::Max(0.f, 1.f - 0.1f / r));
		return Math::Clamp(int(std::ceil(0.5f * Math::RMLUI_PI / step)), 2, 128);
	}

	// The angle at which a rounded corner changes from the side before it to the side after it: where the line from the
	// outer to the inner corner of the box crosses the outer arc (CSS Backgrounds 3, 4.4: the transition between the
	// sides' colours and styles is proportional to their widths).
	float SplitAngle(const BorderMetrics& metrics, const EdgeSizes& widths, int corner)
	{
		const float a0 = StartAngle(corner), a1 = EndAngle(corner);
		const float w0 = widths[(corner + 3) % 4], w1 = widths[corner];
		if (w0 <= 0.f && w1 <= 0.f)
			return 0.5f * (a0 + a1);
		if (w0 <= 0.f)
			return a0;
		if (w1 <= 0.f)
			return a1;

		const Vector2f radius = OuterRadius(metrics, corner);
		const Vector2f outer = metrics.positions_outer[corner];
		const Vector2f direction = metrics.positions_inner[corner] - outer;
		const Vector2f center = metrics.positions_circle_center[corner];
		auto side_of = [&](float angle) { return Cross(direction, ArcPoint(center, radius, angle) - outer) > 0.f; };

		float lo = a0, hi = a1;
		const bool lo_side = side_of(lo);
		if (lo_side == side_of(hi))
			return 0.5f * (a0 + a1);
		for (int i = 0; i < 24; i++)
		{
			const float mid = 0.5f * (lo + hi);
			if (side_of(mid) == lo_side)
				lo = mid;
			else
				hi = mid;
		}
		return 0.5f * (lo + hi);
	}

	void AddArc(Rail& rail, const BorderMetrics& metrics, int corner, float a_begin, float a_end)
	{
		const Vector2f radius = OuterRadius(metrics, corner);
		const float fraction = Math::Absolute(a_end - a_begin) / (0.5f * Math::RMLUI_PI);
		const int num_segments = Math::Max(1, int(std::ceil(float(QuarterSegments(radius)) * fraction)));
		for (int i = 0; i <= num_segments; i++)
		{
			const float angle = Math::Lerp(float(i) / float(num_segments), a_begin, a_end);
			RailPair pair;
			pair.inner = InnerPoint(metrics, corner, angle);
			pair.outer = ArcPoint(metrics.positions_circle_center[corner], radius, angle);
			pair.centre = 0.5f * (pair.inner + pair.outer);
			rail.push_back(pair);
		}
	}

	// The rail of one side, from the split of the corner at its start to the split of the corner at its end. A square
	// corner starts with the outer corner itself and continues with the pair across from the inner corner, so that the
	// side is cut along the line from the outer to the inner corner and lengths are measured along the outer edge.
	void BuildRail(Rail& rail, const BorderMetrics& metrics, const EdgeSizes& widths, int side, const float split[4])
	{
		rail.clear();
		const int c0 = side, c1 = (side + 1) % 4;
		const Vector2f direction = side_direction[side];
		const Vector2f half_normal = side_normal[side] * (0.5f * widths[side]);

		if (IsRounded(OuterRadius(metrics, c0)))
		{
			AddArc(rail, metrics, c0, split[c0], EndAngle(c0));
		}
		else
		{
			const Vector2f outer = metrics.positions_outer[c0], inner = metrics.positions_inner[c0];
			const float along = Math::Max(0.f, Dot(inner - outer, direction));
			rail.push_back(RailPair{outer, outer, outer + half_normal});
			rail.push_back(RailPair{inner, outer + direction * along, outer + direction * along + half_normal});
		}

		if (IsRounded(OuterRadius(metrics, c1)))
		{
			AddArc(rail, metrics, c1, StartAngle(c1), split[c1]);
		}
		else
		{
			const Vector2f outer = metrics.positions_outer[c1], inner = metrics.positions_inner[c1];
			const float along = Math::Max(0.f, Dot(outer - inner, direction));
			rail.push_back(RailPair{inner, outer - direction * along, outer - direction * along + half_normal});
			rail.push_back(RailPair{outer, outer, outer + half_normal});
		}

		float s = 0.f;
		for (size_t i = 0; i < rail.size(); i++)
		{
			if (i > 0)
				s += Length(rail[i].centre - rail[i - 1].centre);
			rail[i].s = s;
		}
	}

	RailPair Interpolate(const RailPair& a, const RailPair& b, float s)
	{
		const float t = (b.s > a.s ? Math::Clamp((s - a.s) / (b.s - a.s), 0.f, 1.f) : 0.f);
		RailPair result;
		result.inner = Math::Lerp(t, a.inner, b.inner);
		result.outer = Math::Lerp(t, a.outer, b.outer);
		result.centre = Math::Lerp(t, a.centre, b.centre);
		result.s = s;
		return result;
	}

	RailPair PairAt(const Rail& rail, float s)
	{
		for (size_t i = 1; i < rail.size(); i++)
		{
			if (s <= rail[i].s)
				return Interpolate(rail[i - 1], rail[i], s);
		}
		return rail.back();
	}

	class BorderWriter {
	public:
		BorderWriter(Vector<Vertex>& vertices, Vector<int>& indices) : vertices(vertices), indices(indices) {}

		// The band between the fractions t_outer and t_inner of the side's width (0 = outer edge, 1 = inner edge), from s0 to s1.
		void Strip(const Rail& rail, float s0, float s1, float t_outer, float t_inner, ColourbPremultiplied color)
		{
			if (s1 <= s0 && !(s0 == 0.f && s1 == 0.f))
				return;
			points.clear();
			points.push_back(PairAt(rail, s0));
			for (const RailPair& pair : rail)
			{
				if (pair.s > s0 && pair.s < s1)
					points.push_back(pair);
			}
			points.push_back(PairAt(rail, s1));

			const int first = (int)vertices.size();
			for (const RailPair& pair : points)
			{
				AddVertex(Math::Lerp(t_inner, pair.outer, pair.inner), color);
				AddVertex(Math::Lerp(t_outer, pair.outer, pair.inner), color);
			}
			for (int i = 0; i + 1 < (int)points.size(); i++)
			{
				const int v = first + 2 * i;
				AddTriangle(v, v + 2, v + 1);
				AddTriangle(v + 1, v + 2, v + 3);
			}
		}

		// A round dot (a square for a width below 2px, as Chromium draws one-pixel dots).
		void Dot(Vector2f center, float diameter, ColourbPremultiplied color)
		{
			const float radius = 0.5f * diameter;
			const int first = (int)vertices.size();
			if (diameter < 2.f)
			{
				AddVertex(center + Vector2f(-radius, -radius), color);
				AddVertex(center + Vector2f(radius, -radius), color);
				AddVertex(center + Vector2f(radius, radius), color);
				AddVertex(center + Vector2f(-radius, radius), color);
				AddTriangle(first, first + 1, first + 2);
				AddTriangle(first, first + 2, first + 3);
				return;
			}
			const int num_points = 4 * QuarterSegments(Vector2f(radius));
			AddVertex(center, color);
			for (int i = 0; i < num_points; i++)
			{
				const float angle = 2.f * Math::RMLUI_PI * float(i) / float(num_points);
				AddVertex(ArcPoint(center, Vector2f(radius), angle), color);
			}
			for (int i = 0; i < num_points; i++)
				AddTriangle(first, first + 1 + i, first + 1 + (i + 1) % num_points);
		}

	private:
		void AddVertex(Vector2f position, ColourbPremultiplied color)
		{
			Vertex vertex;
			vertex.position = position;
			vertex.colour = color;
			vertex.tex_coord = Vector2f(0.f);
			vertices.push_back(vertex);
		}
		void AddTriangle(int a, int b, int c)
		{
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
		}

		Vector<Vertex>& vertices;
		Vector<int>& indices;
		Rail points;
	};

	// The parts of [0, length] left once the interval [gap_begin, gap_end] is removed.
	struct Interval {
		float begin, end;
	};
	void AddInterval(Vector<Interval>& out, float begin, float end, Interval gap)
	{
		if (gap.end <= gap.begin || end <= gap.begin || begin >= gap.end)
		{
			out.push_back({begin, end});
			return;
		}
		if (begin < gap.begin)
			out.push_back({begin, gap.begin});
		if (end > gap.end)
			out.push_back({gap.end, end});
	}

	// Whether the side is the dark side of a groove, ridge, inset or outset border, and the dark part of a groove/ridge
	// is its outer half.
	bool IsTopOrLeft(int side)
	{
		return side == TOP || side == LEFT;
	}

	Colourb Unpremultiplied(ColourbPremultiplied color)
	{
		if (color.alpha == 0 || color.alpha == 255)
			return Colourb(color.red, color.green, color.blue, color.alpha);
		auto channel = [&](byte c) { return byte(Math::Min(255, (int(c) * 255 + color.alpha / 2) / color.alpha)); };
		return Colourb(channel(color.red), channel(color.green), channel(color.blue), color.alpha);
	}
} // namespace

ColourbPremultiplied GeometryBorderStyles::DarkColor(ColourbPremultiplied premultiplied)
{
	// Chromium's Color::Dark(): black becomes a fixed grey, other colours are scaled down by (v - 0.33) / v, v being
	// their largest channel.
	const Colourb color = Unpremultiplied(premultiplied);
	if (color.red == 0 && color.green == 0 && color.blue == 0)
		return Colourb(0x54, 0x54, 0x54, color.alpha).ToPremultiplied();
	const float v = float(Math::Max(color.red, Math::Max(color.green, color.blue))) / 255.f;
	const float multiplier = Math::Max(0.f, (v - 0.33f) / v);
	auto scale = [&](byte c) { return byte(Math::RoundToInteger(float(c) * multiplier)); };
	return Colourb(scale(color.red), scale(color.green), scale(color.blue), color.alpha).ToPremultiplied();
}

ColourbPremultiplied GeometryBorderStyles::LightColor(ColourbPremultiplied color)
{
	return color;
}

bool GeometryBorderStyles::IsNeeded(const BorderMetrics& metrics, EdgeSizes edge_sizes, const ColourbPremultiplied colors[4],
	const BorderStyles& styles)
{
	for (int side = 0; side < 4; side++)
	{
		if (edge_sizes[side] > 0.f && colors[side].alpha > 0 && styles[side] != BorderStyle::Solid)
			return true;
	}
	for (int corner = 0; corner < 4; corner++)
	{
		const int edge0 = (corner + 3) % 4, edge1 = corner;
		if (IsRounded(OuterRadius(metrics, corner)) && colors[edge0] != colors[edge1] && (edge_sizes[edge0] > 0.f || edge_sizes[edge1] > 0.f))
			return true;
	}
	return false;
}

void GeometryBorderStyles::DrawBorder(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, EdgeSizes edge_sizes,
	const ColourbPremultiplied colors[4], const BorderStyles& styles, Vector2f top_gap)
{
	float split[4];
	for (int corner = 0; corner < 4; corner++)
		split[corner] = SplitAngle(metrics, edge_sizes, corner);

	BorderWriter writer(vertices, indices);
	Rail rail;
	Vector<Interval> intervals;

	for (int side = 0; side < 4; side++)
	{
		const float width = edge_sizes[side];
		const ColourbPremultiplied color = colors[side];
		BorderStyle style = styles[side];
		if (width <= 0.f || color.alpha == 0 || !Style::IsBorderStylePainted(style))
			continue;

		BuildRail(rail, metrics, edge_sizes, side, split);
		const float length = rail.back().s;

		// The interrupted part of the top edge (Element::GetBorderTopCut), measured along the outer edge: within the
		// straight part of the edge, where lengths along the side and along the outer edge are the same.
		Interval gap = {0.f, 0.f};
		if (side == TOP && top_gap.y > top_gap.x)
		{
			const float x_left = metrics.positions_outer[0].x;
			const float x_min = Math::Max(metrics.positions_inner[0].x, x_left + metrics.outer_radii[0]);
			const float x_max = Math::Min(metrics.positions_inner[1].x, metrics.positions_outer[1].x - metrics.outer_radii[1]);
			const float x0 = Math::Clamp(x_left + top_gap.x, x_min, x_max);
			const float x1 = Math::Clamp(x_left + top_gap.y, x_min, x_max);
			// The rail pair at the start of the straight part.
			for (const RailPair& pair : rail)
			{
				if (pair.outer.x >= x_min - 0.001f && pair.outer.y <= metrics.positions_outer[0].y + 0.001f)
				{
					gap = {pair.s + (x0 - pair.outer.x), pair.s + (x1 - pair.outer.x)};
					break;
				}
			}
		}

		if (style == BorderStyle::Double && width < 3.f)
			style = BorderStyle::Solid;

		switch (style)
		{
		case BorderStyle::Dashed:
		{
			// Chromium: dashes of 3w with gaps of 2w below 3px, else 2w and w; a whole number of dashes with a full
			// dash at each end, the gaps stretched to fit.
			const float dash = (width < 3.f ? 3.f : 2.f) * width;
			const float space = (width < 3.f ? 2.f : 1.f) * width;
			int num_dashes = Math::Max(1, Math::RoundToInteger((length + space) / (dash + space)));
			while (num_dashes > 1 && float(num_dashes) * dash > length)
				num_dashes--;
			intervals.clear();
			if (num_dashes <= 1)
			{
				AddInterval(intervals, 0.f, length, gap);
			}
			else
			{
				const float stretched_space = (length - float(num_dashes) * dash) / float(num_dashes - 1);
				for (int i = 0; i < num_dashes; i++)
				{
					const float begin = float(i) * (dash + stretched_space);
					AddInterval(intervals, begin, begin + dash, gap);
				}
			}
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, color);
			break;
		}
		case BorderStyle::Dotted:
		{
			// Chromium: round dots of the side's width, about two widths apart, one centred on each corner split.
			Rail path;
			path.reserve(rail.size() + 2);
			const int c0 = side, c1 = (side + 1) % 4;
			auto add_point = [&](Vector2f centre) {
				RailPair pair;
				pair.centre = centre;
				pair.s = (path.empty() ? 0.f : path.back().s + Length(centre - path.back().centre));
				path.push_back(pair);
			};
			const bool rounded_start = IsRounded(OuterRadius(metrics, c0)), rounded_end = IsRounded(OuterRadius(metrics, c1));
			add_point(rounded_start ? rail.front().centre : 0.5f * (metrics.positions_outer[c0] + metrics.positions_inner[c0]));
			for (size_t i = (rounded_start ? 1 : 2); i + (rounded_end ? 1 : 2) < rail.size(); i++)
				add_point(rail[i].centre);
			add_point(rounded_end ? rail.back().centre : 0.5f * (metrics.positions_outer[c1] + metrics.positions_inner[c1]));

			const float path_length = path.back().s;
			const int num_spaces = Math::Max(1, Math::RoundToInteger(path_length / (2.f * width)));
			for (int i = 0; i <= num_spaces; i++)
			{
				const float s = path_length * float(i) / float(num_spaces);
				const Vector2f centre = PairAt(path, s).centre;
				if (side == TOP && gap.end > gap.begin)
				{
					const float x = centre.x - metrics.positions_outer[0].x;
					if (x + 0.5f * width > top_gap.x && x - 0.5f * width < top_gap.y)
						continue;
				}
				writer.Dot(centre, width, color);
			}
			break;
		}
		case BorderStyle::Double:
		{
			const float band = Math::Round(width / 3.f) / width;
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
			{
				writer.Strip(rail, interval.begin, interval.end, 0.f, band, color);
				writer.Strip(rail, interval.begin, interval.end, 1.f - band, 1.f, color);
			}
			break;
		}
		case BorderStyle::Groove:
		case BorderStyle::Ridge:
		{
			// Groove: the top and left sides dark outside and light inside, the bottom and right ones the other way round.
			const bool outer_dark = (IsTopOrLeft(side) == (style == BorderStyle::Groove));
			const ColourbPremultiplied dark = DarkColor(color), light = LightColor(color);
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
			{
				writer.Strip(rail, interval.begin, interval.end, 0.f, 0.5f, outer_dark ? dark : light);
				writer.Strip(rail, interval.begin, interval.end, 0.5f, 1.f, outer_dark ? light : dark);
			}
			break;
		}
		case BorderStyle::Inset:
		case BorderStyle::Outset:
		{
			const bool dark = (IsTopOrLeft(side) == (style == BorderStyle::Inset));
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, dark ? DarkColor(color) : LightColor(color));
			break;
		}
		default:
		{
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, color);
			break;
		}
		}
	}
}

} // namespace Rml
