#include "GeometryBorderStyles.h"
#include "../../Include/RmlUi/Core/Math.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>

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
		// Feathering: the moves of the outer and inner points of the pair for their edge to move one pixel inwards (the
		// curve's normal, or the corner of the two edges moved at a square corner), and the side's thickness across its edge.
		Vector2f axis_outer, axis_inner;
		float thickness = 0.f;
		bool straight = true; // At a square corner or where a curve meets a straight edge (not inside a curve).
		bool square = false;  // The split of a square corner: the line from the outer to the inner corner.
		float adjacent = 0.f; // At a square corner: the width of the side next to this one.
		Vector2f inside;      // At a square corner: across the split, towards this side.
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

	// The unit normal of an ellipse at a parametric angle, pointing to its centre.
	Vector2f EllipseInwardNormal(Vector2f radius, float angle)
	{
		const Vector2f gradient(Math::Cos(angle) / Math::Max(radius.x, 1e-4f), Math::Sin(angle) / Math::Max(radius.y, 1e-4f));
		const float length = Length(gradient);
		return length > 1e-6f ? -gradient / length : Vector2f(0.f);
	}

	void AddArc(Rail& rail, const BorderMetrics& metrics, int corner, float a_begin, float a_end, float unit)
	{
		const Vector2f radius = OuterRadius(metrics, corner);
		const float fraction = Math::Absolute(a_end - a_begin) / (0.5f * Math::RMLUI_PI);
		const int num_segments = Math::Max(1, int(std::ceil(float(QuarterSegments(radius / unit)) * fraction)));
		for (int i = 0; i <= num_segments; i++)
		{
			const float angle = Math::Lerp(float(i) / float(num_segments), a_begin, a_end);
			RailPair pair;
			pair.inner = InnerPoint(metrics, corner, angle);
			pair.outer = ArcPoint(metrics.positions_circle_center[corner], radius, angle);
			pair.centre = 0.5f * (pair.inner + pair.outer);
			pair.axis_outer = EllipseInwardNormal(radius, angle);
			const Vector2f inner_radius = metrics.inner_radii[corner];
			if (inner_radius.x > 0.f && inner_radius.y > 0.f)
				pair.axis_inner = EllipseInwardNormal(inner_radius, angle);
			else
				pair.axis_inner = side_normal[corner] + side_normal[(corner + 3) % 4];
			pair.thickness = Math::Max(Dot(pair.inner - pair.outer, pair.axis_outer), 0.f);
			// The ends of a corner's arc touch the straight edges: they are feathered as straight edges.
			pair.straight = (angle == StartAngle(corner) || angle == EndAngle(corner));
			rail.push_back(pair);
		}
	}

	// The rail of one side, from the split of the corner at its start to the split of the corner at its end; a square
	// corner is split along the line from the outer to the inner corner. Lengths are measured along the side's centre
	// line, and from the outer corner of a square corner (Chromium lays dashes out along the outer edge).
	struct SideRail {
		Rail pairs;
		float length = 0.f;
		Vector2f direction; // The side's direction, for segments whose centre does not move.
		size_t straight_begin = 0; // The pair where the corner at the start ends (the straight part begins).
	};

	// The pair of a side at a square corner: the corner's split, from its outer to its inner corner. 'towards' points
	// along the side away from the corner.
	RailPair SquarePair(const BorderMetrics& metrics, int side, int corner, int adjacent_side, Vector2f towards)
	{
		RailPair pair;
		pair.outer = metrics.positions_outer[corner];
		pair.inner = metrics.positions_inner[corner];
		pair.centre = 0.5f * (pair.inner + pair.outer);
		const Vector2f diagonal = pair.inner - pair.outer;
		pair.thickness = Dot(diagonal, side_normal[side]);
		pair.adjacent = Dot(diagonal, side_normal[adjacent_side]);
		// The edges moved by one pixel meet at the corner moved along both normals; a side without a neighbour ends there.
		const Vector2f miter = side_normal[side] + (pair.adjacent > 0.f ? side_normal[adjacent_side] : Vector2f(0.f));
		pair.axis_outer = pair.axis_inner = miter;
		pair.straight = true;
		pair.square = true;
		pair.inside = Vector2f(diagonal.y, -diagonal.x);
		if (Dot(pair.inside, towards) < 0.f)
			pair.inside = -pair.inside;
		return pair;
	}

	void BuildRail(SideRail& rail, const BorderMetrics& metrics, int side, const float split[4], float unit)
	{
		rail.pairs.clear();
		const int c0 = side, c1 = (side + 1) % 4;
		const Vector2f direction = side_direction[side];
		rail.direction = direction;

		const bool square_start = !IsRounded(OuterRadius(metrics, c0));
		if (square_start)
		{
			rail.pairs.push_back(SquarePair(metrics, side, c0, (side + 3) % 4, direction));
		}
		else
		{
			AddArc(rail.pairs, metrics, c0, split[c0], EndAngle(c0), unit);
		}

		rail.straight_begin = rail.pairs.size() - 1;

		const bool square_end = !IsRounded(OuterRadius(metrics, c1));
		if (square_end)
		{
			rail.pairs.push_back(SquarePair(metrics, side, c1, (side + 1) % 4, -direction));
		}
		else
		{
			AddArc(rail.pairs, metrics, c1, StartAngle(c1), split[c1], unit);
		}

		Rail& pairs = rail.pairs;
		float s = (square_start ? Dot(pairs[0].centre - metrics.positions_outer[c0], direction) : 0.f);
		for (size_t i = 0; i < pairs.size(); i++)
		{
			if (i > 0)
				s += Length(pairs[i].centre - pairs[i - 1].centre);
			pairs[i].s = s;
		}
		rail.length = s + (square_end ? Dot(metrics.positions_outer[c1] - pairs.back().centre, direction) : 0.f);
	}

	RailPair Interpolate(const RailPair& a, const RailPair& b, float s)
	{
		const float t = (b.s > a.s ? Math::Clamp((s - a.s) / (b.s - a.s), 0.f, 1.f) : 0.f);
		RailPair result;
		result.inner = Math::Lerp(t, a.inner, b.inner);
		result.outer = Math::Lerp(t, a.outer, b.outer);
		result.centre = Math::Lerp(t, a.centre, b.centre);
		result.s = s;
		result.axis_outer = Math::Lerp(t, a.axis_outer, b.axis_outer);
		result.axis_inner = Math::Lerp(t, a.axis_inner, b.axis_inner);
		result.thickness = Math::Lerp(t, a.thickness, b.thickness);
		result.straight = a.straight && b.straight;
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

	// Geometry with feathered edges: an edge is moved half a pixel inwards and a one-pixel ring fades from it to
	// transparent half a pixel outside, so that the coverage of a pixel crossed by the edge is close to the area the
	// shape covers (Chromium's analytic anti-aliasing); pixel-aligned edges stay sharp.
	// The half width of a feathered edge's fade: half a pixel on straight edges, which keeps pixel-aligned edges exact
	// (with or without multisampling), and a little more on curves, which keeps their coverage within 7 % of the area
	// they cover under 4x multisampling too (the samples of a pixel in the fade then mostly take the fade's value at the
	// pixel centre). A transparent margin of half a pixel follows the fade, for the same reason.
	constexpr float feather_half = 0.5f;
	constexpr float feather_half_curve = 0.55f;
	constexpr float feather_margin = 0.5f;

	class BorderWriter {
	public:
		// 'unit' is the size of a device pixel in the geometry's units (1 unless the geometry is drawn scaled).
		BorderWriter(Vector<Vertex>& vertices, Vector<int>& indices, FeatherEdges edges, float unit) :
			vertices(vertices), indices(indices), feather(edges == FeatherEdges::All), feather_curves(edges != FeatherEdges::None), unit(unit)
		{}

		// The band between the fractions t_outer and t_inner of the side's width (0 = outer edge, 1 = inner edge), over
		// [s0, s1] of its length; the ends are cut across the side. With feathering, the band's long edges are feathered,
		// and so are its ends where they cut the side (not at the side's corners).
		void Strip(const SideRail& rail, float s0, float s1, float t_outer, float t_inner, ColourbPremultiplied color, bool feather_outer = true,
			bool feather_inner = true)
		{
			const Rail& pairs = rail.pairs;
			const float side_begin = pairs.front().s - 0.001f, side_end = pairs.back().s + 0.001f;
			const bool cut_begin = (s0 > Math::Min(side_begin, 0.f) + 0.002f), cut_end = (s1 < Math::Max(side_end, rail.length) - 0.002f);

			// The pieces along the side: [s0, s1], or with feathered cut ends a fade in, the body and a fade out.
			struct Piece {
				float begin, end;
				float alpha_begin, alpha_end;
			};
			Piece pieces[5];
			int num_pieces = 0;
			if (!feather || (!cut_begin && !cut_end))
			{
				pieces[num_pieces++] = {s0, s1, 1.f, 1.f};
			}
			else
			{
				const float fade_begin = (cut_begin ? (feather_half * unit) : 0.f), fade_end = (cut_end ? (feather_half * unit) : 0.f);
				const float body_begin = s0 + fade_begin, body_end = s1 - fade_end;
				if (body_end >= body_begin)
				{
					// The fades reach a transparent margin, so that the samples of a pixel whose centre is in a fade all take it.
					if (cut_begin)
					{
						pieces[num_pieces++] = {s0 - (feather_half * unit) - (feather_margin * unit), s0 - (feather_half * unit), 0.f, 0.f};
						pieces[num_pieces++] = {s0 - (feather_half * unit), body_begin, 0.f, 1.f};
					}
					pieces[num_pieces++] = {body_begin, body_end, 1.f, 1.f};
					if (cut_end)
					{
						pieces[num_pieces++] = {body_end, s1 + (feather_half * unit), 1.f, 0.f};
						pieces[num_pieces++] = {s1 + (feather_half * unit), s1 + (feather_half * unit) + (feather_margin * unit), 0.f, 0.f};
					}
				}
				else
				{
					// Shorter than the fades: a peak of the dash's length.
					const float middle = 0.5f * (s0 + s1), peak = Math::Clamp((s1 - s0) / unit, 0.f, 1.f);
					pieces[num_pieces++] = {cut_begin ? s0 - (feather_half * unit) : s0, middle, cut_begin ? 0.f : peak, peak};
					pieces[num_pieces++] = {middle, cut_end ? s1 + (feather_half * unit) : s1, peak, cut_end ? 0.f : peak};
				}
			}

			for (size_t k = 0; k + 1 < pairs.size(); k++)
			{
				const RailPair& a = pairs[k];
				const RailPair& b = pairs[k + 1];

				// Lengths across the segment: along the direction its centre moves.
				const Vector2f delta = b.centre - a.centre;
				const float delta_length = Length(delta);
				const Vector2f direction = (delta_length > 1e-4f ? delta / delta_length : rail.direction);
				auto length_at = [&](Vector2f p) { return a.s + Dot(p - a.centre, direction); };

				// The levels across the band at both pairs: positions along the pair and their opacity.
				// One layout of levels for both pairs: with a solid core only if the band is thick enough at both.
				const bool feather_band = (feather_outer || feather_inner) && a.thickness > 1e-4f && b.thickness > 1e-4f &&
					(HalfOf(a) > 0.f || HalfOf(b) > 0.f);
				const float needed = (feather_outer ? (feather_half_curve * unit) : 0.f) + (feather_inner ? (feather_half_curve * unit) : 0.f);
				const bool core = !feather_band || Math::Min(a.thickness, b.thickness) * (t_inner - t_outer) >= needed;
				Level levels_a[8], levels_b[8];
				const int num_levels = BandLevels(a, t_outer, t_inner, feather_band, feather_outer, feather_inner, core, levels_a);
				BandLevels(b, t_outer, t_inner, feather_band, feather_outer, feather_inner, core, levels_b);

				// Square corners of sides of different widths: the fades of the two sides leave a sliver between the corner
				// of the moved edges and the corner's split; it is this side's when that corner is on its side of the split.
				if (feather_band && core)
				{
					for (const RailPair* pair : {&a, &b})
					{
						if (pair->square && ((pair == &a && k == 0) || (pair == &b && k + 2 == pairs.size())))
						{
							if (feather_outer)
								CornerSliver(*pair, t_outer, 1.f, color);
							if (feather_inner)
								CornerSliver(*pair, t_inner, -1.f, color);
						}
					}
				}

				for (int i = 0; i + 1 < num_levels; i++)
				{
					for (int piece_index = 0; piece_index < num_pieces; piece_index++)
					{
						const Piece& piece = pieces[piece_index];
						polygon.clear();
						polygon.push_back({levels_a[i].position, levels_a[i].alpha});
						polygon.push_back({levels_b[i].position, levels_b[i].alpha});
						polygon.push_back({levels_b[i + 1].position, levels_b[i + 1].alpha});
						polygon.push_back({levels_a[i + 1].position, levels_a[i + 1].alpha});
						// The side's own ends (its corners) are not cut: the pairs there are its edge.
						const bool clip_begin = (piece.begin > side_begin + 0.002f), clip_end = (piece.end < side_end - 0.002f);
						if (clip_begin)
							ClipPolygon(length_at, piece.begin, true);
						if (clip_end)
							ClipPolygon(length_at, piece.end, false);
						if (polygon.size() < 3)
							continue;
						const bool fades = (piece.alpha_begin != 1.f || piece.alpha_end != 1.f);
						const int first = (int)vertices.size();
						for (const PolygonPoint& point : polygon)
						{
							float alpha = point.alpha;
							if (fades && piece.end > piece.begin)
								alpha *= Math::Lerp(Math::Clamp((length_at(point.position) - piece.begin) / (piece.end - piece.begin), 0.f, 1.f),
									piece.alpha_begin, piece.alpha_end);
							AddVertex(point.position, Scale(color, alpha));
						}
						for (int j = 1; j + 1 < (int)polygon.size(); j++)
							AddTriangle(first, first + j, first + j + 1);
					}
				}
			}
		}

		// A round dot, or a square one, kept on the inside of the clip planes (SetDotClip).
		void AddDot(Vector2f center, float diameter, ColourbPremultiplied color, bool square = false)
		{
			const float radius = 0.5f * diameter;
			if (square)
			{
				const Vector2f a = center + Vector2f(-radius, -radius), b = center + Vector2f(radius, -radius);
				const Vector2f c = center + Vector2f(radius, radius), d = center + Vector2f(-radius, radius);
				EmitClipped({{a, 1.f}, {b, 1.f}, {c, 1.f}, {d, 1.f}}, color);
				return;
			}
			const int num_points = 4 * QuarterSegments(Vector2f((radius + 0.5f) / unit));
			const float solid_radius = (feather_curves ? Math::Max(radius - (feather_half_curve * unit), 0.f) : radius);
			const float centre_alpha = (feather_curves && radius < (feather_half_curve * unit) ? radius / (feather_half_curve * unit) : 1.f);
			auto point = [&](float ring_radius, int i) {
				return ArcPoint(center, Vector2f(ring_radius), 2.f * Math::RMLUI_PI * float(i % num_points) / float(num_points));
			};
			for (int i = 0; i < num_points; i++)
				EmitClipped({{center, centre_alpha}, {point(solid_radius, i), centre_alpha}, {point(solid_radius, i + 1), centre_alpha}}, color);
			if (feather_curves)
			{
				// The fade to transparent half a pixel outside, and a transparent margin (see BandLevels).
				const float rings[3] = {solid_radius, radius + (feather_half_curve * unit), radius + (feather_half_curve * unit) + (feather_margin * unit)};
				const float alphas[3] = {centre_alpha, 0.f, 0.f};
				for (int ring = 0; ring < 2; ring++)
				{
					for (int i = 0; i < num_points; i++)
					{
						EmitClipped({{point(rings[ring], i), alphas[ring]}, {point(rings[ring + 1], i), alphas[ring + 1]},
										{point(rings[ring + 1], i + 1), alphas[ring + 1]}, {point(rings[ring], i + 1), alphas[ring]}},
							color);
					}
				}
			}
		}

		// Dots and squares drawn next are kept on the side of the line through 'point' that 'inside' points to (corner
		// splits between sides of different colours or styles); 'enable' false removes the clip.
		void SetDotClip(int index, bool enable, Vector2f point = {}, Vector2f inside = {})
		{
			dot_clips[index] = {enable, point, inside};
		}

		// A convex polygon filled with the colour, its edges feathered (each point with its fade's half width).
		struct OutlinePoint {
			Vector2f position;
			float half;
		};
		void Fill(const Vector<OutlinePoint>& points, ColourbPremultiplied color)
		{
			// Without repeated points (where an arc meets the next one), so that every vertex has two edges.
			outline.clear();
			for (const OutlinePoint& point : points)
			{
				if (outline.empty() || Length(point.position - outline.back().position) > 1e-4f)
					outline.push_back(point);
			}
			while (outline.size() > 1 && Length(outline.back().position - outline.front().position) <= 1e-4f)
				outline.pop_back();

			const int count = (int)outline.size();
			if (count < 3)
				return;
			const int first = (int)vertices.size();
			bool any_feather = false;
			for (const OutlinePoint& point : outline)
				any_feather |= (point.half > 0.f);
			if (!any_feather)
			{
				for (const OutlinePoint& point : outline)
					AddVertex(point.position, color);
				for (int i = 1; i + 1 < count; i++)
					AddTriangle(first, first + i + 1, first + i);
				return;
			}
			// Each vertex moved along the bisector of its edges' normals (the corner of the edges moved), inwards for the
			// opaque fill, outwards for the end of the fade and the transparent margin.
			for (int pass = 0; pass < 3; pass++)
			{
				for (int i = 0; i < count; i++)
				{
					const OutlinePoint& point = outline[i];
					const float margin = (point.half > 0.f ? (feather_margin * unit) : 0.f);
					const float offset = (pass == 0 ? -point.half : pass == 1 ? point.half : point.half + margin);
					const Vector2f previous = outline[(i + count - 1) % count].position, next = outline[(i + 1) % count].position;
					const Vector2f normal = OffsetDirection(previous, point.position, next);
					AddVertex(point.position + normal * offset, pass == 0 ? color : ColourbPremultiplied(0, 0));
				}
			}
			for (int i = 1; i + 1 < count; i++)
				AddTriangle(first, first + i + 1, first + i);
			for (int ring = 0; ring < 2; ring++)
			{
				const int inside = first + ring * count, outside = inside + count;
				for (int i = 0; i < count; i++)
				{
					const int i_next = (i + 1) % count;
					AddTriangle(inside + i, outside + i, outside + i_next);
					AddTriangle(inside + i, outside + i_next, inside + i_next);
				}
			}
		}

	private:
		struct Level {
			Vector2f position;
			float alpha;
		};
		struct PolygonPoint {
			Vector2f position;
			float alpha;
		};

		// See Strip: 'inwards' is 1 for the band's outer edge (its opaque part lies further in), -1 for its inner edge.
		void CornerSliver(const RailPair& pair, float t, float inwards, ColourbPremultiplied color)
		{
			const float half = HalfOf(pair);
			if (half <= 0.f || pair.adjacent <= 0.f || pair.thickness <= 0.f || Math::Absolute(pair.adjacent - pair.thickness) < 1e-3f)
				return;
			const Vector2f corner = Math::Lerp(t, pair.outer, pair.inner) - pair.axis_outer * (inwards * half);
			if (Dot(corner - pair.outer, pair.inside) <= 0.f)
				return;
			const Vector2f own = Math::Lerp(t + inwards * half / pair.thickness, pair.outer, pair.inner);
			const Vector2f other = Math::Lerp(t + inwards * half / pair.adjacent, pair.outer, pair.inner);
			const int first = (int)vertices.size();
			AddVertex(corner, ColourbPremultiplied(0, 0));
			AddVertex(other, color);
			AddVertex(own, color);
			AddTriangle(first, first + 1, first + 2);
		}

		// The half width of the fade of the edges at a pair: 0 where they are not feathered.
		float HalfOf(const RailPair& pair) const
		{
			if (pair.straight)
				return feather ? (feather_half * unit) : 0.f;
			return feather_curves ? (feather_half_curve * unit) : 0.f;
		}

		// The levels across a band at one pair: its edges, or with feathering the rims and the edges moved inwards. A
		// feathered edge fades from opaque half a pixel inside to transparent half a pixel outside, followed by a
		// transparent margin of half a pixel: the samples of a pixel whose centre is in the fade all take the fade's value
		// (multisampling), so that a pixel's coverage is the fade at its centre. Internal edges (between the halves of a
		// groove or ridge) are not feathered.
		int BandLevels(const RailPair& pair, float t_outer, float t_inner, bool feather_band, bool feather_outer, bool feather_inner, bool core,
			Level levels[8]) const
		{
			const Vector2f outer = Math::Lerp(t_outer, pair.outer, pair.inner), inner = Math::Lerp(t_inner, pair.outer, pair.inner);
			if (!feather_band)
			{
				levels[0] = {outer, 1.f};
				levels[1] = {inner, 1.f};
				return 2;
			}
			const Vector2f axis_outer = Math::Lerp(t_outer, pair.axis_outer, pair.axis_inner);
			const Vector2f axis_inner = Math::Lerp(t_inner, pair.axis_outer, pair.axis_inner);
			const float half = HalfOf(pair);
			const float margin = (half > 0.f ? (feather_margin * unit) : 0.f);
			int count = 0;
			if (feather_outer)
			{
				levels[count++] = {outer - axis_outer * (half + margin), 0.f};
				levels[count++] = {outer - axis_outer * half, 0.f};
			}
			if (core)
			{
				if (pair.square)
				{
					// The opaque part stays split along the corner's own line (to its sides' colours), half a pixel in
					// from the edges; the fades reach the corner of the edges moved outwards. Where the side next to
					// this one is thinner, its opaque part ends further along the split: the same points are levels
					// here, so that the two sides share their vertices on the split (no gap between them).
					const float shift = half / pair.thickness;
					float t_solid_outer = (feather_outer ? t_outer + shift : t_outer), t_solid_inner = (feather_inner ? t_inner - shift : t_inner);
					float t_shared_outer = t_solid_outer, t_shared_inner = t_solid_inner;
					if (pair.adjacent > 0.f && pair.adjacent < pair.thickness - 1e-3f)
					{
						const float shift_adjacent = half / pair.adjacent;
						if (feather_outer)
							t_shared_outer = t_outer + shift_adjacent;
						if (feather_inner)
							t_shared_inner = t_inner - shift_adjacent;
						if (t_shared_outer > t_shared_inner)
							t_shared_outer = t_shared_inner = 0.5f * (t_solid_outer + t_solid_inner);
					}
					levels[count++] = {Math::Lerp(t_solid_outer, pair.outer, pair.inner), 1.f};
					levels[count++] = {Math::Lerp(t_shared_outer, pair.outer, pair.inner), 1.f};
					levels[count++] = {Math::Lerp(t_shared_inner, pair.outer, pair.inner), 1.f};
					levels[count++] = {Math::Lerp(t_solid_inner, pair.outer, pair.inner), 1.f};
				}
				else
				{
					const Level solid_outer = {feather_outer ? outer + axis_outer * half : outer, 1.f};
					const Level solid_inner = {feather_inner ? inner - axis_inner * half : inner, 1.f};
					levels[count++] = solid_outer;
					levels[count++] = solid_outer;
					levels[count++] = solid_inner;
					levels[count++] = solid_inner;
				}
			}
			else
			{
				// Thinner than its fades: a peak of the band's thickness.
				const float band = (t_inner - t_outer) * pair.thickness;
				const Vector2f peak = (feather_outer && feather_inner ? 0.5f * (outer + inner) : feather_outer ? inner : outer);
				const Level peak_level = {peak, Math::Clamp((feather_outer && feather_inner ? band : band + half) / unit, 0.f, 1.f)};
				for (int i = 0; i < 4; i++)
					levels[count++] = peak_level;
			}
			if (feather_inner)
			{
				levels[count++] = {inner + axis_inner * half, 0.f};
				levels[count++] = {inner + axis_inner * (half + margin), 0.f};
			}
			return count;
		}

		// The direction to move a polygon vertex for its edges to move one unit outwards (clockwise polygon, y down).
		static Vector2f OffsetDirection(Vector2f previous, Vector2f point, Vector2f next)
		{
			auto outward = [](Vector2f from, Vector2f to) {
				const Vector2f d = to - from;
				const float length = Length(d);
				return length > 1e-6f ? Vector2f(d.y, -d.x) / length : Vector2f(0.f);
			};
			const Vector2f n0 = outward(previous, point), n1 = outward(point, next);
			Vector2f sum = n0 + n1;
			const float sum_length = Length(sum);
			if (sum_length < 1e-6f)
				return n0;
			sum = sum / sum_length;
			const float cosine = Dot(sum, n0);
			return sum / Math::Max(cosine, 0.25f);
		}

		static ColourbPremultiplied Scale(ColourbPremultiplied color, float alpha)
		{
			if (alpha >= 1.f)
				return color;
			auto scale = [&](byte c) { return byte(float(c) * alpha + 0.5f); };
			return ColourbPremultiplied(scale(color.red), scale(color.green), scale(color.blue), scale(color.alpha));
		}

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
			// Zero-area triangles (where a fade or a band has no width) draw nothing: skipped.
			if (Math::Absolute(Cross(vertices[b].position - vertices[a].position, vertices[c].position - vertices[a].position)) < 1e-6f)
				return;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
		}

		// A convex polygon (vertex alpha scaling the colour), clipped by the dot clip planes.
		void EmitClipped(std::initializer_list<PolygonPoint> points, ColourbPremultiplied color)
		{
			polygon.assign(points.begin(), points.end());
			for (const DotClip& clip : dot_clips)
			{
				if (!clip.enabled)
					continue;
				ClipPolygon([&](Vector2f p) { return Dot(p - clip.point, clip.inside); }, 0.f, true);
				if (polygon.size() < 3)
					return;
			}
			const int first = (int)vertices.size();
			for (const PolygonPoint& p : polygon)
				AddVertex(p.position, Scale(color, p.alpha));
			for (int j = 1; j + 1 < (int)polygon.size(); j++)
				AddTriangle(first, first + j, first + j + 1);
		}

		// Keeps the part of the polygon whose length is at least (keep_above) or at most the limit.
		template <typename LengthFunction>
		void ClipPolygon(LengthFunction length_at, float limit, bool keep_above)
		{
			clipped.clear();
			const size_t count = polygon.size();
			for (size_t i = 0; i < count; i++)
			{
				const PolygonPoint p = polygon[i], q = polygon[(i + 1) % count];
				const float dp = (keep_above ? length_at(p.position) - limit : limit - length_at(p.position));
				const float dq = (keep_above ? length_at(q.position) - limit : limit - length_at(q.position));
				if (dp >= 0.f)
					clipped.push_back(p);
				if ((dp >= 0.f) != (dq >= 0.f))
				{
					const float t = dp / (dp - dq);
					clipped.push_back({Math::Lerp(t, p.position, q.position), Math::Lerp(t, p.alpha, q.alpha)});
				}
			}
			polygon.swap(clipped);
		}

		Vector<Vertex>& vertices;
		Vector<int>& indices;
		bool feather;        // Straight edges (and the cut ends of dashes) are feathered.
		bool feather_curves; // Curves (rounded corners, round dots) are feathered.
		float unit;
		Vector<PolygonPoint> polygon, clipped;
		Vector<OutlinePoint> outline;
		struct DotClip {
			bool enabled = false;
			Vector2f point, inside;
		};
		DotClip dot_clips[2];
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

namespace {
	// Chromium's Color::Dark() and Color::Light(): the channels scaled by (v - 0.33) / v and min(1, v + 0.33) / v, v being
	// the largest channel, truncated to 8 bits; black lightens to a fixed grey.
	Colourb ChromiumDark(Colourb color)
	{
		const float v = float(Math::Max(color.red, Math::Max(color.green, color.blue))) / 255.f;
		if (v == 0.f)
			return Colourb(0, 0, 0, color.alpha);
		const float multiplier = Math::Max(0.f, (v - 0.33f) / v);
		auto scale = [&](byte c) { return byte(Math::Clamp(int(multiplier * (float(c) / 255.f) * 255.99998f), 0, 255)); };
		return Colourb(scale(color.red), scale(color.green), scale(color.blue), color.alpha);
	}
	Colourb ChromiumLight(Colourb color)
	{
		const float v = float(Math::Max(color.red, Math::Max(color.green, color.blue))) / 255.f;
		if (v == 0.f)
			return Colourb(0x54, 0x54, 0x54, color.alpha);
		const float multiplier = Math::Min(1.f, v + 0.33f) / v;
		auto scale = [&](byte c) { return byte(Math::Clamp(int(multiplier * (float(c) / 255.f) * 255.99998f), 0, 255)); };
		return Colourb(scale(color.red), scale(color.green), scale(color.blue), color.alpha);
	}
	float Luminance(Colourb color)
	{
		auto linear = [](byte c) {
			const float value = float(c) / 255.f;
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		};
		return 0.2126f * linear(color.red) + 0.7152f * linear(color.green) + 0.0722f * linear(color.blue);
	}

	// The dark and light colours of a 3D border (Chromium's ObjectPainter: a colour darker than #202020 is lightened
	// for both parts, one lighter than #ebebeb is its own light part).
	void ShadedColors(ColourbPremultiplied premultiplied, ColourbPremultiplied& dark, ColourbPremultiplied& light)
	{
		const Colourb color = Unpremultiplied(premultiplied);
		const float luminance = Luminance(color);
		Colourb dark_color, light_color;
		if (luminance < 0.0148f)
		{
			dark_color = ChromiumLight(color);
			light_color = ChromiumLight(dark_color);
		}
		else if (luminance > 0.8331f)
		{
			dark_color = ChromiumDark(color);
			light_color = color;
		}
		else
		{
			dark_color = ChromiumDark(color);
			light_color = ChromiumLight(color);
		}
		dark = dark_color.ToPremultiplied();
		light = light_color.ToPremultiplied();
	}
} // namespace

ColourbPremultiplied GeometryBorderStyles::DarkColor(ColourbPremultiplied color)
{
	ColourbPremultiplied dark, light;
	ShadedColors(color, dark, light);
	return dark;
}

ColourbPremultiplied GeometryBorderStyles::LightColor(ColourbPremultiplied color)
{
	ColourbPremultiplied dark, light;
	ShadedColors(color, dark, light);
	return light;
}

RenderBox GeometryBorderStyles::BackgroundRenderBox(const RenderBox& border_box, const RenderBox& padding_box, const ColourbPremultiplied border_colors[4])
{
	const EdgeSizes widths = padding_box.GetBorderWidths();
	const BorderStyles styles = padding_box.GetBorderStyles();
	EdgeSizes inset = {};
	for (int side = 0; side < 4; side++)
	{
		const bool gaps = (styles[side] == BorderStyle::Dotted || styles[side] == BorderStyle::Dashed || styles[side] == BorderStyle::Double);
		if (widths[side] >= 1.f && border_colors[side].alpha == 255 && Style::IsBorderStylePainted(styles[side]) && !gaps)
			inset[side] = 1.f;
	}
	// The border box as the outer box of the insets: the fill is their inner area, its radii reduced by them.
	RenderBox box = border_box;
	box.SetBorderWidths(inset);
	box.SetFillSize(Math::Max(border_box.GetFillSize() - Vector2f(inset[LEFT] + inset[RIGHT], inset[TOP] + inset[BOTTOM]), Vector2f(0.f)));
	return box;
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

namespace {
	// Chromium's StrokeData::SelectBestDashGap: the number of dashes whose gap, stretched or shrunk so that the dashes
	// fill the stroke (with a dash at each end of an open one), is closest to the nominal gap.
	float SelectBestDashGap(float stroke_length, float dash_length, float gap_length, bool closed_path, int& num_dashes)
	{
		const float available = (closed_path ? stroke_length : stroke_length + gap_length);
		const float min_num_dashes = Math::Max(1.f, std::floor(available / (dash_length + gap_length)));
		const float max_num_dashes = min_num_dashes + 1.f;
		const float min_num_gaps = (closed_path ? min_num_dashes : min_num_dashes - 1.f);
		const float max_num_gaps = (closed_path ? max_num_dashes : max_num_dashes - 1.f);
		const float min_gap = (min_num_gaps > 0.f ? (stroke_length - min_num_dashes * dash_length) / min_num_gaps : FLT_MAX);
		const float max_gap = (stroke_length - max_num_dashes * dash_length) / max_num_gaps;
		if (max_gap <= 0.f || Math::Absolute(min_gap - gap_length) < Math::Absolute(max_gap - gap_length))
		{
			num_dashes = int(min_num_dashes);
			return min_gap;
		}
		num_dashes = int(max_num_dashes);
		return max_gap;
	}

	// The dash and gap of a dashed border (Chromium: 3w and 2w below 3px, 2w and w from 3px).
	void DashPattern(float width, float& dash, float& space)
	{
		dash = (width < 3.f ? 3.f : 2.f) * width;
		space = (width < 3.f ? 2.f : 1.f) * width;
	}

	// The dashes of a stroke of the given length (Chromium's StrokeData::SetupPaintDashPathEffect): begin and length of
	// each, empty for a solid stroke.
	void DashesOfStroke(float length, float dash, float space, bool closed_path, Vector<Interval>& dashes)
	{
		dashes.clear();
		if (length <= 2.f * dash)
			return;
		float period = 0.f;
		int num_dashes = 0;
		if (length <= 2.f * dash + space && !closed_path)
		{
			// Two dashes, proportionally sized.
			const float scale = length / (2.f * dash + space);
			dash *= scale;
			space *= scale;
			num_dashes = 2;
			period = dash + space;
		}
		else
		{
			const float gap = SelectBestDashGap(length, dash, space, closed_path, num_dashes);
			period = dash + gap;
		}
		for (int i = 0; i < num_dashes; i++)
			dashes.push_back({float(i) * period, float(i) * period + dash});
	}
} // namespace

void GeometryBorderStyles::DrawBackground(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, ColourbPremultiplied color,
	FeatherEdges edges, const EdgeSizes* under_border, float feather_unit)
{
	const float straight_half = (edges == FeatherEdges::All ? feather_half * feather_unit : 0.f);
	const float curve_half = (edges != FeatherEdges::None ? feather_half_curve * feather_unit : 0.f);

	// The inner area of the metrics, its corners rounded by the inner radii. Under a border it reaches half a pixel
	// below the border's inner edge (where that side has a border), so that no gap opens between the two.
	using OutlinePoint = BorderWriter::OutlinePoint;
	Vector<OutlinePoint> outline;
	for (int corner = 0; corner < 4; corner++)
	{
		Vector2f under(0.f);
		if (under_border)
		{
			const int vertical_side = (corner == 0 || corner == 3 ? LEFT : RIGHT), horizontal_side = (corner <= 1 ? TOP : BOTTOM);
			under.x = ((*under_border)[vertical_side] > 0.f ? 0.5f * feather_unit : 0.f);
			under.y = ((*under_border)[horizontal_side] > 0.f ? 0.5f * feather_unit : 0.f);
		}
		const Vector2f r = metrics.inner_radii[corner];
		if (IsRounded(OuterRadius(metrics, corner)) && r.x > 0.f && r.y > 0.f)
		{
			const int num_segments = QuarterSegments(r / feather_unit);
			for (int i = 0; i <= num_segments; i++)
			{
				const float angle = Math::Lerp(float(i) / float(num_segments), StartAngle(corner), EndAngle(corner));
				const bool arc_end = (i == 0 || i == num_segments);
				const Vector2f outwards = -EllipseInwardNormal(r, angle);
				outline.push_back({ArcPoint(metrics.positions_circle_center[corner], r, angle) + outwards * under, arc_end ? straight_half : curve_half});
			}
		}
		else
		{
			const Vector2f outwards(corner == 0 || corner == 3 ? -1.f : 1.f, corner <= 1 ? -1.f : 1.f);
			outline.push_back({metrics.positions_inner[corner] + outwards * under, straight_half});
		}
	}
	BorderWriter writer(vertices, indices, edges, feather_unit);
	writer.Fill(outline, color);
}

void GeometryBorderStyles::DrawBorder(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, EdgeSizes edge_sizes,
	const ColourbPremultiplied colors[4], const BorderStyles& styles, Vector2f top_gap, FeatherEdges edges, bool feather_inner_edge, float feather_unit)
{
	float split[4];
	for (int corner = 0; corner < 4; corner++)
		split[corner] = SplitAngle(metrics, edge_sizes, corner);

	BorderWriter writer(vertices, indices, edges, feather_unit);
	SideRail rails[4];
	for (int side = 0; side < 4; side++)
		BuildRail(rails[side], metrics, side, split, feather_unit);

	// A box with a rounded corner lays its dashes and dots out along its whole centre line, a closed path starting on the
	// top edge where the top-left corner ends, clockwise (Chromium strokes each side with the box's centre path, clipped
	// to the side). Square boxes lay them out per side.
	bool rounded = false;
	for (int corner = 0; corner < 4; corner++)
		rounded |= IsRounded(OuterRadius(metrics, corner));
	float perimeter = 0.f;
	float side_start[4] = {};
	for (int side = 0; side < 4; side++)
	{
		side_start[side] = perimeter;
		perimeter += rails[side].pairs.back().s - rails[side].pairs.front().s;
	}
	const float origin = rails[TOP].pairs[rails[TOP].straight_begin].s - rails[TOP].pairs.front().s;

	// The part of [begin, end] (lengths along the closed centre path from its origin) on a side, in the side's lengths.
	auto add_path_interval = [&](Vector<Interval>& out, int side, float begin, float end, Interval gap) {
		const float side_length = rails[side].pairs.back().s - rails[side].pairs.front().s;
		const float offset = rails[side].pairs.front().s;
		float local = std::fmod(origin + begin - side_start[side], perimeter);
		if (local < 0.f)
			local += perimeter;
		for (float shift : {0.f, -perimeter})
		{
			const float a = Math::Max(local + shift, 0.f), b = Math::Min(local + shift + (end - begin), side_length);
			if (b <= a)
				continue;
			// A part that reaches the side's start or end covers its corner up to the outer corner.
			const float s0 = (a <= 0.001f ? Math::Min(offset, 0.f) : offset + a);
			const float s1 = (b >= side_length - 0.001f ? Math::Max(offset + side_length, rails[side].length) : offset + b);
			AddInterval(out, s0, s1, gap);
		}
	};

	Vector<Interval> intervals, dashes;

	for (int side = 0; side < 4; side++)
	{
		const float width = edge_sizes[side];
		const ColourbPremultiplied color = colors[side];
		BorderStyle style = styles[side];
		if (width <= 0.f || color.alpha == 0 || !Style::IsBorderStylePainted(style))
			continue;

		const SideRail& rail = rails[side];
		const float length = rail.length;

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
			const RailPair& a = rail.pairs[rail.straight_begin];
			gap = {a.s + (x0 - a.centre.x), a.s + (x1 - a.centre.x)};
		}
		auto in_gap = [&](Vector2f centre, float extent) {
			if (side != TOP || gap.end <= gap.begin)
				return false;
			const float x = centre.x - metrics.positions_outer[0].x;
			return x + extent > top_gap.x && x - extent < top_gap.y;
		};

		if (style == BorderStyle::Double && width < 3.f)
			style = BorderStyle::Solid;

		// Dots and end squares near a square corner stay on this side of the corner split when the side next to it has
		// another colour or style (Chromium clips each side to its polygon then).
		for (int end = 0; end < 2; end++)
		{
			const int corner = (end == 0 ? side : (side + 1) % 4);
			const int adjacent = (end == 0 ? (side + 3) % 4 : (side + 1) % 4);
			const bool differs = edge_sizes[adjacent] > 0.f && colors[adjacent].alpha > 0 && Style::IsBorderStylePainted(styles[adjacent]) &&
				(colors[adjacent] != color || styles[adjacent] != styles[side]);
			if (!differs || IsRounded(OuterRadius(metrics, corner)))
			{
				writer.SetDotClip(end, false);
				continue;
			}
			const Vector2f outer_corner = metrics.positions_outer[corner];
			const Vector2f diagonal = metrics.positions_inner[corner] - outer_corner;
			Vector2f inside(diagonal.y, -diagonal.x);
			if (Dot(inside, end == 0 ? side_direction[side] : -side_direction[side]) < 0.f)
				inside = -inside;
			writer.SetDotClip(end, true, outer_corner, inside);
		}

		switch (style)
		{
		case BorderStyle::Dashed:
		{
			float dash = 0.f, space = 0.f;
			DashPattern(width, dash, space);
			intervals.clear();
			if (rounded)
			{
				DashesOfStroke(perimeter, dash, space, true, dashes);
				if (dashes.empty())
					add_path_interval(intervals, side, 0.f, perimeter, gap);
				for (const Interval& interval : dashes)
					add_path_interval(intervals, side, interval.begin, interval.end, gap);
			}
			else
			{
				DashesOfStroke(length, dash, space, false, dashes);
				if (dashes.empty())
					AddInterval(intervals, 0.f, length, gap);
				for (const Interval& interval : dashes)
					AddInterval(intervals, interval.begin, interval.end, gap);
			}
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, color, true, feather_inner_edge);
			if (!rounded && !dashes.empty())
			{
				// Chromium fills a square of the side's width at each end of a straight dashed side, over the corner.
				const Vector2f direction = side_direction[side], normal = side_normal[side];
				const Vector2f start = metrics.positions_outer[side];
				const float outer_length = Dot(metrics.positions_outer[(side + 1) % 4] - start, direction);
				for (float at : {0.f, outer_length - width})
				{
					const Vector2f centre = start + direction * (at + 0.5f * width) + normal * (0.5f * width);
					if (!in_gap(centre, 0.5f * width))
						writer.AddDot(centre, width, color, true);
				}
			}
			break;
		}
		case BorderStyle::Dotted:
		{
			if (rounded)
			{
				// Round dots one width apart or a little more, the first at the path's origin.
				int num_dots = 0;
				const float gap_length = SelectBestDashGap(perimeter, width, width, true, num_dots);
				const float period = gap_length + width;
				const float side_length = rail.pairs.back().s - rail.pairs.front().s;
				for (int i = 0; i < num_dots; i++)
				{
					float local = std::fmod(origin + float(i) * period - side_start[side], perimeter);
					if (local < 0.f)
						local += perimeter;
					if (local >= side_length)
						continue;
					const Vector2f centre = PairAt(rail.pairs, rail.pairs.front().s + local).centre;
					if (!in_gap(centre, 0.5f * width))
						writer.AddDot(centre, width, color);
				}
			}
			else if (width < 3.f)
			{
				// Thin dots are squares (Chromium's straight dotted line): a square at each end of the side, and squares
				// one width apart between them, the pattern centred and aligned to whole pixels.
				const int c0 = side, c1 = (side + 1) % 4;
				const Vector2f direction = side_direction[side], normal = side_normal[side];
				const Vector2f start = metrics.positions_outer[c0];
				const float outer_length = Dot(metrics.positions_outer[c1] - start, direction);
				auto square = [&](float at) {
					const Vector2f corner = start + direction * at;
					const Vector2f centre = corner + direction * (0.5f * width) + normal * (0.5f * width);
					if (!in_gap(centre, 0.5f * width))
						writer.AddDot(centre, width, color, true);
				};
				square(0.f);
				if (outer_length > width)
					square(outer_length - width);
				const float inner_length = outer_length - 2.f * width;
				if (inner_length >= width)
				{
					const int num_dots = int(std::floor((inner_length + width) / (2.f * width)));
					const float leftover = inner_length - float(2 * num_dots - 1) * width;
					const float offset = std::floor(0.5f * leftover);
					for (int i = 0; i < num_dots; i++)
						square(width + offset + 2.f * width * float(i));
				}
			}
			else
			{
				// Round dots of the side's width, about two widths apart, one centred on each corner split (the first and
				// last pair of the rail), along the side's centre line.
				const Rail& path = rail.pairs;
				const float path_start = path.front().s;
				const float path_length = path.back().s - path_start;
				const int num_spaces = Math::Max(1, Math::RoundToInteger(path_length / (2.f * width)));
				for (int i = 0; i <= num_spaces; i++)
				{
					const float s = path_start + path_length * float(i) / float(num_spaces);
					const Vector2f centre = PairAt(path, s).centre;
					if (!in_gap(centre, 0.5f * width))
						writer.AddDot(centre, width, color);
				}
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
				writer.Strip(rail, interval.begin, interval.end, 1.f - band, 1.f, color, true, feather_inner_edge);
			}
			break;
		}
		case BorderStyle::Groove:
		case BorderStyle::Ridge:
		{
			// Groove: the top and left sides dark outside and light inside, the bottom and right ones the other way round.
			const bool outer_dark = (IsTopOrLeft(side) == (style == BorderStyle::Groove));
			ColourbPremultiplied dark, light;
			ShadedColors(color, dark, light);
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
			{
				writer.Strip(rail, interval.begin, interval.end, 0.f, 0.5f, outer_dark ? dark : light, true, false);
				writer.Strip(rail, interval.begin, interval.end, 0.5f, 1.f, outer_dark ? light : dark, false, feather_inner_edge);
			}
			break;
		}
		case BorderStyle::Inset:
		case BorderStyle::Outset:
		{
			ColourbPremultiplied dark, light;
			ShadedColors(color, dark, light);
			const bool dark_side = (IsTopOrLeft(side) == (style == BorderStyle::Inset));
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, dark_side ? dark : light, true, feather_inner_edge);
			break;
		}
		default:
		{
			intervals.clear();
			AddInterval(intervals, 0.f, length, gap);
			for (const Interval& interval : intervals)
				writer.Strip(rail, interval.begin, interval.end, 0.f, 1.f, color, true, feather_inner_edge);
			break;
		}
		}
	}
}

} // namespace Rml
