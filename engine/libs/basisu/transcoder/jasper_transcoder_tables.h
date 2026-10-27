// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// JasperEngine addition to Basis Universal's transcoder (README.jasper.md): the tables a
// transcoder built with JASPER_BASISD_COMPUTE_DXT1_TABLES computes at basisu_transcoder_init(),
// for the test that compares them with upstream's compiled-in ones.

#pragma once

#include <cstdint>

namespace basist
{
	// the ETC1S -> BC1 table of 5-bit (red, blue) or 6-bit (green) endpoints: 15360 entries of
	// {uint8_t lo, uint8_t hi, uint16_t err}, the layout of basisu_transcoder_tables_dxt1_{5,6}.inc
	const void* jasper_etc1_to_dxt1_table(uint32_t bits, uint32_t& size_in_bytes);
}
