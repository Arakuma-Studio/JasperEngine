// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Compat consistency with the fork (phase-3 spec 5.4). Every fork feature (compat::ForkFeature,
// phase-2 §3.6, phase-3 Appendix A) has sample declarations or selectors below:
//
//   present (ForkFeatures::present() has it): RmlUi parses the sample, compat passes it through
//            (normalized: RmlUi parses compat's output to the same properties) and reports nothing;
//   absent  (forced off with ForkFeatures::with): compat still produces phase 2's rewrite or
//            emulation (the code is kept, spec §1), seen in its output or its diagnostic.
//
// A table row whose support becomes Native with the present features (effectiveSupport) needs a
// sample of its feature and property here, so a wave-1 package that flips a feature (kPresent)
// adds the samples of the rows it makes native in the same merge ("every package adds rows").

#include "CompatTestCommon.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/CssRewriter.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    enum class SampleKind {
        Declaration,   // a style="" declaration (CssRewriter::rewriteDeclarations)
        Selector,      // a selector (CssRewriter::rewriteSelector)
        Rule,          // the declaration in a stylesheet rule ".sample { ... }" (an inline <style>)
        BeforeRule,    // the same in ".sample::before { ... }" (generated content: 'content' applies there)
        Sheet,         // a whole stylesheet (at-rules: @property); present: no diagnostic, RmlUi parses the output and
                       // phase 2's piece (absentOutput) is gone from it
    };

    struct FeatureSample {
        ForkFeature feature;
        SampleKind kind;
        const char* css;        // "border-top: 2px dashed red" or ".a::before"
        // absent: a piece of compat's output (normalized; for a Rule, of the rule's block and of
        // compat's !important sheet), or "" when phase 2 leaves the CSS as it is and the
        // difference is its diagnostic `code` (or nothing at all: GroupOpacity, with code "" too:
        // phase 2 documents it and does not rewrite)
        const char* absentOutput;
        const char* absentCode;
    };

    // One or more per feature; append the samples of the rows a feature makes Native.
    const FeatureSample kSamples[] = {
        {ForkFeature::Calc, SampleKind::Declaration, "width: calc(10px + 20px)", "width:30px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-top: 2px dashed red", "border-top-width:2px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border: 3px double blue", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-right: 1px dotted #00ff00", "border-right-width:1px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-bottom: 6px groove red", "border-bottom-width:6px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-left: 2px ridge red", "border-left-width:2px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-style: dashed solid", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-top-style: inset", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-right-style: outset", "border-right-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-bottom-style: hidden", "border-bottom-width:0px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-left-style: none", "border-left-width:0px", ""},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-radius: 50%", "9999px", ""},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-radius: 10% 20px 30% 4px", "", "HTMLUI-CSS-0007"},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-radius: 10px 20px / 5px 50%", "border-radius:10px20px", "HTMLUI-CSS-0006"},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-top-left-radius: 50%", "border-top-left-radius:9999px", ""},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-top-right-radius: 25%", "", "HTMLUI-CSS-0007"},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-bottom-right-radius: 10px 20px", "", "HTMLUI-CSS-0004"},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-bottom-left-radius: 20% 40%", "", "HTMLUI-CSS-0004"},
        {ForkFeature::Outline, SampleKind::Declaration, "outline: 2px dashed red", "--html-outline-shadow:", ""},
        {ForkFeature::Outline, SampleKind::Declaration, "outline: auto", "--html-outline-shadow:", ""},
        {ForkFeature::Outline, SampleKind::Declaration, "outline-style: double", "--html-outline-shadow:", ""},
        {ForkFeature::Outline, SampleKind::Declaration, "outline-width: 4px", "--html-outline-shadow:", ""},
        {ForkFeature::Outline, SampleKind::Declaration, "outline-color: #00ff00", "--html-outline-shadow:", ""},
        {ForkFeature::Outline, SampleKind::Declaration, "outline-offset: -3px", "--html-outline-shadow:", ""},
        {ForkFeature::ObjectFit, SampleKind::Declaration, "object-fit: cover", "", ""},
        {ForkFeature::ObjectFit, SampleKind::Declaration, "object-position: right 10px bottom 20%", "", "HTMLUI-CSS-0004"},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background: linear-gradient(#ff0000ff, #0000ffff) 10px 20px / 30px 40px repeat-x padding-box content-box #000000ff", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-image: linear-gradient(#ff0000ff, #0000ffff), none", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-image: linear-gradient(currentcolor, #0000ffff)", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-position: 10px 20%", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-position-x: 10px", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-position-y: 20%", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-size: 30px auto", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-repeat: space round", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-origin: content-box", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-clip: padding-box", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "background-attachment: fixed", "decorator:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "mask: linear-gradient(#000000ff, #00000000) 0% 0% / 10px 10px repeat-x", "mask-image:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "mask-image: linear-gradient(#000000ff, #00000000)", "mask-image:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "mask-position: 10px 20%", "mask-image:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "mask-size: contain", "mask-image:", ""},
        {ForkFeature::BackgroundLayers, SampleKind::Declaration, "mask-repeat: no-repeat", "mask-image:", ""},
        {ForkFeature::PseudoElements, SampleKind::Selector, ".a::before", "", "HTMLUI-SEL-0003"},
        {ForkFeature::RootIsWhere, SampleKind::Selector, ":is(.a, .b) > .c", ".a>.c,.b>.c", ""},
        {ForkFeature::Fixed, SampleKind::Declaration, "position: fixed", "", "HTMLUI-CSS-0007"},
        {ForkFeature::GroupOpacity, SampleKind::Declaration, "opacity: 0.5", "", ""},
        {ForkFeature::GroupOpacity, SampleKind::Declaration, "isolation: isolate", "", "HTMLUI-CSS-0003"},
        {ForkFeature::GroupOpacity, SampleKind::Declaration, "will-change: transform, opacity", "", "HTMLUI-CSS-0003"},
        {ForkFeature::CssWideKeywords, SampleKind::Declaration, "all: unset", "margin-top:var(--html-unset)", ""},
        {ForkFeature::RelativeFontWeight, SampleKind::Declaration, "font-weight: bolder", "font-weight:700", "HTMLUI-CSS-0005"},
        {ForkFeature::RelativeFontWeight, SampleKind::Declaration, "font-weight: lighter", "font-weight:100", "HTMLUI-CSS-0005"},
        {ForkFeature::ColorFunctions, SampleKind::Declaration, "color: hsl(var(--h) 50% 50%)", "", "HTMLUI-CSS-0016"},
        {ForkFeature::ColorFunctions, SampleKind::Declaration, "background-color: rgba(var(--rgb), 0.2)", "", "HTMLUI-CSS-0016"},
        {ForkFeature::ColorFunctions, SampleKind::Declaration, "border-top-color: color-mix(in srgb, var(--a) 40%, white)", "", "HTMLUI-CSS-0016"},
        {ForkFeature::CssWideKeywords, SampleKind::Declaration, "all: revert", "margin-top:var(--html-unset)", ""},
        // p5-infra: the gradient-text filter list of effect layers (P5-1); without the feature it is dropped
        {ForkFeature::TextFillFilter, SampleKind::Declaration, "-jfx-text-fill: jfx-pixelate(v(8px) v(0) v(0) v(100%) v(1) v(0s))", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TimingFunctions, SampleKind::Declaration, "transition: opacity 0.2s ease-in-out", "quadratic-in-out", ""},
        {ForkFeature::Important, SampleKind::Rule, "color: red !important", ".sample{color:#ff0000ff;", ""},
        {ForkFeature::CssWideKeywords, SampleKind::Declaration, "color: inherit", "var(", ""},
        {ForkFeature::Grid, SampleKind::Declaration, "display: grid", "display:flex", ""},
        // p3-cascade
        {ForkFeature::RootIsWhere, SampleKind::Selector, ".a :where(.b, .c)", ".a .b,.a .c", ""},
        {ForkFeature::Has, SampleKind::Selector, "li:has(> input:checked)", "", "HTMLUI-SEL-0001"},
        {ForkFeature::Has, SampleKind::Selector, ".card:has(+ .badge, .x)", "", "HTMLUI-SEL-0001"},
        {ForkFeature::FocusPseudoClasses, SampleKind::Selector, ".a:focus", ".a:html-focus", ""},
        {ForkFeature::FocusPseudoClasses, SampleKind::Selector, ".a:focus-visible", ".a:html-focus-visible", ""},
        {ForkFeature::FocusPseudoClasses, SampleKind::Selector, "form:focus-within", "form:focus", ""},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "input:enabled", "input:not(:disabled)", ""},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "[data-x=\"a\" i]", "[data-x=\"a\"]", "HTMLUI-SEL-0002"},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "li:nth-child(2 of .x)", "", "HTMLUI-SEL-0001"},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "a:any-link", "a[href]", ""},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "input:required:read-write", "input[required]:not([readonly])", ""},
        {ForkFeature::SelectorsLevel4, SampleKind::Selector, "input:default, :target, :indeterminate", "", "HTMLUI-SEL-0001"},
        {ForkFeature::PseudoElements, SampleKind::BeforeRule, "content: \"x\" attr(title) counter(item, upper-roman)", "", "HTMLUI-SEL-0003"},
        {ForkFeature::PseudoElements, SampleKind::Declaration, "counter-reset: item 2", "", "HTMLUI-CSS-0002"},
        {ForkFeature::PseudoElements, SampleKind::Declaration, "counter-increment: item", "", "HTMLUI-CSS-0002"},
        {ForkFeature::PseudoElements, SampleKind::Declaration, "counter-set: item 5", "", "HTMLUI-CSS-0002"},
        {ForkFeature::PseudoElements, SampleKind::Declaration, "quotes: \"<\" \">\"", "", "HTMLUI-CSS-0002"},
        {ForkFeature::ListMarkers, SampleKind::Selector, "li::marker", "li > marker", ""},
        {ForkFeature::ListMarkers, SampleKind::Declaration, "display: list-item", "display:block", ""},
        {ForkFeature::ListMarkers, SampleKind::Declaration, "list-style-type: lower-roman", "", ""},
        {ForkFeature::ListMarkers, SampleKind::Declaration, "list-style-position: inside", "", "HTMLUI-CSS-0005"},
        {ForkFeature::ListMarkers, SampleKind::Declaration, "list-style-image: none", "", ""},
        {ForkFeature::ListMarkers, SampleKind::Declaration, "list-style: square inside", "list-style-type:square", ""},
        // p3-grid
        {ForkFeature::Grid, SampleKind::Declaration, "display: inline-grid", "display:inline-flex", ""},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-template-columns: [a] repeat(3, minmax(40px, 1fr)) [b]", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-template-rows: 40px auto fit-content(30%)", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-template-areas: \"a b\" \"c d\"", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-template: \"h h\" 40px \"s m\" 1fr / 100px 1fr", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid: auto-flow dense 30px / repeat(auto-fill, 50px)", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-auto-columns: 50px", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-auto-rows: minmax(40px, auto)", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-auto-flow: column dense", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-area: 1 / 2 / 3 / 4", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-row: span 2 / -1", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-column: main", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-row-start: 2", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-row-end: span 3", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-column-start: col 2", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "grid-column-end: -1", "", "HTMLUI-CSS-0007"},
        // p3-text
        {ForkFeature::TextShadow, SampleKind::Declaration, "text-shadow: 1px 2px 3px #000000, 0 0 2px", "font-effect:", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "letter-spacing: 10%", "", "HTMLUI-CSS-0004"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "word-spacing: 0.25em", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-align: start", "text-align:left", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-decoration: underline dotted", "text-decoration:underline", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-decoration-line: underline overline", "text-decoration:underline", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-decoration-color: #ff0000", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-decoration-style: double", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-decoration-thickness: 2px", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-underline-offset: 3px", "", "HTMLUI-CSS-0002"},
        {ForkFeature::TextProperties, SampleKind::Declaration, "white-space: break-spaces", "white-space:pre-wrap", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "white-space-collapse: preserve", "white-space:pre", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-wrap-mode: nowrap", "white-space:nowrap", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "text-wrap: nowrap", "white-space:nowrap", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "overflow-wrap: anywhere", "word-break:break-word", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "word-wrap: break-word", "word-break:break-word", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "word-break: keep-all", "word-break:normal", ""},
        {ForkFeature::TextProperties, SampleKind::Declaration, "user-select: none", "", "HTMLUI-CSS-0003"},
        // p3-animation: the transition and animation properties as CSS writes them, translate / rotate / scale
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation: spin 1s ease-in 0.5s 2 reverse both paused", "spin-rev", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation: spin 2s 1.5", "", "HTMLUI-CSS-0021"},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-name: spin", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-duration: 200ms", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-timing-function: ease-in-out", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-delay: -0.5s", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-iteration-count: infinite", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-direction: alternate-reverse", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-fill-mode: both", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-play-state: paused", "-html-animation-fill:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "animation-composition: replace", "", "HTMLUI-CSS-0002"},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition: opacity 0.2s ease-in 0.1s allow-discrete, all 1s", "transition:opacity0.2sease-in0.1s,all1s", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition-property: opacity", "transition:none", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition-duration: 200ms", "transition:all0.2s", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition-timing-function: steps(4, jump-start)", "transition:", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition-delay: 0.5s", "transition:", ""},
        {ForkFeature::AnimationLonghands, SampleKind::Declaration, "transition-behavior: allow-discrete", "", "HTMLUI-CSS-0003"},
        {ForkFeature::IndividualTransforms, SampleKind::Declaration, "translate: 10px 20%", "transform:translate(10px,20%)", ""},
        {ForkFeature::IndividualTransforms, SampleKind::Declaration, "rotate: x 45deg", "transform:rotatex(45deg)", ""},
        {ForkFeature::IndividualTransforms, SampleKind::Declaration, "scale: 1.5 2", "transform:scale(1.5,2)", ""},
        // p3-values
        {ForkFeature::CurrentColor, SampleKind::Declaration, "border-top-color: currentcolor", "var(--html-current-color", ""},
        {ForkFeature::CurrentColor, SampleKind::Declaration, "background-color: currentColor", "var(--html-current-color", ""},
        {ForkFeature::CurrentColor, SampleKind::Declaration, "color: currentcolor", "var(--html-unset)", ""},
        {ForkFeature::CurrentColor, SampleKind::Declaration, "border-color: currentcolor", "var(--html-current-color", ""},
        {ForkFeature::CurrentColor, SampleKind::Declaration, "border-top-color: color-mix(in srgb, currentcolor 40%, blue)", "", "HTMLUI-CSS-0016"},
        {ForkFeature::CurrentColor, SampleKind::Declaration, "background-color: color-mix(in oklab, currentColor, white)", "", "HTMLUI-CSS-0016"},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "width: 10vmin", "", "HTMLUI-CSS-0013"},
        {ForkFeature::ExtraUnits, SampleKind::Rule, "height: 5vmax", "@media", ""},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "margin-left: 2ch", "margin-left:1em", "HTMLUI-CSS-0005"},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "padding-top: 1.5ex", "padding-top:0.75em", "HTMLUI-CSS-0005"},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "transform: rotate(0.25turn)", "rotate(90deg)", ""},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "width: 50dvw", "width:50vw", ""},
        // (in grid values, which the fork reads as written with Grid: track lists and gaps)
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "grid-template-columns: 4ch minmax(2ex, 1fr) calc(1ch + 10px)", "grid-template-columns:2em minmax(1em,1fr)", "HTMLUI-CSS-0005"},
        {ForkFeature::ExtraUnits, SampleKind::Rule, "grid-auto-rows: 10vmin", "@media", ""},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "gap: 1ch 2ex", "gap:0.5em 1em", "HTMLUI-CSS-0005"},
        // (in p3-text's spacing properties)
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "letter-spacing: 0.1ch", "letter-spacing:0.05em", "HTMLUI-CSS-0005"},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "word-spacing: 1ex", "word-spacing:0.5em", "HTMLUI-CSS-0005"},
        // (in p3-animation's translate / rotate / scale)
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "translate: 2ch 1ex", "translate:1em0.5em", "HTMLUI-CSS-0005"},
        {ForkFeature::ExtraUnits, SampleKind::Declaration, "rotate: 0.25turn", "rotate:90deg", ""},
        {ForkFeature::Calc, SampleKind::Declaration, "translate: calc(50% + 1em) 0px", "translate:50%", "HTMLUI-CSS-0007"},
        {ForkFeature::LineHeightNormal, SampleKind::Declaration, "line-height: normal", "line-height:1.2", ""},
        // p3-layout
        {ForkFeature::Sticky, SampleKind::Declaration, "position: sticky", "position:relative", ""},
        {ForkFeature::Sticky, SampleKind::Declaration, "overflow: clip", "overflow:hidden", ""},
        {ForkFeature::Sticky, SampleKind::Declaration, "overflow-clip-margin: 4px", "", "HTMLUI-CSS-0003"},
        {ForkFeature::Sticky, SampleKind::Declaration, "scrollbar-gutter: stable", "", "HTMLUI-CSS-0003"},
        {ForkFeature::BoxParity, SampleKind::Declaration, "aspect-ratio: 16 / 9", "", "HTMLUI-CSS-0002"},
        {ForkFeature::BoxParity, SampleKind::Declaration, "order: 2", "", "HTMLUI-CSS-0002"},
        {ForkFeature::BoxParity, SampleKind::Declaration, "flex-line-count: 2", "", "HTMLUI-CSS-0002"},
        {ForkFeature::Grid, SampleKind::Declaration, "justify-items: center", "", "HTMLUI-CSS-0007"},
        {ForkFeature::Grid, SampleKind::Declaration, "justify-self: end", "", "HTMLUI-CSS-0007"},
        {ForkFeature::BoxParity, SampleKind::Declaration, "width: fit-content", "width:auto", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "max-height: min-content", "max-height:none", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "min-width: auto", "min-width:0px", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "display: contents", "display:block", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "align-items: safe center", "align-items:center", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "justify-content: start", "justify-content:flex-start", ""},
        {ForkFeature::BoxParity, SampleKind::Declaration, "flex-basis: content", "", "HTMLUI-CSS-0004"},
        {ForkFeature::BoxParity, SampleKind::Declaration, "flex-wrap: balance", "", "HTMLUI-CSS-0004"},
        // p5-property: registered custom properties (phase 2: the initial value on the root, inherits: false lost)
        {ForkFeature::RegisteredProperties, SampleKind::Sheet,
         "@property --sample-len { syntax: '<length>'; inherits: false; initial-value: 4px; } .sample { width: var(--sample-len); }",
         "--sample-len:4px", "HTMLUI-AT-0010"},
        // p6-line-clamp (P6-L1): the fork's legacy line clamping (phase 2: a max-height cut, the legacy box a flex container)
        {ForkFeature::LineClamp, SampleKind::Declaration, "-webkit-line-clamp: 3", "max-height:3.6em", ""},
        {ForkFeature::LineClamp, SampleKind::Declaration, "display: -webkit-box", "display:flex", ""},
        {ForkFeature::LineClamp, SampleKind::Declaration, "display: -webkit-inline-box", "display:inline-flex", ""},
    };

    // lower case, no white space, no trailing ';'
    std::string normalized(std::string_view css){
        std::string out;
        for (char c : css){
            if (!std::isspace((unsigned char)c))
                out += (char)std::tolower((unsigned char)c);
        }
        while (!out.empty() && out.back() == ';')
            out.pop_back();
        return out;
    }

    struct Rewrite {
        std::string output;      // declarations (Declaration, Rule: the rule's block), or the selector
        std::string important;   // Rule: compat's !important sheet
        std::vector<Diagnostic> diagnostics;
    };

    Rewrite rewrite(const FeatureSample& sample, const ForkFeatures& fork){
        SourceFiles files;
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        Rewrite result;
        if (sample.kind == SampleKind::Selector){
            result.output = CssRewriter::rewriteSelector(sample.css, &sink, options);
        }else if (sample.kind == SampleKind::Declaration){
            result.output = CssRewriter::rewriteDeclarations(sample.css, &sink, options);
        }else{
            const std::string sheet = sample.kind == SampleKind::Sheet
                                          ? std::string(sample.css)
                                          : std::string(sample.kind == SampleKind::BeforeRule ? ".sample::before { " : ".sample { ") + sample.css + " }";
            const uint32_t file = files.add("ui/sample.html", sheet);
            CssRewriter rewriter(options, files, sink);
            StylesheetInput input;
            input.kind = StylesheetInput::Kind::Inline;
            input.file = file;
            input.text = files.text(file);
            const RewriteResult rewritten = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
            if (!rewritten.sheets.empty() && sample.kind == SampleKind::Sheet){
                result.output = rewritten.sheets[0].rcss;
            }else if (!rewritten.sheets.empty()){
                const std::string& rcss = rewritten.sheets[0].rcss;
                const size_t open = rcss.find('{'), close = rcss.rfind('}');
                if (open != std::string::npos && close != std::string::npos && close > open)
                    result.output = rcss.substr(open + 1, close - open - 1);
            }
            result.important = rewritten.importantSheet.rcss;
        }
        result.diagnostics = sink.diagnostics();
        return result;
    }

    bool hasCode(const std::vector<Diagnostic>& diagnostics, const std::string& code){
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d){ return d.code.id() == code; });
    }

    std::string describe(const Rewrite& rewrite){
        std::string text = "output '" + rewrite.output + "'";
        if (!rewrite.important.empty())
            text += ", !important sheet '" + rewrite.important + "'";
        for (const Diagnostic& d : rewrite.diagnostics)
            text += "; " + toText(d);
        return text;
    }

    // a declaration list as RmlUi parses it (custom properties, compat's companions, left out);
    // false when RmlUi rejects a declaration
    bool parseDeclarations(const std::string& css, Rml::PropertyDictionary& dictionary, std::string* rejected){
        size_t at = 0;
        while (at < css.size()){
            size_t end = css.find(';', at);
            if (end == std::string::npos)
                end = css.size();
            const std::string declaration = css.substr(at, end - at);
            at = end + 1;
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos)
                continue;
            std::string name = declaration.substr(0, colon), value = declaration.substr(colon + 1);
            name.erase(0, name.find_first_not_of(" \t\n"));
            name.erase(name.find_last_not_of(" \t\n") + 1);
            if (name.rfind("--", 0) == 0)
                continue;
            const size_t important = value.find("!important");
            if (important != std::string::npos)
                value.erase(important);
            if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value)){
                if (rejected)
                    *rejected = declaration;
                return false;
            }
        }
        return true;
    }

    // spec 5.4 (a)-(c) for one sample with the given features: "" when RmlUi parses it, compat
    // passes it through (normalized: the same properties and values as RmlUi parses them) and
    // reports nothing (no fallback, approximation or note); else what is wrong. Needs RmlUi
    // initialised.
    std::string passThroughProblem(const FeatureSample& sample, const ForkFeatures& fork){
        const Rewrite result = rewrite(sample, fork);
        if (!result.diagnostics.empty())
            return "a diagnostic: " + toText(result.diagnostics[0]);
        if (sample.kind == SampleKind::Sheet){
            if (!Rml::Factory::InstanceStyleSheetString(result.output))
                return "RmlUi does not parse compat's sheet: " + describe(result);
            return normalized(result.output).find(normalized(sample.absentOutput)) == std::string::npos ? std::string()
                                                                                                         : "phase 2's output: " + describe(result);
        }
        if (sample.kind == SampleKind::Selector){
            Rml::SharedPtr<Rml::StyleSheetContainer> container = Rml::Factory::InstanceStyleSheetString(std::string(sample.css) + " { color: red; }");
            if (!container)
                return "RmlUi does not parse the selector";
            return normalized(result.output) == normalized(sample.css) ? std::string() : "rewritten: " + describe(result);
        }
        Rml::PropertyDictionary expected, actual;
        std::string rejected;
        if (!parseDeclarations(sample.css, expected, &rejected))
            return "RmlUi rejects '" + rejected + "'";
        if (!parseDeclarations(result.output, actual, &rejected))
            return "RmlUi rejects compat's '" + rejected + "'";
        if (!result.important.empty())
            return "hoisted: " + describe(result);
        if (expected.GetNumProperties() != actual.GetNumProperties())
            return "other properties: " + describe(result);
        for (const auto& property : expected.GetProperties()){
            const Rml::Property* other = actual.GetProperty(property.first);
            if (!other || other->ToString() != property.second.ToString())
                return "other values: " + describe(result);
        }
        return std::string();
    }

}

