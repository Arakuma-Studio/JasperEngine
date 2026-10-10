#pragma once

#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/*
    The values of the CSS Grid properties (CSS Grid Layout 2, phase 3: docs/html-ui/phase-3-fork-patches.md 3.14.1). They
    are StyleValues: the specified value as written, lengths unresolved (layout resolves them with the element's context:
    em, rem, vw, dp; percentages and calc() against the grid container's content box).
*/

/// A <track-breadth>: a length or percentage (or calc()), 'auto', 'min-content', 'max-content', or a flexible length.
struct GridTrackBreadth {
	enum class Kind : uint8_t { Length, Auto, MinContent, MaxContent, Fr };

	Kind kind = Kind::Auto;
	LengthPercentageValue length; // Kind::Length
	float fr = 0.f;               // Kind::Fr

	bool operator==(const GridTrackBreadth& other) const;
	bool operator!=(const GridTrackBreadth& other) const { return !(*this == other); }
	String ToString() const;
	bool IsLength() const { return kind == Kind::Length; }
	/// True for a percentage, or a calc() expression with a percentage in it.
	bool UsesPercentage() const;
};

/// A <track-size>: one breadth, 'minmax(min, max)' or 'fit-content(limit)'.
struct GridTrackSize {
	enum class Form : uint8_t { Breadth, MinMax, FitContent };

	Form form = Form::Breadth;
	GridTrackBreadth min, max;          // Breadth: both the same. FitContent: 'auto', and 'max-content' as the maximum.
	LengthPercentageValue fit_content_limit; // Form::FitContent

	static GridTrackSize Auto() { return GridTrackSize(); }

	bool operator==(const GridTrackSize& other) const;
	bool operator!=(const GridTrackSize& other) const { return !(*this == other); }
	String ToString() const;
	/// True if the minimum or the maximum is a length or percentage (CSS: a <fixed-size>).
	bool HasFixedComponent() const;
	Units GetUnitDependencies() const;
};

/// A 'repeat()' of a track list.
struct GridTrackRepeat {
	enum class Count : uint8_t { Fixed, AutoFill, AutoFit };

	Count count = Count::Fixed;
	int repetitions = 1; // Count::Fixed
	Vector<GridTrackSize> tracks;
	Vector<StringList> line_names; // tracks.size() + 1 lists, the names before each track and after the last

	bool operator==(const GridTrackRepeat& other) const;
	String ToString() const;
	bool IsAutoRepeat() const { return count != Count::Fixed; }
};

/// One entry of a track list: a track size or a repeat().
struct GridTemplateComponent {
	bool is_repeat = false;
	GridTrackSize track;    // !is_repeat
	GridTrackRepeat repeat; // is_repeat

	bool operator==(const GridTemplateComponent& other) const;
};

/// 'grid-template-columns', 'grid-template-rows': 'none' or a track list with line names.
class GridTrackListValue final : public StyleValue {
public:
	bool none = true;
	Vector<GridTemplateComponent> components;
	Vector<StringList> line_names; // components.size() + 1 lists, the names before each component and after the last

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	Units GetUnitDependencies() const override;

	/// The repeat(auto-fill) or repeat(auto-fit) of the list, or nullptr.
	const GridTrackRepeat* GetAutoRepeat() const;
};

/// 'grid-auto-columns', 'grid-auto-rows': one or more track sizes.
class GridAutoTracksValue final : public StyleValue {
public:
	Vector<GridTrackSize> tracks;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	Units GetUnitDependencies() const override;
};

/// A named area of 'grid-template-areas', with its lines (1-based, end exclusive).
struct GridTemplateArea {
	String name;
	int row_start = 0, row_end = 0;
	int column_start = 0, column_end = 0;

	bool operator==(const GridTemplateArea& other) const;
};

/// 'grid-template-areas': 'none' or rows of cell names.
class GridTemplateAreasValue final : public StyleValue {
public:
	StringList rows; // each row as written, its cells separated by one space ("a a ." )
	int num_rows = 0, num_columns = 0;
	Vector<GridTemplateArea> areas;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
};

/// 'grid-row-start', 'grid-row-end', 'grid-column-start', 'grid-column-end': a <grid-line>.
class GridLineValue final : public StyleValue {
public:
	enum class Kind : uint8_t { Auto, Line, NamedLine, Span, NamedSpan };

	Kind kind = Kind::Auto;
	int index = 0; // Line: the line number (not 0). NamedLine: the nth line of that name (0: the first). Span, NamedSpan: the count.
	String name;   // NamedLine, NamedSpan

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	/// True for a lone <custom-ident> ('foo'): the value the omitted parts of the 'grid-row', 'grid-column' and 'grid-area'
	/// shorthands copy.
	bool IsCustomIdent() const { return kind == Kind::NamedLine && index == 0; }
};

/// Parses a <track-list> or <auto-track-list> (with 'none' when allowed) into a GridTrackListValue.
/// @param[in] allow_auto_repeat False for the explicit track list of the 'grid-template' areas form, which takes no repeat().
bool ParseGridTrackList(const String& value, GridTrackListValue& result, bool allow_none = true, bool allow_auto_repeat = true);
/// Parses one or more <track-size> values ('grid-auto-columns', 'grid-auto-rows').
bool ParseGridAutoTracks(const String& value, GridAutoTracksValue& result);
/// Parses the value of 'grid-template-areas': 'none' or one or more strings forming rectangular named areas.
bool ParseGridTemplateAreas(const StringList& strings, GridTemplateAreasValue& result);
/// Parses a <grid-line>.
bool ParseGridLine(const String& value, GridLineValue& result);

/// Registers the grid parsers ('grid_track_list', 'grid_auto_tracks', 'grid_template_areas', 'grid_line',
/// 'grid_auto_flow', 'gap').
void RegisterGridParsers();
/// Registers the grid properties and shorthands, and the parsers of the shorthands.
void RegisterGridProperties(PropertySpecification& specification);

} // namespace Rml
