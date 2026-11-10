// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DiagnosticCodes.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr Severity E = Severity::Error;
    constexpr Severity W = Severity::Warning;
    constexpr Severity I = Severity::Info;

    // Appendix F of phase-2-compat.md. Sorted by area, then number; append only.
    const DiagCodeInfo registry[] = {
        // ---- GEN
        {codes::FileNotFound, E, false, "file-not-found", "File not found",
         "A file the document references (the document itself, a stylesheet, an image, a script or a font) does not exist at the "
         "resolved asset-relative path. Check the path: relative paths resolve against the referencing file's directory, paths "
         "starting with '/' against the assets directory."},
        {codes::DiagnosticsTruncated, I, false, "diagnostics-truncated", "Diagnostics truncated",
         "The per-document diagnostic limit was reached; later diagnostics were not recorded. Fix the reported problems and load "
         "the document again to see the rest."},
        {codes::InvalidUtf8, W, false, "invalid-utf8", "Invalid UTF-8",
         "The file contains bytes that are not valid UTF-8; they were replaced by U+FFFD. Save the file as UTF-8."},
        {codes::InternalError, E, false, "internal-error", "Internal error",
         "The compatibility layer failed on this input. This is a bug in the engine; the message has the details. The document "
         "may render incompletely."},
        {codes::NonUtf8Charset, W, false, "non-utf8-charset", "Charset other than UTF-8",
         "The document or stylesheet declares a character encoding other than UTF-8 (<meta charset>, @charset). The engine "
         "always decodes UTF-8: save the file as UTF-8 and declare utf-8."},
        {codes::RmluiMessage, W, false, "rmlui-message", "RmlUi message",
         "RmlUi, the engine's layout library, rejected or warned about the converted document or stylesheet. The location is "
         "mapped back to your file. Simplify the construct or report the case: converted output should always be accepted."},
        {codes::GlyphAtlasBudget, W, false, "glyph-atlas-budget", "Glyph atlas budget too small",
         "The text drawn needs more glyphs than the glyph atlas holds: its least recently used pages keep being cleared and "
         "their glyphs made again (more than 4 times in 120 frames). Raise Project Settings > Graphics > HTML UI > Glyph Atlas "
         "Budget (graphics.html_ui_glyph_atlas_mib), or use fewer font sizes and fonts at once."},

        // ---- HTML
        {codes::HtmlParseError, I, false, "html-parse-error", "HTML parse error",
         "The HTML parser recovered from a syntax error exactly the way browsers do (for example a missing end tag). The "
         "document renders as in a browser; fix the markup to silence the note."},
        {codes::UnsupportedElement, W, false, "unsupported-element", "Unsupported element",
         "The element has no engine equivalent (video, audio, iframe, canvas, object, ...); it was replaced by a placeholder "
         "box of the browser's default size or dropped. The suggestion names an alternative: an image, a Lottie animation or "
         "a render-to-texture document."},
        {codes::ElementMapped, I, false, "element-mapped", "Element mapped",
         "The element was mapped to an equivalent the engine supports (meter to progress, optgroup flattened into its select, "
         "output to span, picture resolved to one img, ...)."},
        {codes::UnsupportedAttribute, W, true, "unsupported-attribute", "Unsupported attribute",
         "The attribute has no effect in the engine and was dropped (Info when it never has a visible effect in a game UI, "
         "Warning when it changes behavior in a browser, such as contenteditable or popover)."},
        {codes::EventHandlerAttribute, E, false, "event-handler-attribute", "Event handler attribute",
         "An on* attribute holds JavaScript, which the engine never runs. Register the handler from Lua instead "
         "(element:addEventListener('click', fn)) or use data-event-click with a data model."},
        {codes::Javascript, E, false, "javascript", "JavaScript not run",
         "A <script> with JavaScript was removed: the engine has no JavaScript engine. Port the logic to "
         "<script type=\"text/lua\"> with the element API (document:getElementById, addEventListener, ...)."},
        {codes::KnownScript, W, true, "known-script", "Known library script",
         "A script of a recognized library was removed with a tailored note: Lucide icons and Lottie players are built into "
         "the engine (Info); Tailwind's Play CDN needs a built stylesheet, animation and framework libraries need the Lua "
         "anime API or a Lua port (Warning)."},
        {codes::InputTypeApproximated, W, true, "input-type-approximated", "Input type approximated",
         "The input type is rendered as an approximation (for example a date field as a text field with a format "
         "placeholder, no picker). The severity depends on how visible the difference is."},
        {codes::FormNavigation, I, false, "form-navigation", "Form navigation ignored",
         "Form action, method and target are ignored: submitting a form dispatches the submit event with the field values, "
         "which a Lua listener handles."},
        {codes::LinkSkipped, I, false, "link-skipped", "Link skipped",
         "A <link> or <style> was skipped because it is disabled or has a rel or type the engine does not load."},
        {codes::DuplicateId, W, false, "duplicate-id", "Duplicate id",
         "The same id appears on more than one element; getElementById and #id selectors find the first one. Make ids unique."},
        {codes::NoscriptRendered, I, false, "noscript-rendered", "noscript rendered",
         "Scripting is off from HTML's point of view, so the content of <noscript> is shown."},
        {codes::ReservedName, W, false, "reserved-name", "Reserved name",
         "The document used a name the compatibility layer reserves (id htmlui-root, the htmlbody element, data-htmlui-* "
         "attributes); it was renamed. Pick another name."},
        {codes::BindingAttributeOutsideModel, W, false, "binding-attribute-outside-model", "Binding attribute outside a data model",
         "RmlUi reads this data-* attribute as a data binding (data-if, data-for, data-event-*, ...), but no data-model "
         "encloses it. Rename the attribute, or wrap the element in a data-model."},
        {codes::TitleTooltip, I, false, "title-tooltip", "title tooltip not shown",
         "No longer reported (S17): title attributes show a tooltip after a hover or focus delay (UIDocument.tooltip.configure, "
         "tooltip-delay; tooltip=\"#id\" and el:setTooltip for rich ones). Documents compiled before reported it when the engine "
         "showed no tooltips."},
        {codes::BaseIgnored, W, false, "base-ignored", "<base> ignored",
         "<base href> is ignored: URLs resolve against the file that references them."},
        {codes::ImageCandidateChosen, I, false, "image-candidate-chosen", "Image candidate chosen",
         "srcset/sizes or <picture> sources were resolved once, at load, for the canvas size and pixel ratio; the note names "
         "the chosen file."},
        {codes::UnknownCustomElement, I, false, "unknown-custom-element", "Unknown custom element",
         "An undefined custom element renders as a generic inline element, as in a browser without its JavaScript definition."},
        {codes::TabindexOrder, W, false, "tabindex-order", "tabindex order ignored",
         "A positive tabindex makes the element focusable, but the order is the document order. Reorder the elements."},
        {codes::FieldsetDisabled, I, false, "fieldset-disabled", "Fieldset disabled",
         "disabled on a <fieldset> was propagated to every control inside it, as browsers do."},
        {codes::UnknownLucideIcon, W, false, "unknown-lucide-icon", "Unknown Lucide icon",
         "The data-lucide name is not in the built-in Lucide icon pack; the element stays empty. The suggestion lists the "
         "closest names."},
        {codes::LuaScriptError, E, false, "lua-script-error", "Lua script error",
         "A <script type=\"text/lua\"> failed to compile or raised an error; the location is the line in the HTML file."},
        {codes::LinkNotNavigable, I, false, "link-not-navigable", "Link not navigable",
         "The link points outside the project's documents (http(s), mailto:, javascript:, ...): clicking it only dispatches "
         "the htmlnavigate event."},
        {codes::UnsupportedImageFormat, W, false, "unsupported-image-format", "Unsupported image format",
         "The engine cannot decode this image format (WebP, AVIF, ...). Convert the image to PNG or JPEG (or SVG)."},
        {codes::DialogNotModal, I, false, "dialog-not-modal", "Dialog not modal",
         "Dialogs open and close, but showModal() has no backdrop and does not trap the focus. Add a backdrop element styled "
         "in CSS if needed."},
        {codes::SelfClosingTag, W, false, "self-closing-tag", "Self-closing tag",
         "In HTML, <div/> (any element that is not void, such as img, br or input) does not close the element: everything "
         "after it goes inside it until its parent ends, which changes the layout. Browsers do the same. Write <div></div>."},
        {codes::UnsupportedMediaFormat, W, true, "unsupported-media-format", "Unsupported media format",
         "A <video> (or <source>) names a file the engine cannot play: the engine plays WebM with AV1 video and Opus audio on "
         "every platform (MP4, MOV, Ogg, H.264, HEVC, VP8, VP9 and Theora are not decoded). The source is skipped, and the "
         "video shows its poster or nothing. Convert the file: MCP import_files and the Project window convert videos with "
         "a user-installed FFmpeg (SVT-AV1 or libaom, libopus). Error when no source of the <video> can be played."},
        {codes::MediaAttributeIgnored, I, false, "media-attribute-ignored", "Media attribute ignored",
         "A <video> attribute or content without an engine equivalent was dropped: playsinline (videos always play inline), "
         "crossorigin, controlslist, disablepictureinpicture, disableremoteplayback, x-webkit-airplay, mediagroup, and the "
         "fallback content inside <video> (browsers that play video never show it either)."},
        {codes::VirtualViewHeight, W, false, "virtual-view-height", "Virtual view without a definite height",
         "A <list-view>, <grid-view>, <tree-view> or <table-view> grew to the height of all its rows: it has no definite "
         "height, so it cannot scroll its rows. It lays out 20 rows instead. Give it a height (height, max-height, or flex: 1 "
         "inside a container of a definite height)."},
        {codes::VirtualViewTemplate, E, false, "virtual-view-template", "Virtual view template",
         "A <list-view>, <grid-view>, <tree-view> or <table-view> needs exactly one <template> child: the markup of a row "
         "(a table-view's cells come from its <table-column> children instead, each with an optional <template>). The view "
         "shows no rows without one, and only the first of several."},
        {codes::LargeDataFor, I, false, "large-data-for", "Large data-for",
         "A data-for made more than 500 elements: every one of them is laid out and drawn. A <list-view items=\"...\"> (or "
         "grid-view, table-view) makes only the rows on screen and keeps a 10,000-row list fast on mobile."},
        {codes::MarkdownRawHtml, I, false, "markdown-raw-html", "HTML in Markdown",
         "The Markdown of a <markdown> element holds HTML markup. Markdown is rendered without HTML passthrough: the markup "
         "shows as its text (text from servers or players is safe this way). Write Markdown, or put the HTML outside the "
         "<markdown> element."},
        {codes::ParticleSystemSource, W, false, "particle-system-source", "Particle system source missing",
         "A <particle-system src=\"...\"> names no prefab of the build, or a prefab whose GameObjects have no Particle "
         "System (with an Action): the element draws nothing. Name a .prefab of the project that has one (MCP "
         "create_particles then create_prefab, or the Hierarchy's Particle System saved as a prefab); prefabs exist in Play "
         "and in exported games, not in edit mode."},

        // ---- CSS
        {codes::UnknownProperty, W, false, "unknown-property", "Unknown property",
         "The name is not a CSS property (a typo, or a property of another product). The suggestion is the closest known "
         "property."},
        {codes::UnsupportedProperty, W, true, "unsupported-property", "Unsupported property",
         "The CSS property is not supported by the engine and was dropped (Info when the difference is minor). The suggestion "
         "says what to write instead."},
        {codes::IgnoredProperty, I, false, "ignored-property", "Ignored property",
         "The property is accepted and dropped because it has no effect in a game UI (printing, speech, browser-only hints)."},
        {codes::UnsupportedValue, W, false, "unsupported-value", "Unsupported value",
         "The property is supported but this value is not; the declaration was dropped, as browsers drop invalid declarations. "
         "The suggestion names a supported value."},
        {codes::ApproximatedMinor, I, false, "approximated-minor", "Approximated (minor)",
         "The declaration was rewritten with a small difference from the browser result (the note says which)."},
        {codes::ApproximatedVisible, W, false, "approximated-visible", "Approximated (visible)",
         "The declaration was rewritten with a visible difference from the browser result (for example a dashed border drawn "
         "solid). The suggestion gives an exact alternative when there is one."},
        {codes::ForkFallback, W, true, "fork-fallback", "Fork fallback",
         "The feature needs a patch of the engine's layout library that is not present yet; a fallback rewrite runs until it "
         "lands. The note describes the fallback."},
        {codes::CssSyntaxError, W, false, "css-syntax-error", "CSS syntax error",
         "The CSS has a syntax error (an unclosed block, a bad string or URL, an invalid declaration or rule); it was recovered "
         "the way browsers recover it, which usually drops the broken part."},
        {codes::VendorPrefix, I, false, "vendor-prefix", "Vendor prefix",
         "A -webkit-/-moz-/-ms-/-o- prefixed property or value was mapped to its standard form, or dropped as a duplicate of "
         "the unprefixed declaration in the same block."},
        {codes::ImportantMayLose, W, false, "important-may-lose", "!important may lose",
         "!important is emulated by moving the declaration after every author rule; a rule with a higher specificity that sets "
         "the same property can still win. The message names that rule; raise the specificity or remove the conflict."},
        {codes::InheritApproximated, I, false, "inherit-approximated", "inherit approximated",
         "inherit on a non-inherited property copies the nearest ancestor's declared value as text, so relative units resolve "
         "at the element instead of the parent."},
        {codes::VarUntyped, W, false, "var-untyped", "var() cannot be typed",
         "The property needs a rewrite, and the var() it uses has no declaration in the document, or declarations of different "
         "kinds; the value is passed on unchanged and may be rejected. Declare the variable in the document with one kind of "
         "value."},
        {codes::ViewportDependent, I, false, "viewport-dependent", "Viewport-dependent value",
         "The value was evaluated for the current canvas size (or font size) at load; it is evaluated again when the canvas "
         "is resized."},
        {codes::UnsupportedUnit, W, false, "unsupported-unit", "Unsupported unit",
         "The unit is not supported (container query units, ...); the declaration was dropped."},
        {codes::UnsupportedFunction, W, false, "unsupported-function", "Unsupported function",
         "The CSS function is not supported (attr(), element(), cross-fade(), paint(), ...); the declaration or layer was "
         "dropped."},
        {codes::ColorUnresolvable, W, false, "color-unresolvable", "Color unresolvable",
         "A color function (color-mix(), relative colors) has operands that are only known at run time (var()); it cannot be "
         "computed ahead and the declaration was dropped. Use static colors."},
        {codes::MergedFromBaseRule, I, false, "merged-from-base-rule", "Merged from a base rule",
         "A longhand of a property the engine only has as a shorthand (transition, animation, background layers, individual "
         "transforms) was merged with the shorthand of a base rule, named in the message."},
        {codes::LonghandWithoutBase, W, false, "longhand-without-base", "Longhand without base rule",
         "A longhand of a property the engine only has as a shorthand was set in a rule with no matching base rule to merge "
         "with; it was dropped. Write the shorthand in the same rule."},
        {codes::EasingApproximated, I, false, "easing-approximated", "Easing approximated",
         "The easing function was replaced by the nearest built-in tween; the message names it and the largest error."},
        {codes::StepsApproximated, W, false, "steps-approximated", "steps() approximated",
         "steps(), step-start and step-end are replaced by a linear easing until the layout library supports them."},
        {codes::AnimationApproximated, W, true, "animation-approximated", "Animation approximated",
         "Part of an animation is approximated: per-keyframe easing dropped, a fractional iteration count rounded, the "
         "backwards fill mode not applied during the delay, ..."},
        {codes::FontSource, W, true, "font-source", "Font source",
         "An @font-face source was skipped (SVG/EOT, local(): Info), or the rule has no supported vector or bitmap font "
         "(.bitmapfont, .fnt) source and was dropped (Warning). Provide TTF, OTF, WOFF, WOFF2 or a bitmap font; remote fonts are imported by "
         "the editor."},
        {codes::UnknownFontFamily, I, false, "unknown-font-family", "Unknown font family",
         "No @font-face declares the family and the engine does not know it; the text uses the next family of the list or the "
         "engine's fallback font."},
        {codes::BitmapFontEffect, W, false, "bitmap-font-effect", "Blurred effect on a bitmap font",
         "A rule with a bitmap font family (.bitmapfont or .fnt @font-face) has a glow, a blur or a blurred text-shadow: a pixel "
         "font draws only pixel outlines and sharp shadows, so the effect is dropped. Use font-effect: outline(1px <colour>) or a "
         "text-shadow without blur."},

        // ---- SEL
        {codes::UnsupportedSelector, W, false, "unsupported-selector", "Unsupported selector",
         "The selector uses a feature the engine cannot match (:has(), form validation states, ...); the rule was dropped. Set "
         "a class from Lua instead."},
        {codes::SelectorRewritten, I, false, "selector-rewritten", "Selector rewritten",
         "A selector feature was mapped to an engine equivalent (:lang() to an attribute selector, :defined removed, ...)."},
        {codes::PseudoElementPending, W, false, "pseudo-element-pending", "Pseudo-element pending",
         "::before and ::after need a patch of the engine's layout library that is not present yet; the rule was dropped. Add "
         "a real element instead."},
        {codes::IsWhereExpanded, I, false, "is-where-expanded", "is()/where() expanded",
         ":is() and :where() were expanded into a selector list; :where() keeps the specificity of its argument instead of zero."},
        {codes::SelectorExpansionLimit, W, false, "selector-expansion-limit", "Selector expansion limit",
         "Flattening nesting or :is() produced more than 256 selectors for one rule; the rule was dropped. Simplify the "
         "selectors."},
        {codes::VisitedDropped, I, false, "visited-dropped", ":visited dropped",
         "There is no browsing history, so :visited never matches; the rule was dropped."},
        {codes::InvalidSelector, W, false, "invalid-selector", "Invalid selector",
         "The selector has a syntax error; the rule was dropped, as in a browser."},
        {codes::SelectorIgnored, I, false, "selector-ignored", "Selector ignored",
         "A selector feature that cannot apply in a game UI (:dir(), an unknown vendor-prefixed pseudo-class or pseudo-element "
         "Chromium also drops) was dropped with its selector or rule."},

        // ---- AT
        {codes::UnsupportedAtRule, W, true, "unsupported-at-rule", "Unsupported at-rule",
         "The at-rule is not supported and was dropped with its contents (@container, @scope, @starting-style: Warning; "
         "@page, @namespace, @counter-style, ...: Info), or an @import came after other rules."},
        {codes::LayerFlattened, I, false, "layer-flattened", "@layer flattened",
         "@layer blocks were flattened in layer order; a layered rule with a higher specificity can still beat an unlayered "
         "rule for the property named in the message."},
        {codes::SupportsEvaluated, I, false, "supports-evaluated", "@supports evaluated",
         "The @supports condition was evaluated against the engine's support tables; the message gives the result."},
        {codes::MediaFeatureUnsupported, W, false, "media-feature-unsupported", "Media feature unsupported",
         "The media condition uses a feature or a negation the engine cannot express; the block never matches."},
        {codes::MediaNormalized, I, false, "media-normalized", "Media query normalized",
         "The media query was rewritten to the engine's syntax (range syntax to min-/max-, comma lists to separate blocks, "
         "prefers-color-scheme to themes, ...)."},
        {codes::MediaStatic, I, false, "media-static", "Media feature evaluated statically",
         "The media feature has one fixed value in the engine (hover, pointer, color, ...); the message gives the value used."},
        {codes::ImportProblem, W, true, "import-problem", "@import problem",
         "An @import cycle (Error: the repeated import is dropped), a missing imported file, or an @import nested too deeply "
         "(Warning)."},
        {codes::NestingFlattened, I, false, "nesting-flattened", "Nesting flattened",
         "Nested rules were flattened into top-level rules."},
        {codes::KeyframesInvalid, W, false, "keyframes-invalid", "Invalid keyframes",
         "A keyframe selector or the content of a keyframe block is invalid; it was dropped."},
        {codes::PropertyApproximated, I, false, "property-approximated", "@property approximated",
         "@property is emulated by setting the initial value on the root element; inherits: false cannot be honored."},
        {codes::FontFaceDescriptor, I, false, "font-face-descriptor", "@font-face descriptor",
         "An @font-face descriptor was dropped or approximated (unicode-range, font-display, variable weight ranges, ...)."},
        {codes::AtPropertyUnsupportedSyntax, W, false, "at-property-unsupported-syntax", "@property syntax not supported",
         "The @property rule's syntax is not one the engine registers (the types length, number, integer, percentage, "
         "length-percentage, color, angle and time, or *): combinations, lists and other types are not. The custom property "
         "falls back to an unregistered one with the initial value written on the root (it inherits and changes discretely), "
         "as without the fork. The same applies when the live documents register more custom properties than the engine "
         "has property ids for (a few thousand; a document that unloads gives its ids back)."},
        {codes::AtPropertyConflict, W, false, "at-property-conflict", "@property conflict",
         "Another document loaded at the same time registered the name first with another syntax, inherits or "
         "initial-value. A name has one definition in the engine at a time, so the first live document's applies to both; "
         "this document's own definition applies once that document unloads or registers the same definition. A hot "
         "reload of one document never conflicts with its own earlier version. Give both rules the same definition (a "
         "shared stylesheet), or rename one of the properties."},
        {codes::AtPropertyInvalid, E, true, "at-property-invalid", "@property invalid",
         "Error: the @property rule is invalid and dropped, as in CSS: the name is not a custom property name, a descriptor "
         "(syntax, inherits, initial-value) is missing or invalid, the initial value does not match the syntax or is not "
         "computationally independent (em, rem, %, var()), or the rule is nested in a style rule. Warning: a declaration of a "
         "registered custom property does not match its syntax; it is invalid at computed-value time, so the property takes "
         "its initial value (inherited: its parent's value)."},

        // ---- IMP
        {codes::NotImported, E, false, "not-imported", "Remote resource not imported",
         "The document references a remote URL that has no entry in the project's import manifest "
         "(assets/htmlui-imports/manifest.json); a fallback is used. The editor imports it when it loads the document "
         "(Editor Settings > HTML UI > Import Remote Resources), and so do jasperengine-editor import-ui and MCP import_files "
         "from_document; the runtime never downloads."},
        {codes::DownloadFailed, E, false, "download-failed", "Download failed",
         "The editor could not download a remote resource (network error, HTTP error, timeout, unknown Google Fonts family)."},
        {codes::UnsupportedContent, W, false, "unsupported-content", "Unsupported content",
         "The downloaded content has a type the engine cannot use (WebP, AVIF, ...); it was saved but will not render."},
        {codes::Imported, I, false, "imported", "Imported",
         "A remote resource was downloaded into assets/htmlui-imports/ and recorded in the manifest."},
        {codes::CurlMissing, E, false, "curl-missing", "curl missing",
         "The editor downloads through the curl executable, which was not found. Install curl and make sure it is on the PATH."},
        {codes::FontVariantUnavailable, W, false, "font-variant-unavailable", "Font variant unavailable",
         "The font provider does not offer a requested weight or style; the text falls back to the nearest available one."},
        {codes::WoffOnly, W, false, "woff-only", "WOFF only",
         "Legacy diagnostic, no longer emitted: WOFF and WOFF2 fonts are decoded at runtime."},
        {codes::LicenseNote, I, false, "license-note", "License note",
         "The license of an imported resource was recorded, or it is unknown and must be verified before shipping."},
        {codes::ManifestFileMissing, W, false, "manifest-file-missing", "Manifest file missing",
         "An import manifest entry points to a file that does not exist; re-import the resource (load the document in the "
         "editor, jasperengine-editor import-ui, or MCP import_files from_document)."},
        {codes::VideoNotConverted, W, false, "video-not-converted", "Video not converted",
         "The importer kept a video the engine cannot play because no FFmpeg with an AV1 encoder was found (JASPER_FFMPEG, "
         "the editor setting, then PATH). Install FFmpeg with SVT-AV1 (or libaom-av1, rav1e) and libopus and import it "
         "again, or run the command in the message. FFmpeg is the user's own tool, run as a separate program; the engine "
         "never links or ships it."},
        {codes::VideoConversionFailed, E, false, "video-conversion-failed", "Video conversion failed",
         "FFmpeg could not convert the video to AV1 + Opus WebM (an unreadable or damaged input, or an encoder error); the "
         "message has FFmpeg's last lines. The original is not imported."},

        // ---- SVG
        {codes::SvgRecolor, I, false, "svg-recolor", "SVG recolored",
         "The SVG mixes currentColor with fixed colors; it is rasterized again whenever the element's color changes."},
        {codes::SvgUnsupportedContent, W, false, "svg-unsupported-content", "SVG content unsupported",
         "The SVG contains content the renderer cannot draw (foreignObject, ...); it was dropped."},
        {codes::SvgInternalCss, I, false, "svg-internal-css", "CSS inside SVG",
         "CSS rules that target elements inside an SVG are not applied; style the shapes with SVG attributes, or set fill and "
         "stroke on the <svg> element."},

        // ---- FX (phase 5: the UI effects, effect() and @effect)
        {codes::EffectUnknown, E, false, "effect-unknown", "Unknown effect",
         "effect() names no built-in effect and no effect declared with @effect; the layer or filter is dropped. The suggestion "
         "lists the nearest names and the effects made for what the name describes. Every effect is listed in "
         "jasper://docs/html-ui-effects."},
        {codes::EffectWrongContext, E, false, "effect-wrong-context", "Effect in the wrong property",
         "A paint effect goes in background, background-image, mask-image, filter or backdrop-filter; a filter effect only in "
         "filter or backdrop-filter; effect() in any other property is dropped. The suggestion says where the effect is allowed "
         "and names its paint or filter counterpart when there is one."},
        {codes::EffectPositionalArg, E, false, "effect-positional-arg", "Effect argument without a name",
         "Every argument of effect() is named: effect(aurora, speed 2), not effect(aurora, 2). The argument is dropped; the "
         "suggestion lists the effect's parameters."},
        {codes::EffectUnknownParam, W, false, "effect-unknown-param", "Unknown effect parameter",
         "The effect has no parameter of that name; the argument is ignored. The suggestion names the nearest parameter."},
        {codes::EffectBadValue, W, false, "effect-bad-value", "Bad effect parameter value",
         "The value does not parse as the parameter's type (number, length, angle, time, colour, position, keyword, palette); "
         "the default is used. The suggestion gives the expected type and range."},
        {codes::EffectOutOfRange, W, false, "effect-out-of-range", "Effect parameter out of range",
         "The value is outside the parameter's range and was clamped to it."},
        {codes::EffectDuplicateParam, W, false, "effect-duplicate-param", "Duplicate effect parameter",
         "The parameter is given more than once; the last value wins."},
        {codes::EffectBlendOnFilter, W, false, "effect-blend-on-filter", "blend on a filter effect",
         "The blend parameter applies to paint effects; a filter effect ignores it. Remove it."},
        {codes::EffectIgnoredBackgroundProp, W, false, "effect-ignored-background-prop", "Background property ignored for an effect layer",
         "background-size, background-position and background-repeat do not apply to an effect layer: an effect always covers "
         "its paint area. Use the effect's own parameters (scale, center, ...)."},
        {codes::EffectUnsupportedBlend, W, false, "effect-unsupported-blend", "Blend mode not supported for an effect layer",
         "An effect layer takes background-blend-mode normal, multiply, screen and plus-lighter (also lighter); other modes are "
         "drawn as normal."},
        {codes::EffectTextFill, I, false, "effect-text-fill", "Text colour set for an effect fill",
         "background-clip: text with an effect layer fills the content with the effect: the transparent text colour was set to "
         "white so the text has an alpha to fill. Unlike a browser, all of the element's content is filled, not only its glyphs."},
        {codes::EffectDeclareConflict, E, false, "effect-declare-conflict", "Effect declaration conflict",
         "@effect declares a name that is a built-in effect, or that another @effect declared with another src; the first "
         "declaration wins (effect names are global to the game). Rename the effect."},
        {codes::EffectFileMissing, E, false, "effect-file-missing", "Effect file not found",
         "The src of @effect names no file; src resolves against the stylesheet like url(). The message has the path checked."},
        {codes::EffectManifest, E, false, "effect-manifest", "Invalid effect manifest",
         "The project effect's .frag has an invalid manifest (the // @ lines after #version 450), reported at its line: a "
         "missing @effect or @kind, an unknown directive or type, a default outside its range, a main include that does not "
         "match the kind, or an @effect name other than the one CSS declares."},
        {codes::EffectShaderCompile, E, false, "effect-shader-compile", "Effect shader does not compile",
         "The editor could not compile the project effect's fragment shader; the compiler messages are mapped to lines of the "
         ".frag. The effect draws nothing until it compiles."},
        {codes::EffectCost, I, false, "effect-cost", "Many expensive effects",
         "The document uses more than four high-cost effects; each costs a lot per pixel. Keep high-cost effects to a couple of "
         "elements or a quarter of the screen (the guide's performance section). Stateful effects count too: more than four "
         "in one document is reported."},
        {codes::EffectStateUnsupported, E, false, "effect-state-unsupported", "Effect state format not supported",
         "A stateful effect keeps its simulation in float images (rgba16f, rg16f, r16f, rgba32f, r32f) this device cannot "
         "render to (or blend into): GLES3 and WebGL2 need EXT_color_buffer_float (EXT_color_buffer_half_float for the "
         "16-bit ones). The effect is not drawn (a filter leaves the content); use a stateless effect as the fallback for such devices."},
        {codes::EffectStatefulBudget, W, true, "effect-stateful-budget", "Too many stateful effects",
         "More stateful simulations are active than the budget (8 per document, 32 in all, 64 MB of state per document): each "
         "runs its passes every frame. A warning while over the soft budget; an error when the hard cap (64 simulations) "
         "refuses a new one, which then draws nothing. Hide the elements whose simulations should stop: a simulation that is "
         "not drawn does not step."},
    };

    char toLower(char c){
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    bool equalsIgnoreCase(std::string_view a, std::string_view b){
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++){
            if (toLower(a[i]) != toLower(b[i]))
                return false;
        }
        return true;
    }

}

