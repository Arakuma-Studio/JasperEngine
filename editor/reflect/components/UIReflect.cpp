// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The UI components (docs/specs/S08-reflection.md P2; the UI renderer keeps its own codec, P4).
// Field order is the YAML key order.

#include "reflect/components/MaterialReflect.h"

#include "Factory.h"
#include "Stream.h"
#include "component/ButtonComponent.h"
#include "component/UISelectable.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/PanelComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/TextComponent.h"
#include "subsystem/TitleArtSystem.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    template<typename T, bool T::*flag>
    void markForUpdate(EntityRegistry* registry, Entity entity) {
        if (T* comp = registry->findComponent<T>(entity)) comp->*flag = true;
    }

    // ---------------------------------------------------------------- Rect Transform

    bool rectUsesAnchors(const void* layout) { return static_cast<const RectTransformComponent*>(layout)->usingAnchors; }
    bool rectAnchorsEditable(const void* layout) { return static_cast<const RectTransformComponent*>(layout)->anchorPreset == AnchorPreset::NONE; }

    constexpr uint64_t kLayoutSizes = UpdateFlags_Layout_Sizes | UpdateFlags_Layout_Anchors;

    constexpr FieldDesc kRectTransformFields[] = {
        JE_FIELD(RectTransformComponent, width, .flags = NoDefault, .updateFlags = kLayoutSizes, .unit = Unit::Pixels, .step = 1.0f),
        JE_FIELD(RectTransformComponent, height, .flags = NoDefault, .updateFlags = kLayoutSizes, .unit = Unit::Pixels, .step = 1.0f),
        JE_FIELD(RectTransformComponent, anchorPointLeft, .min = 0.0f, .max = 1.0f, .step = 0.01f, .label = "Anchor Left",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 4),
        JE_FIELD(RectTransformComponent, anchorPointTop, .min = 0.0f, .max = 1.0f, .step = 0.01f, .label = "Anchor Top",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 5),
        JE_FIELD(RectTransformComponent, anchorPointRight, .min = 0.0f, .max = 1.0f, .step = 0.01f, .label = "Anchor Right",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 6),
        JE_FIELD(RectTransformComponent, anchorPointBottom, .min = 0.0f, .max = 1.0f, .step = 0.01f, .label = "Anchor Bottom",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 7),
        JE_FIELD(RectTransformComponent, anchorOffsetLeft, .unit = Unit::Pixels, .step = 1.0f, .label = "Offset Left",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 8),
        JE_FIELD(RectTransformComponent, anchorOffsetTop, .unit = Unit::Pixels, .step = 1.0f, .label = "Offset Top",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 9),
        JE_FIELD(RectTransformComponent, anchorOffsetRight, .unit = Unit::Pixels, .step = 1.0f, .label = "Offset Right",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 10),
        JE_FIELD(RectTransformComponent, anchorOffsetBottom, .unit = Unit::Pixels, .step = 1.0f, .label = "Offset Bottom",
                 .visibleIf = &rectUsesAnchors, .enabledIf = &rectAnchorsEditable, .uiOrder = 11),
        JE_FIELD(RectTransformComponent, positionOffset, .unit = Unit::Pixels, .step = 1.0f, .uiOrder = 12),
        JE_FIELD(RectTransformComponent, pivot, .updateFlags = UpdateFlags_Transform, .step = 0.01f,
                 .tooltip = "Scale and rotation center: (0, 0) is the top-left corner, (0.5, 0.5) the center", .uiOrder = 13),
        JE_FIELD(RectTransformComponent, anchorPreset, .enumDesc = &enums::kAnchorPreset, .label = "Preset", .visibleIf = &rectUsesAnchors,
                 .uiOrder = 3),
        JE_FIELD(RectTransformComponent, usingAnchors, .label = "Use Anchors", .uiOrder = 2),
        // the layout's own bookkeeping: saved, not edited
        JE_FIELD(RectTransformComponent, panel, .ops = &ops::Entity, .flags = Derived | Hidden | NoCodegen),
        JE_FIELD(RectTransformComponent, containerBoxIndex, .flags = Derived | Hidden | NoCodegen),
        JE_FIELD(RectTransformComponent, scissor, .flags = Derived | Hidden | NoCodegen | NotWritten),
        JE_FIELD(RectTransformComponent, ignoreScissor, .uiOrder = 14),
        JE_FIELD(RectTransformComponent, ignoreEvents, .uiOrder = 15),
    };

    // ---------------------------------------------------------------- Button

    // what a Button shows by its transition and navigation mode (Unity's Inspector)
    bool buttonTints(const void* button) {
        const ButtonTransition t = static_cast<const ButtonComponent*>(button)->transition;
        return t == ButtonTransition::COLOR_TINT || t == ButtonTransition::COLOR_TINT_AND_SPRITE_SWAP;
    }
    bool buttonSwaps(const void* button) {
        const ButtonTransition t = static_cast<const ButtonComponent*>(button)->transition;
        return t == ButtonTransition::SPRITE_SWAP || t == ButtonTransition::COLOR_TINT_AND_SPRITE_SWAP;
    }
    bool buttonNavigationExplicit(const void* button) {
        return static_cast<const ButtonComponent*>(button)->navigationMode == NavigationMode::EXPLICIT;
    }
    bool buttonNavigationWraps(const void* button) {
        const NavigationMode mode = static_cast<const ButtonComponent*>(button)->navigationMode;
        return mode != NavigationMode::NONE && mode != NavigationMode::EXPLICIT;
    }

    // every Button field redraws the button, from any edit path (Inspector, MCP, undo)
    constexpr uint64_t kButton = UpdateFlags_Button;

    constexpr FieldDesc kButtonFields[] = {
        JE_FIELD(ButtonComponent, label, .ops = &ops::Entity, .updateFlags = kButton, .uiOrder = 30),
        JE_FIELD(ButtonComponent, textureNormal, .updateFlags = kButton, .label = "Normal Texture", .group = "Transition",
                 .visibleIf = &buttonSwaps, .uiOrder = 10),
        JE_FIELD(ButtonComponent, textureHovered, .updateFlags = kButton, .label = "Highlighted Texture", .group = "Transition",
                 .visibleIf = &buttonSwaps, .uiOrder = 11),
        JE_FIELD(ButtonComponent, texturePressed, .updateFlags = kButton, .label = "Pressed Texture", .group = "Transition",
                 .visibleIf = &buttonSwaps, .uiOrder = 12),
        JE_FIELD(ButtonComponent, textureDisabled, .updateFlags = kButton, .label = "Disabled Texture", .group = "Transition",
                 .visibleIf = &buttonSwaps, .uiOrder = 14),
        JE_FIELD(ButtonComponent, colorNormal, .flags = ColorLinear, .updateFlags = kButton, .label = "Normal Color", .group = "Transition",
                 .visibleIf = &buttonTints, .uiOrder = 3),
        JE_FIELD(ButtonComponent, colorHovered, .flags = ColorLinear, .updateFlags = kButton, .label = "Highlighted Color", .group = "Transition",
                 .visibleIf = &buttonTints, .uiOrder = 4),
        JE_FIELD(ButtonComponent, colorPressed, .flags = ColorLinear, .updateFlags = kButton, .label = "Pressed Color", .group = "Transition",
                 .visibleIf = &buttonTints, .uiOrder = 5),
        JE_FIELD(ButtonComponent, colorDisabled, .flags = ColorLinear, .updateFlags = kButton, .label = "Disabled Color", .group = "Transition",
                 .visibleIf = &buttonTints, .uiOrder = 7),
        // the Inspector draws it inverted, as Unity's Interactable (InspectorButton.cpp)
        JE_FIELD(ButtonComponent, disabled, .flags = Hidden, .updateFlags = kButton, .uiOrder = 1),
        // ui-components: Unity's Selectable (appended: the YAML keys above keep their order)
        JE_FIELD(ButtonComponent, textureSelected, .updateFlags = kButton, .label = "Selected Texture", .group = "Transition",
                 .visibleIf = &buttonSwaps, .uiOrder = 13),
        JE_FIELD(ButtonComponent, colorSelected, .flags = ColorLinear, .updateFlags = kButton, .label = "Selected Color", .group = "Transition",
                 .visibleIf = &buttonTints, .uiOrder = 6),
        JE_FIELD(ButtonComponent, transition, .enumDesc = &enums::kButtonTransition, .updateFlags = kButton,
                 .tooltip = "What the button changes by its state: the color, the texture, both or nothing (then animate the image yourself)",
                 .group = "Transition", .uiOrder = 2),
        JE_FIELD(ButtonComponent, fadeDuration, .flags = Clamped, .updateFlags = kButton, .unit = Unit::Seconds, .min = 0.0f, .max = 10.0f,
                 .step = 0.01f, .tooltip = "Seconds the color and scale take to change (UI time: also while the game is paused)",
                 .group = "Transition", .uiOrder = 8),
        JE_FIELD(ButtonComponent, scaleHovered, .flags = Clamped, .updateFlags = kButton, .min = 0.01f, .max = 10.0f, .step = 0.01f,
                 .label = "Highlighted Scale", .group = "Transition", .uiOrder = 15),
        JE_FIELD(ButtonComponent, scalePressed, .flags = Clamped, .updateFlags = kButton, .min = 0.01f, .max = 10.0f, .step = 0.01f,
                 .label = "Pressed Scale", .group = "Transition", .uiOrder = 16),
        JE_FIELD(ButtonComponent, scaleSelected, .flags = Clamped, .updateFlags = kButton, .min = 0.01f, .max = 10.0f, .step = 0.01f,
                 .label = "Selected Scale", .group = "Transition", .uiOrder = 17),
        JE_FIELD(ButtonComponent, navigationMode, .enumDesc = &enums::kNavigationMode, .updateFlags = kButton, .label = "Navigation",
                 .tooltip = "How the arrow keys, the d-pad and a TV remote move the selection from this button",
                 .group = "Navigation", .uiOrder = 20),
        JE_FIELD(ButtonComponent, wrapAround, .updateFlags = kButton, .group = "Navigation", .visibleIf = &buttonNavigationWraps, .uiOrder = 21),
        JE_FIELD(ButtonComponent, selectOnUp, .ops = &ops::Entity, .updateFlags = kButton, .group = "Navigation",
                 .visibleIf = &buttonNavigationExplicit, .uiOrder = 22),
        JE_FIELD(ButtonComponent, selectOnDown, .ops = &ops::Entity, .updateFlags = kButton, .group = "Navigation",
                 .visibleIf = &buttonNavigationExplicit, .uiOrder = 23),
        JE_FIELD(ButtonComponent, selectOnLeft, .ops = &ops::Entity, .updateFlags = kButton, .group = "Navigation",
                 .visibleIf = &buttonNavigationExplicit, .uiOrder = 24),
        JE_FIELD(ButtonComponent, selectOnRight, .ops = &ops::Entity, .updateFlags = kButton, .group = "Navigation",
                 .visibleIf = &buttonNavigationExplicit, .uiOrder = 25),
        JE_FIELD(ButtonComponent, selectOnEnable, .updateFlags = kButton,
                 .tooltip = "Becomes the scene's selection when it comes on: keyboards, gamepads and TV remotes then drive the menu (Unity's First Selected)",
                 .group = "Navigation", .uiOrder = 26),
    };

    // ---------------------------------------------------------------- Scrollbar, Progress Bar

    constexpr FieldDesc kScrollbarFields[] = {
        JE_FIELD(ScrollbarComponent, bar, .ops = &ops::Entity),
        JE_FIELD(ScrollbarComponent, type, .enumDesc = &enums::kScrollbarType),
        JE_FIELD(ScrollbarComponent, barSize, .min = 0.0f, .max = 1.0f, .step = 0.01f),
        JE_FIELD(ScrollbarComponent, step, .min = 0.0f, .max = 1.0f, .step = 0.01f),
        JE_FIELD(ScrollbarComponent, barMarginLeft, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Left", .group = "Bar margin"),
        JE_FIELD(ScrollbarComponent, barMarginRight, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Right", .group = "Bar margin"),
        JE_FIELD(ScrollbarComponent, barMarginTop, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Top", .group = "Bar margin"),
        JE_FIELD(ScrollbarComponent, barMarginBottom, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Bottom", .group = "Bar margin"),
    };

    constexpr FieldDesc kProgressbarFields[] = {
        JE_FIELD(ProgressbarComponent, fill, .ops = &ops::Entity),
        JE_FIELD(ProgressbarComponent, type, .enumDesc = &enums::kProgressbarType),
        JE_FIELD(ProgressbarComponent, value, .min = 0.0f, .max = 1.0f, .step = 0.01f),
        JE_FIELD(ProgressbarComponent, fillMarginLeft, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Left", .group = "Fill margin"),
        JE_FIELD(ProgressbarComponent, fillMarginRight, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Right", .group = "Fill margin"),
        JE_FIELD(ProgressbarComponent, fillMarginTop, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Top", .group = "Fill margin"),
        JE_FIELD(ProgressbarComponent, fillMarginBottom, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Bottom", .group = "Fill margin"),
    };

    // ---------------------------------------------------------------- Input Field

    constexpr FieldDesc kInputFieldFields[] = {
        JE_FIELD(InputFieldComponent, text, .ops = &ops::Entity),
        JE_FIELD(InputFieldComponent, selection, .ops = &ops::Entity),
        JE_FIELD(InputFieldComponent, cursor, .ops = &ops::Entity),
        JE_FIELD(InputFieldComponent, cursorBlink, .unit = Unit::Seconds, .label = "Blink", .group = "Cursor", .uiOrder = 7),
        JE_FIELD(InputFieldComponent, cursorWidth, .unit = Unit::Pixels, .label = "Width", .group = "Cursor", .uiOrder = 8),
        JE_FIELD(InputFieldComponent, cursorColor, .flags = ColorLinear, .label = "Color", .group = "Cursor", .uiOrder = 9),
        JE_FIELD(InputFieldComponent, selectionColor, .flags = ColorLinear, .group = "Cursor", .uiOrder = 10),
        JE_FIELD(InputFieldComponent, placeholderColor, .flags = ColorLinear, .group = "Cursor", .uiOrder = 11),
        JE_FIELD(InputFieldComponent, placeholder, .group = "Content", .uiOrder = 6),
        // saved and exported, edited by its own row
        JE_FIELD(InputFieldComponent, passwordChar, .ops = &ops::Char, .flags = Derived | Hidden),
        JE_FIELD(InputFieldComponent, cursorIndex, .step = 1.0f, .group = "Selection", .uiOrder = 12),
        JE_FIELD(InputFieldComponent, selectionAnchor, .step = 1.0f, .group = "Selection", .uiOrder = 13),
        JE_FIELD(InputFieldComponent, disabled, .uiOrder = 4),
        JE_FIELD(InputFieldComponent, password, .uiOrder = 5),
    };

    // ---------------------------------------------------------------- Panel

    constexpr FieldDesc kPanelFields[] = {
        JE_FIELD(PanelComponent, headerimage, .ops = &ops::Entity, .label = "Header Image"),
        JE_FIELD(PanelComponent, headercontainer, .ops = &ops::Entity, .label = "Header Container"),
        JE_FIELD(PanelComponent, headertext, .ops = &ops::Entity, .label = "Header Text"),
        JE_FIELD(PanelComponent, titleAnchorPreset, .enumDesc = &enums::kAnchorPreset, .label = "Title Anchor"),
        JE_FIELD(PanelComponent, minWidth, .unit = Unit::Pixels, .step = 1.0f),
        JE_FIELD(PanelComponent, minHeight, .unit = Unit::Pixels, .step = 1.0f),
        JE_FIELD(PanelComponent, headerMarginLeft, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Left", .group = "Header margin",
                 .uiOrder = 11),
        JE_FIELD(PanelComponent, headerMarginTop, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Top", .group = "Header margin",
                 .uiOrder = 13),
        JE_FIELD(PanelComponent, headerMarginRight, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Right", .group = "Header margin",
                 .uiOrder = 12),
        JE_FIELD(PanelComponent, headerMarginBottom, .unit = Unit::Pixels, .step = 1.0f, .label = "Margin Bottom", .group = "Header margin",
                 .uiOrder = 14),
        JE_FIELD(PanelComponent, defaultHeaderMargin, .label = "Default Margin", .group = "Header margin", .uiOrder = 10),
        JE_FIELD(PanelComponent, resizeMargin, .unit = Unit::Pixels, .step = 1.0f, .uiOrder = 6),
        JE_FIELD(PanelComponent, canMove, .uiOrder = 7),
        JE_FIELD(PanelComponent, canResize, .uiOrder = 8),
        JE_FIELD(PanelComponent, canBringToFront, .uiOrder = 9),
    };

    // ---------------------------------------------------------------- Layout Group

    constexpr FieldDesc kRectLaneFields[] = {
        JE_PROXY(Rect, "x", ptr()[0]),
        JE_PROXY(Rect, "y", ptr()[1]),
        JE_PROXY(Rect, "width", ptr()[2]),
        JE_PROXY(Rect, "height", ptr()[3]),
    };
    const StructDesc kRectLanes = JE_STRUCT_DESC(Rect, "Rect", kRectLaneFields);

    constexpr FieldDesc kContainerBoxFields[] = {
        JE_FIELD(ContainerBox, layout, .flags = ReadOnly, .label = "Layout GameObject"),
        JE_FIELD(ContainerBox, rect, .ops = &ops::RectLanes, .structDesc = &kRectLanes, .flags = ReadOnly),
        JE_FIELD(ContainerBox, expand, .updateFlags = UpdateFlags_Layout_Sizes),
    };

}

JE_REFLECT_STRUCT_TRAITS(ContainerBox, kContainerBoxStruct);
const StructDesc kContainerBoxStruct = JE_STRUCT_DESC(ContainerBox, "ContainerBox", kContainerBoxFields);

namespace {

    bool layoutWraps(const void* container) {
        const LayoutGroupType type = static_cast<const LayoutGroupComponent*>(container)->type;
        return type == LayoutGroupType::VERTICAL_WRAP || type == LayoutGroupType::HORIZONTAL_WRAP;
    }

    // older files call useAllWrapSpace fillAvailableSpace
    constexpr const char* kUseAllWrapSpaceAliases[] = {"fillAvailableSpace"};

    constexpr FieldDesc kLayoutGroupFields[] = {
        JE_FIELD(LayoutGroupComponent, type, .enumDesc = &enums::kLayoutGroupType, .updateFlags = UpdateFlags_Layout_Sizes),
        JE_FIELD(LayoutGroupComponent, useAllWrapSpace, .updateFlags = UpdateFlags_Layout_Sizes, .label = "Use All Space", .group = "Wrap settings",
                 .visibleIf = &layoutWraps, .aliases = kUseAllWrapSpaceAliases),
        JE_FIELD(LayoutGroupComponent, wrapCellWidth, .updateFlags = UpdateFlags_Layout_Sizes, .unit = Unit::Pixels, .step = 1.0f,
                 .label = "Cell Width", .group = "Wrap settings", .visibleIf = &layoutWraps),
        JE_FIELD(LayoutGroupComponent, wrapCellHeight, .updateFlags = UpdateFlags_Layout_Sizes, .unit = Unit::Pixels, .step = 1.0f,
                 .label = "Cell Height", .group = "Wrap settings", .visibleIf = &layoutWraps),
        // the layout's runtime state
        JE_FIELD(LayoutGroupComponent, numBoxes, .flags = Transient | ReadOnly | NoMcpWrite, .drawer = "label"),
        JE_FIELD(LayoutGroupComponent, boxes, .flags = Transient | Hidden | NoMcpWrite | ListTemplate),
    };

    // ---------------------------------------------------------------- Text

    constexpr uint64_t kText = UpdateFlags_Text;

    constexpr FieldDesc kTextFields[] = {
        JE_FIELD(TextComponent, font, .updateFlags = UpdateFlags_Text_Atlas, .asset = AssetKind::Font, .uiOrder = 1),
        JE_FIELD(TextComponent, text, .updateFlags = kText, .drawer = "multiline"),
        JE_FIELD(TextComponent, fontSize, .updateFlags = UpdateFlags_Text_Atlas, .unit = Unit::Pixels, .step = 1.0f, .label = "FontSize",
                 .uiOrder = 2),
        JE_FIELD(TextComponent, multiline, .updateFlags = kText, .uiOrder = 3),
        JE_FIELD(TextComponent, maxTextSize, .updateFlags = kText, .step = 1.0f, .label = "Max. text size", .group = "Advanced settings",
                 .uiOrder = 10),
        JE_FIELD(TextComponent, fixedWidth, .updateFlags = kText, .label = "Width", .group = "Fixed size"),
        JE_FIELD(TextComponent, fixedHeight, .updateFlags = kText, .label = "Height", .group = "Fixed size"),
        JE_FIELD(TextComponent, pivotBaseline, .updateFlags = kText, .label = "Baseline", .group = "Pivot"),
        JE_FIELD(TextComponent, pivotCentered, .updateFlags = kText, .label = "Center", .group = "Pivot"),
        // upstream (ui-scaling-text): outline, shadow and visible characters, written only when set;
        // the widths read finite and not below 0, the visible characters not below -1 (its decode)
        JE_FIELD(TextComponent, outlineWidth, .ops = &ops::NonNegativeFiniteFloat, .flags = OmitIfDefault, .updateFlags = kText, .min = 0.0f, .group = "Outline"),
        JE_FIELD(TextComponent, outlineColor, .flags = OmitIfDefault | ColorLinear, .updateFlags = kText, .group = "Outline"),
        JE_FIELD(TextComponent, shadowEnabled, .flags = OmitIfDefault, .updateFlags = kText, .label = "Shadow", .group = "Shadow"),
        JE_FIELD(TextComponent, shadowColor, .flags = OmitIfDefault | ColorLinear, .updateFlags = kText, .group = "Shadow"),
        JE_FIELD(TextComponent, shadowOffset, .flags = OmitIfDefault, .updateFlags = kText, .group = "Shadow"),
        JE_FIELD(TextComponent, shadowSoftness, .ops = &ops::NonNegativeFiniteFloat, .flags = OmitIfDefault, .updateFlags = kText, .min = 0.0f, .group = "Shadow"),
        JE_FIELD(TextComponent, shadowDilate, .ops = &ops::NonNegativeFiniteFloat, .flags = OmitIfDefault, .updateFlags = kText, .min = 0.0f, .group = "Shadow"),
        JE_FIELD(TextComponent, visibleCharacters, .ops = &ops::IntClampedOnRead, .flags = OmitIfDefault, .updateFlags = kText, .min = -1.0f,
                 .tooltip = "Characters past it are not drawn (-1: all)."),
    };

    // ---------------------------------------------------------------- Image

    constexpr FieldDesc kImageFields[] = {
        // unsigned, read as signed (the older decoder did)
        JE_FIELD(ImageComponent, patchMarginLeft, .ops = &ops::UIntFromInt, .updateFlags = UpdateFlags_Image_Patches, .unit = Unit::Pixels,
                 .step = 1.0f, .label = "Margin Left", .group = "Nine-patch rect"),
        JE_FIELD(ImageComponent, patchMarginRight, .ops = &ops::UIntFromInt, .updateFlags = UpdateFlags_Image_Patches, .unit = Unit::Pixels,
                 .step = 1.0f, .label = "Margin Right", .group = "Nine-patch rect"),
        JE_FIELD(ImageComponent, patchMarginTop, .ops = &ops::UIntFromInt, .updateFlags = UpdateFlags_Image_Patches, .unit = Unit::Pixels,
                 .step = 1.0f, .label = "Margin Top", .group = "Nine-patch rect"),
        JE_FIELD(ImageComponent, patchMarginBottom, .ops = &ops::UIntFromInt, .updateFlags = UpdateFlags_Image_Patches, .unit = Unit::Pixels,
                 .step = 1.0f, .label = "Margin Bottom", .group = "Nine-patch rect"),
        JE_FIELD(ImageComponent, textureScaleFactor, .updateFlags = UpdateFlags_Image_Patches, .label = "Texture Scale",
                 .tooltip = "Increase or decrease texture area by a factor", .group = "Nine-patch rect"),
    };

    // ---------------------------------------------------------------- UI Document

    bool documentInTexture(const void* document) {
        return static_cast<const UIDocumentComponent*>(document)->renderMode == UIDocumentRenderMode::TEXTURE;
    }

    constexpr uint64_t kDocumentReload = UpdateFlags_UIDocument_Reload;

    constexpr FieldDesc kUIDocumentFields[] = {
        JE_FIELD(UIDocumentComponent, enabled, .flags = NonStructural | CodegenIfChanged),
        // written when empty too: decoding over a component with a file must clear it
        JE_FIELD(UIDocumentComponent, filename, .updateFlags = kDocumentReload, .asset = AssetKind::UIDocument, .label = "Source",
                 .tooltip = "HTML file of the document, relative to the assets folder (e.g. ui/menu.html)."),
        JE_FIELD(UIDocumentComponent, sortOrder, .flags = NonStructural,
                 .tooltip = "Documents with a higher sort order draw on top of the scene's other documents."),
        // phase 1: only what differs from the defaults; an empty value is the default too
        JE_FIELD(UIDocumentComponent, interactive, .flags = OmitIfDefault | EmptyIsDefault | NonStructural | CodegenIfChanged,
                 .tooltip = "Receives pointer, keyboard and text input. Off: the game gets every event."),
        JE_FIELD(UIDocumentComponent, renderMode, .enumDesc = &enums::kUIDocumentRenderMode,
                 .flags = OmitIfDefault | EmptyIsDefault | CodegenIfChanged, .updateFlags = kDocumentReload,
                 .tooltip = "Screen: drawn over the scene. Texture: rendered into a texture that a material, Mesh or Image shows."),
        JE_FIELD(UIDocumentComponent, textureWidth, .ops = &ops::IntClampedOnRead, .flags = OmitIfDefault | EmptyIsDefault | CodegenIfChanged | Clamped,
                 .updateFlags = kDocumentReload, .unit = Unit::Pixels, .min = 1.0f, .max = 8192.0f, .visibleIf = &documentInTexture),
        JE_FIELD(UIDocumentComponent, textureHeight, .ops = &ops::IntClampedOnRead, .flags = OmitIfDefault | EmptyIsDefault | CodegenIfChanged | Clamped,
                 .updateFlags = kDocumentReload, .unit = Unit::Pixels, .min = 1.0f, .max = 8192.0f, .visibleIf = &documentInTexture),
        JE_FIELD(UIDocumentComponent, inputSurface, .ops = &ops::Entity, .flags = OmitIfDefault | EmptyIsDefault | CodegenIfChanged,
                 .tooltip = "Mesh whose surface receives the pointer (hit test on its UVs). None: this GameObject's own mesh.",
                 .visibleIf = &documentInTexture),
        JE_FIELD(UIDocumentComponent, pixelRatio, .ops = &ops::NonNegativeFiniteFloat,
                 .flags = OmitIfDefault | OmitIfNonFinite | EmptyIsDefault | NonStructural | CodegenIfChanged, .updateFlags = kDocumentReload,
                 .min = 0.0f, .tooltip = "Document pixels per CSS pixel. 0 = Auto: the canvas-to-screen scale (1 in Texture mode)."),
    };

    constexpr const char* kUIDocumentCodegenOrder[] = {
        "filename", "sortOrder", "enabled", "interactive", "renderMode", "textureWidth", "textureHeight", "inputSurface", "pixelRatio"
    };

    // the old one's runtime state (its loaded document) stays while the file and render settings do
    void uiDocumentPostDecode(void* comp, const DecodeContext& ctx) {
        UIDocumentComponent& document = *static_cast<UIDocumentComponent*>(comp);
        const UIDocumentComponent* old = static_cast<const UIDocumentComponent*>(ctx.old);
        if (old && (old->filename != document.filename || old->renderMode != document.renderMode || old->textureWidth != document.textureWidth ||
                    old->textureHeight != document.textureHeight || old->pixelRatio != document.pixelRatio)) {
            document.needReload = true;
        }
    }

    // ---------------------------------------------------------------- UI renderer and polygon (their own codecs)

    YAML::Node encodeUI(const void* comp, const EncodeContext& ctx) {
        // a Title Art Renderer's Live Effects drive the shader: the Image's own one is saved (S81 R2)
        TitleArtCompanionScope companion(ctx.registry, ctx.entity);
        const bool isText = ctx.registry && ctx.signature.test(ctx.registry->getComponentId<TextComponent>());
        const bool isGenerated = ctx.registry && Stream::isGeneratedUI(ctx.registry, ctx.signature);
        return Stream::encodeUIComponent(*static_cast<const UIComponent*>(comp), !isGenerated, !isText, isText);
    }

    void decodeUI(const YAML::Node& node, void* comp, const DecodeContext& ctx) {
        // a Text decoded over a live one keeps the atlas it shows (a new one publishes its own)
        const bool isText = ctx.registry && ctx.entity != NULL_ENTITY && ctx.registry->findComponent<TextComponent>(ctx.entity);
        *static_cast<UIComponent*>(comp) = Stream::decodeUIComponent(node, static_cast<const UIComponent*>(ctx.old), isText);
    }

    const ComponentCodec kUICodec{&encodeUI, &decodeUI};

    constexpr uint64_t kShaderReload = UpdateFlags_Shader_Reload;

    constexpr FieldDesc kUIFields[] = {
        JE_FIELD(UIComponent, enabled),
        JE_FIELD(UIComponent, color, .flags = ColorLinear),
        JE_FIELD(UIComponent, texture, .updateFlags = UpdateFlags_UI_Texture),
        JE_FIELD(UIComponent, customShader, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        // the shader library's: resolved when the UI loads its shader
        JE_FIELD(UIComponent, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(UIComponent, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, stencil, .flags = ListWhole, .updateFlags = kShaderReload),
        JE_FIELD(UIComponent, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kShaderReload, .min = -1.0f, .max = 15.0f),
    };

    std::string uiCodegen(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createUIComponent(ctx.indentSpaces, registry, entity, ctx.projectPath, ctx.sceneName, ctx.entityName,
                                          ctx.assignExisting, ctx.entityVarNames);
    }

    constexpr const char* kUIExtraKeys[] = {"buffer", "indices", "minBufferCount", "minIndicesCount", "primitiveType", "vertexCount", "aabb",
        "worldAABB", "texture", "color", "automaticFlipY", "flipY", "pointerMoved", "focused", "customShader", "shaderUniforms", "shaderTextures",
        "shaderFeatures", "shaderGlobalBindings", "blendMode", "depthWrite", "stencil", "colorWrite", "enabled"};

}

const ComponentDesc kRectTransformDesc{
    .type = ComponentType::RectTransformComponent, .cppName = "RectTransformComponent", .key = "recttransform",
    .displayName = "Rect Transform", .icon = ICON_FA_VECTOR_SQUARE,
    .body = JE_STRUCT_DESC(RectTransformComponent, "RectTransformComponent", kRectTransformFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(RectTransformComponent),
    .codegenVar = "layout",
};

const ComponentDesc kButtonDesc{
    .type = ComponentType::ButtonComponent, .cppName = "ButtonComponent", .key = "button", .displayName = "Button", .icon = ICON_FA_SQUARE_CHECK,
    .body = JE_STRUCT_DESC(ButtonComponent, "ButtonComponent", kButtonFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ButtonComponent),
    .codegenVar = "button", .edited = &markForUpdate<ButtonComponent, &ButtonComponent::needUpdateButton>,
};

const ComponentDesc kScrollbarDesc{
    .type = ComponentType::ScrollbarComponent, .cppName = "ScrollbarComponent", .key = "scrollbar", .displayName = "Scrollbar",
    .icon = ICON_FA_ARROWS_UP_DOWN,
    .body = JE_STRUCT_DESC(ScrollbarComponent, "ScrollbarComponent", kScrollbarFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ScrollbarComponent),
    .postDecode = [](void* comp, const DecodeContext&) { static_cast<ScrollbarComponent*>(comp)->needUpdateScrollbar = true; },
    .codegenVar = "scrollbar", .edited = &markForUpdate<ScrollbarComponent, &ScrollbarComponent::needUpdateScrollbar>,
};

const ComponentDesc kProgressbarDesc{
    .type = ComponentType::ProgressbarComponent, .cppName = "ProgressbarComponent", .key = "progressbar", .displayName = "Progress Bar",
    .icon = ICON_FA_BARS_PROGRESS,
    .body = JE_STRUCT_DESC(ProgressbarComponent, "ProgressbarComponent", kProgressbarFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ProgressbarComponent),
    .postDecode = [](void* comp, const DecodeContext&) { static_cast<ProgressbarComponent*>(comp)->needUpdateProgressbar = true; },
    .codegenVar = "progressbar", .edited = &markForUpdate<ProgressbarComponent, &ProgressbarComponent::needUpdateProgressbar>,
};

const ComponentDesc kInputFieldDesc{
    .type = ComponentType::InputFieldComponent, .cppName = "InputFieldComponent", .key = "inputfield", .displayName = "Input Field",
    .icon = ICON_FA_KEYBOARD,
    .body = JE_STRUCT_DESC(InputFieldComponent, "InputFieldComponent", kInputFieldFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(InputFieldComponent),
    .postDecode = [](void* comp, const DecodeContext&) { static_cast<InputFieldComponent*>(comp)->needUpdateInputField = true; },
    .codegenVar = "inputfield", .edited = &markForUpdate<InputFieldComponent, &InputFieldComponent::needUpdateInputField>,
};

const ComponentDesc kPanelDesc{
    .type = ComponentType::PanelComponent, .cppName = "PanelComponent", .key = "panel", .displayName = "Panel", .icon = ICON_FA_WINDOW_MAXIMIZE,
    .body = JE_STRUCT_DESC(PanelComponent, "PanelComponent", kPanelFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(PanelComponent),
    .postDecode = [](void* comp, const DecodeContext&) { static_cast<PanelComponent*>(comp)->needUpdatePanel = true; },
    .codegenVar = "panel", .edited = &markForUpdate<PanelComponent, &PanelComponent::needUpdatePanel>,
};

const ComponentDesc kLayoutGroupDesc{
    .type = ComponentType::LayoutGroupComponent, .cppName = "LayoutGroupComponent", .key = "layoutgroup", .displayName = "Layout Group",
    .icon = ICON_FA_TABLE_CELLS,
    .body = JE_STRUCT_DESC(LayoutGroupComponent, "LayoutGroupComponent", kLayoutGroupFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(LayoutGroupComponent),
    .codegenVar = "container",
};

const ComponentDesc kTextDesc{
    .type = ComponentType::TextComponent, .cppName = "TextComponent", .key = "text", .displayName = "Text", .icon = ICON_FA_FONT,
    .body = JE_STRUCT_DESC(TextComponent, "TextComponent", kTextFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(TextComponent),
    .codegenVar = "text",
};

const ComponentDesc kImageDesc{
    .type = ComponentType::ImageComponent, .cppName = "ImageComponent", .key = "image", .displayName = "Image", .icon = ICON_FA_IMAGE,
    .body = JE_STRUCT_DESC(ImageComponent, "ImageComponent", kImageFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ImageComponent),
    .codegenVar = "image",
};

const ComponentDesc kUIDocumentDesc{
    .type = ComponentType::UIDocumentComponent, .cppName = "UIDocumentComponent", .key = "uidocument", .displayName = "UI Document",
    .icon = ICON_FA_FILE_CODE,
    .body = JE_STRUCT_DESC(UIDocumentComponent, "UIDocumentComponent", kUIDocumentFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(UIDocumentComponent),
    .postDecode = &uiDocumentPostDecode,
    .codegenVar = "uidocument", .codegenOrder = kUIDocumentCodegenOrder,
};

const ComponentDesc kUIDesc{
    .type = ComponentType::UIComponent, .cppName = "UIComponent", .key = "ui", .displayName = "Canvas Renderer", .icon = ICON_FA_PAINTBRUSH,
    .body = JE_STRUCT_DESC(UIComponent, "UIComponent", kUIFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(UIComponent),
    .codec = &kUICodec,
    .codegen = &uiCodegen,
    .extraKeys = kUIExtraKeys,
};


}
