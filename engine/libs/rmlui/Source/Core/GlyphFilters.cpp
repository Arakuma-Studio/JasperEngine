#include "GlyphFilters.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Rml {
namespace GlyphFilters {

namespace {

	// The squared Euclidean distance of every sample to the nearest site of a column-wise transform: 'f' holds each sample's
	// squared distance to the nearest site of its column (unreached: none near enough), 'nearest' (when given) gets the
	// sample whose parabola is lowest: Felzenszwalb and Huttenlocher's lower envelope, in integers. The intersections are
	// kept as fractions (numerator, positive denominator) and compared by cross-multiplying: no division on the way.
	// v, zn and zd are scratch of n, n + 1 and n + 1 entries.
	void Envelope(const int* f, int n, int unreached, int* distance, int* nearest, int* v, int64_t* zn, int64_t* zd)
	{
		// z[k] = zn[k] / zd[k]; zd 0 is minus infinity (the first parabola's start)
		// a site inside a run of sites is nearest to itself only (outside the run, the run's ends are nearer): the envelope
		// takes the runs' ends, which keeps a glyph's inside rows from costing a parabola a pixel
		int k = -1;
		for (int q = 0; q < n; ++q)
		{
			if (f[q] >= unreached || (f[q] == 0 && q > 0 && q + 1 < n && f[q - 1] == 0 && f[q + 1] == 0))
				continue;
			const int64_t fq = (int64_t)f[q] + (int64_t)q * q;
			int64_t sn = 0, sd = 1;
			while (k >= 0)
			{
				const int p = v[k];
				sn = fq - ((int64_t)f[p] + (int64_t)p * p);
				sd = 2 * (int64_t)(q - p);
				// s > z[k]
				if (zd[k] == 0 || sn * zd[k] > zn[k] * sd)
					break;
				--k;
			}
			++k;
			v[k] = q;
			zn[k] = (k == 0) ? 0 : sn;
			zd[k] = (k == 0) ? 0 : sd;
		}
		if (k < 0)
		{
			for (int q = 0; q < n; ++q)
				distance[q] = unreached;
			if (nearest)
				for (int q = 0; q < n; ++q)
					nearest[q] = -1;
			return;
		}
		int j = 0;
		for (int q = 0; q < n; ++q)
		{
			// the next parabola starts at or before q: z[j + 1] <= q (the last one runs to the end)
			while (j < k && zn[j + 1] <= (int64_t)q * zd[j + 1])
				++j;
			const int p = f[q] == 0 ? q : v[j];
			distance[q] = (q - p) * (q - p) + f[p];
			if (nearest)
				nearest[q] = p;
		}
	}

	// The working memory of the transforms, kept by each thread between glyphs: a fresh buffer of a big glyph's size is
	// pages the kernel maps and zeroes again, which cost more than the transform itself
	struct Scratch {
		std::vector<int> rows, last, f, nearest, v;
		std::vector<int64_t> zn, zd;
		std::vector<int> full_distance, any_distance, any_site;
	};

	Scratch& ThreadScratch()
	{
		static thread_local Scratch scratch;
		return scratch;
	}