std::string DiagCode::id() const{
    // "HTMLUI-%s-%04u"
    std::string text = "HTMLUI-";
    text += diagAreaName(area);
    text += '-';
    char digits[8];
    int count = 0;
    unsigned int n = number;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    for (int i = count; i < 4; i++)
        text += '0';
    while (count > 0)
        text += digits[--count];
    return text;
}

Span<const DiagCodeInfo> jasperengine::htmlui::compat::diagnosticCodes(){
    return Span<const DiagCodeInfo>(registry);
}

const DiagCodeInfo* jasperengine::htmlui::compat::findDiagnosticCode(std::string_view idOrName){
    for (const DiagCodeInfo& info : registry){
        if (idOrName == info.name || equalsIgnoreCase(idOrName, info.code.id()))
            return &info;
    }
    return nullptr;
}

const DiagCodeInfo* jasperengine::htmlui::compat::findDiagnosticCode(DiagCode code){
    // the registry is sorted by area then number (compat_codes_test)
    const DiagCodeInfo* begin = std::begin(registry);
    const DiagCodeInfo* end = std::end(registry);
    const DiagCodeInfo* it = std::lower_bound(begin, end, code, [](const DiagCodeInfo& info, const DiagCode& c){ return info.code < c; });
    if (it != end && it->code == code)
        return it;
    // (a registry out of order would still be searched)
    for (const DiagCodeInfo& info : registry){
        if (info.code == code)
            return &info;
    }
    return nullptr;
}

const char* jasperengine::htmlui::compat::diagAreaName(DiagArea area){
    switch (area){
        case DiagArea::GEN: return "GEN";
        case DiagArea::HTML: return "HTML";
        case DiagArea::CSS: return "CSS";
        case DiagArea::SEL: return "SEL";
        case DiagArea::AT: return "AT";
        case DiagArea::IMP: return "IMP";
        case DiagArea::SVG: return "SVG";
        case DiagArea::FX: return "FX";
    }
    return "GEN";
}

const char* jasperengine::htmlui::compat::severityName(Severity severity){
    switch (severity){
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "info";
}
