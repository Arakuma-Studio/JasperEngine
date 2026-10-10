set(TARGET_NAME "rmlui_unit_tests")

add_executable(${TARGET_NAME}
	Alignment.cpp
	Animation.cpp
	AnimationLayers.cpp
	Calculation.cpp
	CalculationTypes.cpp
	Colors.cpp
	ContainingBlockOverride.cpp
	Core.cpp
	CustomProperties.cpp
	DataBinding.cpp
	DataExpression.cpp
	DataModel.cpp
	Debugger.cpp
	Decorator.cpp
	Element.cpp
	ElementBackgroundBorder.cpp
	ElementDocument.cpp
	ElementHandle.cpp
	ElementFormControlInput.cpp
	ElementFormControlSelect.cpp
	ElementImage.cpp
	ElementStyle.cpp
	EventListener.cpp
	Filter.cpp
	FlexFormatting.cpp
	InitialValues.cpp
	IntrinsicSizing.cpp
	Layout.cpp
	LayoutChildren.cpp
	Localization.cpp
	main.cpp
	Math.cpp
	MediaQuery.cpp
	Navigation.cpp
	Properties.cpp
	RootFontSize.cpp
	PropertySpecification.cpp
	Selectors.cpp
	ShorthandTypes.cpp
	Specificity_Basic.cpp
	Specificity_MediaQuery.cpp
	StableVector.cpp
	StringUtilities.cpp
	StyleSheetParser.cpp
	StyleValue.cpp
	Template.cpp
	URL.cpp
	Values.cpp
	Variant.cpp
	XMLParser.cpp
	PluginDataModel.cpp
)

set_common_target_options(${TARGET_NAME})

target_link_libraries(${TARGET_NAME} PRIVATE
	rmlui_tests_common
	rmlui_core
	doctest::doctest
	trompeloeil::trompeloeil
)

if(NOT EMSCRIPTEN)
	doctest_discover_tests(${TARGET_NAME})
endif()