	// The exact squared distances of every pixel to the nearest fully covered pixel and to the nearest covered one, and that
	// one's index; distances beyond 'reach' are (reach + 1)^2 or more (nothing that far changes a dilation of reach - 1).
	// One pass down and one up the columns (a row at a time, the image's order) for both, then the rows' lower envelopes.
	void DistanceTransforms(const byte* alpha, Vector2i dimensions, int reach, Scratch& scratch)
	{
		const int width = dimensions.x, height = dimensions.y;
		const size_t area = (size_t)width * (size_t)height;
		const int unreached = (reach + 1) * (reach + 1);
		std::vector<int>& rows = scratch.rows;          // the covered pixel nearest in the column: its row
		std::vector<int>& last = scratch.last;          // per column: the nearest full and covered rows so far
		std::vector<int>& full = scratch.full_distance;
		std::vector<int>& any = scratch.any_distance;
		rows.resize(area);
		full.resize(area);
		any.resize(area);
		last.assign((size_t)width * 2, -1);
		int* fullAbove = last.data();
		int* anyAbove = last.data() + width;
		// down: the nearest above (kept in full/any as rows for now)
		for (int y = 0; y < height; ++y)
		{
			const byte* in = alpha + (size_t)y * (size_t)width;
			int* fullOut = full.data() + (size_t)y * (size_t)width;
			int* anyOut = any.data() + (size_t)y * (size_t)width;
			for (int x = 0; x < width; ++x)
			{
				fullAbove[x] = in[x] == 255 ? y : fullAbove[x];
				anyAbove[x] = in[x] != 0 ? y : anyAbove[x];
				fullOut[x] = fullAbove[x];
				anyOut[x] = anyAbove[x];
			}
		}
		// up: the nearer of above and below, as squared distances
		std::fill(last.begin(), last.end(), -1);
		int* fullBelow = last.data();
		int* anyBelow = last.data() + width;
		for (int y = height - 1; y >= 0; --y)
		{
			const byte* in = alpha + (size_t)y * (size_t)width;
			int* fullOut = full.data() + (size_t)y * (size_t)width;
			int* anyOut = any.data() + (size_t)y * (size_t)width;
			int* rowOut = rows.data() + (size_t)y * (size_t)width;
			for (int x = 0; x < width; ++x)
			{
				fullBelow[x] = in[x] == 255 ? y : fullBelow[x];
				anyBelow[x] = in[x] != 0 ? y : anyBelow[x];
				// (no site: -1, as far as it gets)
				const int fullUp = fullOut[x] < 0 ? unreached : y - fullOut[x];
				const int fullDown = fullBelow[x] < 0 ? unreached : fullBelow[x] - y;
				const int fullGap = fullUp <= fullDown ? fullUp : fullDown;
				fullOut[x] = fullGap > reach ? unreached : fullGap * fullGap;
				const int anyUp = anyOut[x] < 0 ? unreached : y - anyOut[x];
				const int anyDown = anyBelow[x] < 0 ? unreached : anyBelow[x] - y;
				const int anyGap = anyUp <= anyDown ? anyUp : anyDown;
				rowOut[x] = anyUp <= anyDown ? anyOut[x] : anyBelow[x];
				anyOut[x] = anyGap > reach ? unreached : anyGap * anyGap;
			}
		}

		// per row: the lower envelope of the columns' parabolas (the column distances are read whole first)
		std::vector<int>& site = scratch.any_site;
		site.resize(area);
		std::vector<int>& f = scratch.f;
		std::vector<int>& nearest = scratch.nearest;
		std::vector<int>& v = scratch.v;
		std::vector<int64_t>& zn = scratch.zn;
		std::vector<int64_t>& zd = scratch.zd;
		f.resize((size_t)width);
		nearest.resize((size_t)width);
		v.resize((size_t)width);
		zn.resize((size_t)width + 1);
		zd.resize((size_t)width + 1);
		for (int y = 0; y < height; ++y)
		{
			int* fullRow = full.data() + (size_t)y * (size_t)width;
			std::copy(fullRow, fullRow + width, f.begin());
			Envelope(f.data(), width, unreached, fullRow, nullptr, v.data(), zn.data(), zd.data());
			int* anyRow = any.data() + (size_t)y * (size_t)width;
			std::copy(anyRow, anyRow + width, f.begin());
			Envelope(f.data(), width, unreached, anyRow, nearest.data(), v.data(), zn.data(), zd.data());
			int* sites = site.data() + (size_t)y * (size_t)width;
			const int* columnRows = rows.data() + (size_t)y * (size_t)width;
			for (int x = 0; x < width; ++x)
			{
				const int column = nearest[(size_t)x];
				sites[x] = column < 0 ? -1 : columnRows[column] * width + column;
			}
		}
	}

	// FontEffectOutline's kernel weight at a distance
	float Weight(float distance, int radius)
	{
		if (distance <= float(radius))
			return 1.f;
		return Math::Max(float(radius + 1) - distance, 0.f);
	}

