#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/Math.h"
#include <cstring>

namespace Rml {

// The high ids of PropertyIdSet (fork, phase 5, FORK.md P5-4): the registered custom properties, bits of a heap array
// sized to the highest word a set holds.

void PropertyIdSet::InsertHigh(size_t index)
{
	const size_t word = (index - InlineIds) / 64;
	if (word >= num_high)
	{
		const uint32_t count = uint32_t(word + 1);
		uint64_t* grown = new uint64_t[count];
		for (uint32_t i = 0; i < count; i++)
			grown[i] = (i < num_high ? high[i] : 0);
		delete[] high;
		high = grown;
		num_high = count;
	}
	high[word] |= Bit(index);
	words[0] |= HighFlag;
}

void PropertyIdSet::EraseHigh(size_t index)
{
	const size_t word = (index - InlineIds) / 64;
	if (word < num_high)
		high[word] &= ~Bit(index);
	if (HighEmpty())
		words[0] &= ~HighFlag;
}

bool PropertyIdSet::HighEmpty() const
{
	uint64_t any = 0;
	for (uint32_t i = 0; i < num_high; i++)
		any |= high[i];
	return any == 0;
}

size_t PropertyIdSet::HighSize() const
{
	size_t count = 0;
	for (uint32_t i = 0; i < num_high; i++)
		count += PopCount(high[i]);
	return count;
}

void PropertyIdSet::CopyHigh(const PropertyIdSet& other)
{
	if (num_high != other.num_high)
	{
		delete[] high;
		high = new uint64_t[other.num_high];
		num_high = other.num_high;
	}
	std::memcpy(high, other.high, sizeof(uint64_t) * num_high);
}

void PropertyIdSet::OrHigh(const PropertyIdSet& other)
{
	if (num_high < other.num_high)
	{
		uint64_t* grown = new uint64_t[other.num_high];
		for (uint32_t i = 0; i < other.num_high; i++)
			grown[i] = (i < num_high ? high[i] : 0);
		delete[] high;
		high = grown;
		num_high = other.num_high;
	}
	for (uint32_t i = 0; i < other.num_high; i++)
		high[i] |= other.high[i];
}

void PropertyIdSet::AndHigh(const PropertyIdSet& other)
{
	// (called when this set had high ids; the inline flag is already the AND of both flags)
	for (uint32_t i = 0; i < num_high; i++)
		high[i] &= ((other.words[0] & HighFlag) && i < other.num_high ? other.high[i] : 0);
	if (HighEmpty())
		words[0] &= ~HighFlag;
}

void PropertyIdSet::HighOf(const PropertyIdSet& a, const PropertyIdSet& b, bool intersect)
{
	const bool a_high = (a.words[0] & HighFlag) != 0, b_high = (b.words[0] & HighFlag) != 0;
	const uint32_t count = intersect ? Math::Min(a.num_high, b.num_high) : Math::Max(a_high ? a.num_high : 0u, b_high ? b.num_high : 0u);
	if (count == 0)
	{
		words[0] &= ~HighFlag;
		return;
	}
	high = new uint64_t[count];
	num_high = count;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint64_t x = (a_high && i < a.num_high ? a.high[i] : 0);
		const uint64_t y = (b_high && i < b.num_high ? b.high[i] : 0);
		high[i] = (intersect ? (x & y) : (x | y));
	}
	if (HighEmpty())
		words[0] &= ~HighFlag;
}

bool PropertyIdSet::HighIntersects(const PropertyIdSet& other) const
{
	const uint32_t count = Math::Min(num_high, other.num_high);
	for (uint32_t i = 0; i < count; i++)
		if (high[i] & other.high[i])
			return true;
	return false;
}

void PropertyIdSet::ClearHigh()
{
	for (uint32_t i = 0; i < num_high; i++)
		high[i] = 0;
}

void PropertyIdSet::FreeHigh()
{
	delete[] high;
	high = nullptr;
	num_high = 0;
}

size_t PropertyIdSet::NextFromHigh(size_t index) const
{
	size_t word_index = (index - InlineIds) / 64;
	if (word_index >= num_high)
		return N;
	uint64_t word = high[word_index] & (~uint64_t(0) << (index % 64));
	while (true)
	{
		if (word)
			return InlineIds + word_index * 64 + CountTrailingZeros(word);
		if (++word_index >= num_high)
			return N;
		word = high[word_index];
	}
}

} // namespace Rml
