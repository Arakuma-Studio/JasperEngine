// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// JasperEngine addition to Basis Universal's transcoder (README.jasper.md), #included by
// basisu_transcoder.cpp inside namespace basist when JASPER_BASISD_COMPUTE_DXT1_TABLES is 1.
//
// The ETC1S -> BC1 tables (basisu_transcoder_tables_dxt1_5.inc and _6.inc, 120 KB of the web
// transcoder) computed by basisu_transcoder_init() instead of compiled in. Each entry is the
// (lo, hi) endpoint pair of upstream's exhaustive search (create_etc1_to_dxt1_{5,6}_conversion_table,
// BASISD_WRITE_NEW_DXT1_TABLES: the least squared error over the range's selectors, the first pair
// in hi-major order on ties), found here in a window around the least-squares endpoints, or along
// the one palette entry a mapping uses. The two give the same tables entry for entry
// (TextureBasisTranscode.Etc1sToBc1TablesMatchUpstream), in milliseconds instead of seconds.

namespace jasper_dxt1
{
	// how far the search goes on each side of the least-squares endpoints (2 is exact; one more
	// for a least-squares solve rounded the other way)
	const int cWindow = 3;

	struct target
	{
		int m_t[4];
		uint32_t m_low, m_high;
		const uint8_t* m_map;
	};

	static inline void palette(const uint8_t* pExpand, uint32_t lo, uint32_t hi, int c[4])
	{
		c[0] = pExpand[lo];
		c[3] = pExpand[hi];
		c[1] = (c[0] * 2 + c[3]) / 3;
		c[2] = (c[3] * 2 + c[0]) / 3;
	}

	static inline uint32_t error_of(const target& t, const uint8_t* pExpand, uint32_t lo, uint32_t hi)
	{
		int c[4];
		palette(pExpand, lo, hi, c);
		uint32_t e = 0;
		for (uint32_t s = t.m_low; s <= t.m_high; s++)
		{
			const int d = t.m_t[s] - c[t.m_map[s]];
			e += (uint32_t)(d * d);
		}
		return e;
	}

	// for each value palette entry k takes: the first endpoint pair in the exhaustive search's
	// order (hi-major) that gives it, as hi * n + lo; cNone where no pair does
	struct first_pairs
	{
		static const uint32_t cNone = UINT32_MAX;
		uint32_t m_pair[4][256];
	};

	static void solve(const target& t, const uint8_t* pExpand, uint32_t n, const first_pairs& firsts, etc1_to_dxt1_56_solution& out)
	{
		const uint32_t k0 = t.m_map[t.m_low];
		bool one_entry = true;
		for (uint32_t s = t.m_low; s <= t.m_high; s++)
			one_entry = one_entry && t.m_map[s] == k0;

		uint32_t best_err = UINT32_MAX, best_lo = 0, best_hi = 0;
		if (one_entry)
		{
			// the error only depends on the value of that palette entry: of the values with the
			// least error, the one an earlier pair reaches
			uint32_t best_pair = first_pairs::cNone;
			for (int v = 0; v < 256; v++)
			{
				const uint32_t pair = firsts.m_pair[k0][v];
				if (pair == first_pairs::cNone)
					continue;
				uint32_t e = 0;
				for (uint32_t s = t.m_low; s <= t.m_high; s++)
					e += (uint32_t)((t.m_t[s] - v) * (t.m_t[s] - v));
				if (e < best_err || (e == best_err && pair < best_pair))
				{
					best_err = e;
					best_pair = pair;
				}
			}
			best_hi = best_pair / n;
			best_lo = best_pair % n;
		}
		else
		{
			// palette entry k is about ((3 - k) a + k b) / 3 of the expanded endpoints a (lo), b (hi):
			// the least-squares a, b, on the [0, 255] box
			double suu = 0, svv = 0, suv = 0, sut = 0, svt = 0;
			for (uint32_t s = t.m_low; s <= t.m_high; s++)
			{
				const double u = (3.0 - t.m_map[s]) / 3.0, v = t.m_map[s] / 3.0;
				suu += u * u;
				svv += v * v;
				suv += u * v;
				sut += u * t.m_t[s];
				svt += v * t.m_t[s];
			}
			const double det = suu * svv - suv * suv;
			double a = (sut * svv - svt * suv) / det;
			double b = (suu * svt - suv * sut) / det;
			if (a < 0.0 || a > 255.0 || b < 0.0 || b > 255.0)
			{
				// one endpoint on its bound, the other solved for it: the best of the four
				double best = -1.0;
				for (int edge = 0; edge < 4; edge++)
				{
					double ea, eb;
					if (edge < 2)
					{
						ea = edge ? 255.0 : 0.0;
						eb = (svt - suv * ea) / svv;
					}
					else
					{
						eb = (edge == 3) ? 255.0 : 0.0;
						ea = (sut - suv * eb) / suu;
					}
					ea = basisu::clamp(ea, 0.0, 255.0);
					eb = basisu::clamp(eb, 0.0, 255.0);
					double e = 0.0;
					for (uint32_t s = t.m_low; s <= t.m_high; s++)
					{
						const double d = t.m_t[s] - ((3.0 - t.m_map[s]) * ea + t.m_map[s] * eb) / 3.0;
						e += d * d;
					}
					if (best < 0.0 || e < best)
					{
						best = e;
						a = ea;
						b = eb;
					}
				}
			}
			const int center_lo = (int)(a * (n - 1) / 255.0 + 0.5), center_hi = (int)(b * (n - 1) / 255.0 + 0.5);
			const int lo0 = basisu::maximum(0, center_lo - cWindow), lo1 = basisu::minimum((int)n - 1, center_lo + cWindow);
			const int hi0 = basisu::maximum(0, center_hi - cWindow), hi1 = basisu::minimum((int)n - 1, center_hi + cWindow);
			for (int hi = hi0; hi <= hi1; hi++)
				for (int lo = lo0; lo <= lo1; lo++)
				{
					const uint32_t e = error_of(t, pExpand, (uint32_t)lo, (uint32_t)hi);
					if (e < best_err)
					{
						best_err = e;
						best_lo = (uint32_t)lo;
						best_hi = (uint32_t)hi;
					}
				}
		}
		assert(best_err <= 0xFFFF);
		out.m_lo = (uint8_t)best_lo;
		out.m_hi = (uint8_t)best_hi;
		out.m_err = (uint16_t)best_err;
	}