TEST(NativeFeatureConsistency, EveryFeatureHasASample){
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        const ForkFeature feature = (ForkFeature)i;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){ return s.feature == feature; }))
            << forkFeatureName(feature) << " has no sample in NativeFeatureConsistencyTest.cpp";
    }
}

// The rows a present feature makes Native have a sample of that feature and property.
TEST(NativeFeatureConsistency, NativeRowsHaveSamples){
    const ForkFeatures present = ForkFeatures::present();
    size_t rows = 0;
    for (const PropertySupport& row : propertyTable()){
        if (row.fork == ForkFeature::None || !present.has(row.fork) || effectiveSupport(row, present) != Support::Native)
            continue;
        rows++;
        const std::string property = row.name;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){
            const std::string css = s.css;
            return s.feature == row.fork && s.kind != SampleKind::Selector && css.compare(0, property.size() + 1, property + ":") == 0;
        })) << "property " << row.name << " is Native with " << forkFeatureName(row.fork) << ": add a sample";
    }
    for (const SelectorSupport& row : selectorTable()){
        if (row.fork == ForkFeature::None || !present.has(row.fork) || effectiveSupport(row, present) != Support::Native)
            continue;
        rows++;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){ return s.feature == row.fork && s.kind == SampleKind::Selector; }))
            << "selector " << row.name << " is Native with " << forkFeatureName(row.fork) << ": add a selector sample";
    }
    std::cout << "[ NATIVE   ] " << rows << " table rows native through present fork features\n";
}