	// One axis of the blur over 'count' samples, read from 'in' (contiguous) and written to 'out' as bytes 'step' apart.
	void BlurLine(const byte* in, byte* out, int count, int step, int radius, double decay, double tail, double normal, std::vector<double>& causal)
	{
		// causal[n] = sum_{k=0..radius} decay^k in[n-k], anticausal = sum_{k=1..radius} decay^k in[n+k]
		double running = 0.0;
		for (int n = 0; n < count; ++n)
		{
			const int leaving = n - radius - 1;
			running = double(in[n]) + decay * running - (leaving >= 0 ? tail * double(in[leaving]) : 0.0);
			causal[(size_t)n] = running;
		}
		double anticausal = 0.0;
		for (int n = count - 1; n >= 0; --n)
		{
			if (n < count - 1)
			{
				const int leaving = n + radius + 1;
				anticausal = decay * (double(in[n + 1]) + anticausal) - (leaving < count ? tail * double(in[leaving]) : 0.0);
			}
			// (the dense filter summed floats and truncated; a whole value must not come out one below)
			const double value = (causal[(size_t)n] + anticausal) * normal + 1e-3;
			out[n * step] = byte(Math::Clamp(value, 0.0, 255.0));
		}
	}

} // namespace

void Place(byte* destination, Vector2i dimensions, const byte* source, Vector2i source_dimensions, Vector2i offset, ColorFormat source_format)
{
	const int bytes_per_pixel = (source_format == ColorFormat::RGBA8 ? 4 : 1);
	const int alpha_offset = (source_format == ColorFormat::RGBA8 ? 3 : 0);
	for (int i = 0; i < dimensions.x * dimensions.y; ++i)
		destination[i] = 0;
	if (!source)
		return;
	for (int y = 0; y < source_dimensions.y; ++y)
	{
		const int dy = y + offset.y;
		if (dy < 0 || dy >= dimensions.y)
			continue;
		for (int x = 0; x < source_dimensions.x; ++x)
		{
			const int dx = x + offset.x;
			if (dx < 0 || dx >= dimensions.x)
				continue;
			destination[dy * dimensions.x + dx] = source[(y * source_dimensions.x + x) * bytes_per_pixel + alpha_offset];
		}
	}
}

void Dilate(byte* destination, const byte* source, Vector2i dimensions, int radius)
{
	RMLUI_ZoneScopedNC("GlyphFilters::Dilate", 0xd6bf49);
	const int width = dimensions.x, height = dimensions.y;
	const size_t area = (size_t)width * (size_t)height;
	if (area == 0)
		return;
	if (radius <= 0)
	{
		for (size_t i = 0; i < area; ++i)
			destination[i] = source[i];
		return;
	}

	// a pixel farther than radius + 1 from every covered one stays empty
	Scratch& scratch = ThreadScratch();
	DistanceTransforms(source, dimensions, radius + 1, scratch);
	const std::vector<int>& full_distance = scratch.full_distance;
	const std::vector<int>& any_distance = scratch.any_distance;
	const std::vector<int>& any_site = scratch.any_site;

	const int inner = radius * radius;
	const int outer = (radius + 1) * (radius + 1);
	constexpr int around = 2;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const size_t i = (size_t)y * (size_t)width + (size_t)x;
			const int full = full_distance[i];
			if (full <= inner)
			{
				destination[i] = 255;
				continue;
			}
			float value = (full < outer) ? 255.f * Weight(std::sqrt(float(full)), radius) : 0.f;
			if (any_distance[i] < outer && any_site[i] >= 0)
			{
				// partly covered pixels: the nearest one and those around it
				const int sx = any_site[i] % width, sy = any_site[i] / width;
				for (int qy = Math::Max(sy - around, 0); qy <= Math::Min(sy + around, height - 1); ++qy)
				{
					for (int qx = Math::Max(sx - around, 0); qx <= Math::Min(sx + around, width - 1); ++qx)
					{
						const byte coverage = source[(size_t)qy * (size_t)width + (size_t)qx];
						if (coverage == 0 || float(coverage) <= value)
							continue;
						const float dx = float(x - qx), dy = float(y - qy);
						value = Math::Max(value, float(coverage) * Weight(std::sqrt(dx * dx + dy * dy), radius));
					}
				}
			}
			destination[i] = byte(Math::Min(value, 255.f));
		}
	}
}

void Blur(byte* buffer, Vector2i dimensions, int radius, float std_dev)
{
	RMLUI_ZoneScopedNC("GlyphFilters::Blur", 0xd6bf49);
	const int width = dimensions.x, height = dimensions.y;
	if (radius <= 0 || width <= 0 || height <= 0)
		return;
	const double decay = std::exp(-1.0 / (std::sqrt(2.0) * double(std_dev)));
	double sum = 1.0, power = 1.0;
	for (int k = 1; k <= radius; ++k)
	{
		power *= decay;
		sum += 2.0 * power;
	}
	const double tail = power * decay;
	const double normal = 1.0 / sum;

	std::vector<double> causal((size_t)Math::Max(width, height));
	std::vector<byte> line((size_t)Math::Max(width, height));
	for (int y = 0; y < height; ++y)
	{
		byte* row = buffer + (size_t)y * (size_t)width;
		for (int x = 0; x < width; ++x)
			line[(size_t)x] = row[x];
		BlurLine(line.data(), row, width, 1, radius, decay, tail, normal, causal);
	}
	for (int x = 0; x < width; ++x)
	{
		for (int y = 0; y < height; ++y)
			line[(size_t)y] = buffer[(size_t)y * (size_t)width + (size_t)x];
		BlurLine(line.data(), buffer + x, height, width, radius, decay, tail, normal, causal);
	}
}

void WriteWhite(byte* destination, Vector2i dimensions, int destination_stride, const byte* alpha)
{
	for (int y = 0; y < dimensions.y; ++y)
	{
		byte* row = destination + y * destination_stride;
		for (int x = 0; x < dimensions.x; ++x)
		{
			const byte value = alpha[y * dimensions.x + x];
			row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = value;
		}
	}
}

} // namespace GlyphFilters
} // namespace Rml