	// [inten][g][selector range][mapping], the layout of the .inc files
	static void compute(etc1_to_dxt1_56_solution* pTable, uint32_t bits)
	{
		const uint32_t n = 1U << bits;
		uint8_t expand[64];
		for (uint32_t i = 0; i < n; i++)
			expand[i] = (uint8_t)((bits == 5) ? ((i << 3) | (i >> 2)) : ((i << 2) | (i >> 4)));

		first_pairs firsts;
		for (uint32_t k = 0; k < 4; k++)
			for (uint32_t v = 0; v < 256; v++)
				firsts.m_pair[k][v] = first_pairs::cNone;
		for (uint32_t hi = 0; hi < n; hi++)
			for (uint32_t lo = 0; lo < n; lo++)
			{
				int c[4];
				palette(expand, lo, hi, c);
				for (uint32_t k = 0; k < 4; k++)
					if (firsts.m_pair[k][c[k]] == first_pairs::cNone)
						firsts.m_pair[k][c[k]] = hi * n + lo;
			}

		uint32_t index = 0;
		for (uint32_t inten = 0; inten < 8; inten++)
		{
			for (uint32_t g = 0; g < 32; g++)
			{
				const int base = (int)((g << 3) | (g >> 2));
				target t;
				for (uint32_t s = 0; s < 4; s++)
					t.m_t[s] = basisu::clamp(base + g_etc1_inten_tables[inten][s], 0, 255);
				for (uint32_t sr = 0; sr < NUM_ETC1_TO_DXT1_SELECTOR_RANGES; sr++)
				{
					t.m_low = g_etc1_to_dxt1_selector_ranges[sr].m_low;
					t.m_high = g_etc1_to_dxt1_selector_ranges[sr].m_high;
					for (uint32_t m = 0; m < NUM_ETC1_TO_DXT1_SELECTOR_MAPPINGS; m++)
					{
						t.m_map = g_etc1_to_dxt1_selector_mappings[m];
						solve(t, expand, n, firsts, pTable[index++]);
					}
				}
			}
		}
	}
} // namespace jasper_dxt1

static void jasper_compute_etc1_to_dxt1_tables()
{
	jasper_dxt1::compute(g_etc1_to_dxt_5, 5);
	jasper_dxt1::compute(g_etc1_to_dxt_6, 6);
}

const void* jasper_etc1_to_dxt1_table(uint32_t bits, uint32_t& size_in_bytes)
{
	size_in_bytes = (uint32_t)sizeof(g_etc1_to_dxt_5);
	return bits == 5 ? (const void*)g_etc1_to_dxt_5 : (const void*)g_etc1_to_dxt_6;
}