// The checker itself: phase 2 leaves `opacity` alone and rewrites `display: grid`.
TEST(NativeFeatureConsistency, PassThroughCheckSeesRewrites){
    ASSERT_TRUE(HtmlUI::acquire());
    const ForkFeatures none = ForkFeatures::none();
    EXPECT_EQ(passThroughProblem({ForkFeature::GroupOpacity, SampleKind::Declaration, "opacity: 0.5", "", ""}, none), "");
    EXPECT_EQ(passThroughProblem({ForkFeature::None, SampleKind::Declaration, "width: 10px; height: 20%", "", ""}, none), "");
    EXPECT_EQ(passThroughProblem({ForkFeature::None, SampleKind::Selector, ".a > .b:hover", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Grid, SampleKind::Declaration, "display: grid", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::RootIsWhere, SampleKind::Selector, ":is(.a, .b) > .c", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Important, SampleKind::Rule, "color: red !important", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Fixed, SampleKind::Declaration, "position: fixed", "", ""}, none), "");
    HtmlUI::release();
}

// present: RmlUi parses it, compat passes it through and reports nothing
TEST(NativeFeatureConsistency, PresentFeaturesPassThrough){
    const ForkFeatures present = ForkFeatures::present();
    ASSERT_TRUE(HtmlUI::acquire());
    size_t checked = 0;
    for (const FeatureSample& sample : kSamples){
        if (!present.has(sample.feature))
            continue;
        checked++;
        EXPECT_EQ(passThroughProblem(sample, present), "") << forkFeatureName(sample.feature) << ": " << sample.css;
    }
    HtmlUI::release();
    std::cout << "[ NATIVE   ] " << checked << " samples of present fork features checked\n";
}

// absent: phase 2's rewrite or emulation is still there
TEST(NativeFeatureConsistency, AbsentFeaturesKeepPhaseTwoRewrites){
    for (const FeatureSample& sample : kSamples){
        SCOPED_TRACE(std::string(forkFeatureName(sample.feature)) + ": " + sample.css);
        const Rewrite result = rewrite(sample, ForkFeatures::present().with(sample.feature, false));
        const std::string output = normalized(result.output);
        if (*sample.absentOutput){
            EXPECT_NE((output + " " + normalized(result.important)).find(normalized(sample.absentOutput)), std::string::npos) << describe(result);
        }
        if (*sample.absentCode){
            EXPECT_TRUE(hasCode(result.diagnostics, sample.absentCode)) << describe(result);
        }
        if (!*sample.absentOutput && !*sample.absentCode){
            EXPECT_EQ(output, normalized(sample.css)) << "phase 2 leaves it as it is: " << describe(result);
        }
    }
}
