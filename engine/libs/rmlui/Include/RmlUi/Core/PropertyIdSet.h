#pragma once

#include "ID.h"
#include "Types.h"
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
	#include <intrin.h>
#endif

namespace Rml {

class PropertyIdSetIterator;

/*
    PropertyIdSet is a 'set'-like container for PropertyIds.

    Implemented as a fixed bit set of 64-bit words. It is cheap to construct and use, requiring no dynamic allocation.

    Supports union and intersection operations between two sets, as well as iteration through the IDs that are inserted.
    Iteration skips empty words, so its cost follows the number of inserted IDs rather than the number of possible IDs.
*/

class RMLUICORE_API PropertyIdSet {
private:
	static constexpr size_t N = size_t(PropertyId::MaxNumIds);
	static constexpr size_t NumWords = (N + 63) / 64;
	uint64_t words[NumWords] = {};

	static uint64_t Bit(size_t index) { return uint64_t(1) << (index % 64); }

public:
	PropertyIdSet() { static_assert((size_t)PropertyId::Invalid == 0, "PropertyIdSet makes an assumption that PropertyId::Invalid is zero."); }

	void Insert(PropertyId id)
	{
		RMLUI_ASSERT(size_t(id) < N);
		words[size_t(id) / 64] |= Bit(size_t(id));
	}

	void Clear()
	{
		for (uint64_t& word : words)
			word = 0;
	}
	void Erase(PropertyId id)
	{
		RMLUI_ASSERT(size_t(id) < N);
		words[size_t(id) / 64] &= ~Bit(size_t(id));
	}

	bool Empty() const
	{
		for (uint64_t word : words)
			if (word)
				return false;
		return true;
	}
	bool Contains(PropertyId id) const { return size_t(id) < N && (words[size_t(id) / 64] & Bit(size_t(id))) != 0; }

	size_t Size() const
	{
		size_t count = 0;
		for (uint64_t word : words)
			count += PopCount(word);
		return count;
	}

	// Union with another set
	PropertyIdSet& operator|=(const PropertyIdSet& other)
	{
		for (size_t i = 0; i < NumWords; i++)
			words[i] |= other.words[i];
		return *this;
	}
	PropertyIdSet operator|(const PropertyIdSet& other) const
	{
		PropertyIdSet result = *this;
		result |= other;
		return result;
	}

	// Intersection with another set
	PropertyIdSet& operator&=(const PropertyIdSet& other)
	{
		for (size_t i = 0; i < NumWords; i++)
			words[i] &= other.words[i];
		return *this;
	}
	PropertyIdSet operator&(const PropertyIdSet& other) const
	{
		PropertyIdSet result = *this;
		result &= other;
		return result;
	}

	// Iterator support. Iterates through all the PropertyIds that are set (contained).
	// @note: Only const_iterators are provided.
	inline PropertyIdSetIterator begin() const;
	inline PropertyIdSetIterator end() const;

	// Erases the property id represented by a valid iterator. Iterator must be in the range [begin, end).
	// @return A new iterator pointing to the next element or end().
	inline PropertyIdSetIterator Erase(PropertyIdSetIterator it);

private:
	// The index of the first contained id at or after 'index', or N if there is none.
	size_t NextFrom(size_t index) const
	{
		size_t word_index = index / 64;
		if (word_index >= NumWords)
			return N;
		uint64_t word = words[word_index] & (~uint64_t(0) << (index % 64));
		while (true)
		{
			if (word)
				return word_index * 64 + CountTrailingZeros(word);
			if (++word_index >= NumWords)
				return N;
			word = words[word_index];
		}
	}

	static size_t CountTrailingZeros(uint64_t word)
	{
		RMLUI_ASSERT(word != 0);
#if defined(__GNUC__) || defined(__clang__)
		return (size_t)__builtin_ctzll(word);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
		unsigned long index = 0;
		_BitScanForward64(&index, word);
		return (size_t)index;
#else
		size_t count = 0;
		while ((word & 1) == 0)
		{
			word >>= 1;
			count++;
		}
		return count;
#endif
	}

	static size_t PopCount(uint64_t word)
	{
#if defined(__GNUC__) || defined(__clang__)
		return (size_t)__builtin_popcountll(word);
#else
		size_t count = 0;
		for (; word; word &= word - 1)
			count++;
		return count;
#endif
	}

	friend class PropertyIdSetIterator;
};

class RMLUICORE_API PropertyIdSetIterator {
public:
	PropertyIdSetIterator() = default;
	PropertyIdSetIterator(const PropertyIdSet* container, size_t id_index) : container(container), id_index(id_index) { ProceedToNextValid(); }

	PropertyIdSetIterator& operator++()
	{
		++id_index;
		ProceedToNextValid();
		return *this;
	}

	bool operator==(const PropertyIdSetIterator& other) const { return container == other.container && id_index == other.id_index; }
	bool operator!=(const PropertyIdSetIterator& other) const { return !(*this == other); }
	PropertyId operator*() const { return static_cast<PropertyId>(id_index); }

private:
	inline void ProceedToNextValid()
	{
		if (container && id_index < size_t(PropertyId::MaxNumIds))
			id_index = container->NextFrom(id_index);
	}

	const PropertyIdSet* container = nullptr;
	size_t id_index = 0;

	friend PropertyIdSetIterator PropertyIdSet::Erase(PropertyIdSetIterator);
};

PropertyIdSetIterator PropertyIdSet::begin() const
{
	if (Empty())
		return end();
	return PropertyIdSetIterator(this, 1);
}

PropertyIdSetIterator PropertyIdSet::end() const
{
	return PropertyIdSetIterator(this, N);
}

PropertyIdSetIterator PropertyIdSet::Erase(PropertyIdSetIterator it)
{
	RMLUI_ASSERT(it.container == this && it.id_index < N);
	words[it.id_index / 64] &= ~Bit(it.id_index);
	++it;
	return it;
}

} // namespace Rml
