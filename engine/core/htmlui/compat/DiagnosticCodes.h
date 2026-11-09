// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_DIAGNOSTICCODES_H
#define COMPAT_DIAGNOSTICCODES_H

#include "Export.h"
#include "CompatTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    enum class Severity : uint8_t { Error, Warning, Info };

    // Areas of the stable diagnostic codes HTMLUI-<AREA>-NNNN (phase-2 §3.4, Appendix F).
    // Append only: phase 5 added FX after SVG (the UI effects).
    enum class DiagArea : uint8_t { GEN, HTML, CSS, SEL, AT, IMP, SVG, FX };

    struct DiagCode {
        DiagArea area = DiagArea::GEN;
        uint16_t number = 0;      // never reused; Appendix F

        JASPERENGINE_API std::string id() const;   // "HTMLUI-CSS-0004"

        constexpr bool operator==(const DiagCode& other) const { return area == other.area && number == other.number; }
        constexpr bool operator!=(const DiagCode& other) const { return !(*this == other); }
        constexpr bool operator<(const DiagCode& other) const {
            return area != other.area ? area < other.area : number < other.number;
        }
    };

    struct DiagCodeInfo {
        DiagCode code;
        Severity severity;        // default severity
        bool severityByCase;      // "by case" in Appendix F: the reporter picks the severity
        const char* name;         // kebab-case, e.g. "unsupported-property"
        const char* title;        // short title for docs
        const char* help;         // paragraph for the generated docs: what happened, how to fix
    };

    // The registry, sorted by area then number. A code is never renumbered or reused; a new
    // situation takes the next free number of its area (a row here and a constant below).
    JASPERENGINE_API Span<const DiagCodeInfo> diagnosticCodes();
    // by id ("HTMLUI-CSS-0004", any case) or by name ("unsupported-property"); nullptr if none
    JASPERENGINE_API const DiagCodeInfo* findDiagnosticCode(std::string_view idOrName);
    JASPERENGINE_API const DiagCodeInfo* findDiagnosticCode(DiagCode code);

    JASPERENGINE_API const char* diagAreaName(DiagArea area);     // "CSS"
    JASPERENGINE_API const char* severityName(Severity severity); // "error", "warning", "info"

    // Named constants, one per registry row: the name in CamelCase. Report through these
    // (sink.report(codes::UnknownProperty, ...)); the codes test scans the sources and
    // fails on a constant or a literal "HTMLUI-…" id the registry does not have.
    namespace codes {
        // GEN
        inline constexpr DiagCode FileNotFound{DiagArea::GEN, 1};
        inline constexpr DiagCode DiagnosticsTruncated{DiagArea::GEN, 2};
        inline constexpr DiagCode InvalidUtf8{DiagArea::GEN, 3};
        inline constexpr DiagCode InternalError{DiagArea::GEN, 4};
        inline constexpr DiagCode NonUtf8Charset{DiagArea::GEN, 5};
        inline constexpr DiagCode RmluiMessage{DiagArea::GEN, 6};
        inline constexpr DiagCode GlyphAtlasBudget{DiagArea::GEN, 7};
        // HTML
        inline constexpr DiagCode HtmlParseError{DiagArea::HTML, 1};
        inline constexpr DiagCode UnsupportedElement{DiagArea::HTML, 2};
        inline constexpr DiagCode ElementMapped{DiagArea::HTML, 3};
        inline constexpr DiagCode UnsupportedAttribute{DiagArea::HTML, 4};
        inline constexpr DiagCode EventHandlerAttribute{DiagArea::HTML, 5};
        inline constexpr DiagCode Javascript{DiagArea::HTML, 6};
        inline constexpr DiagCode KnownScript{DiagArea::HTML, 7};
        inline constexpr DiagCode InputTypeApproximated{DiagArea::HTML, 8};
        inline constexpr DiagCode FormNavigation{DiagArea::HTML, 9};
        inline constexpr DiagCode LinkSkipped{DiagArea::HTML, 10};
        inline constexpr DiagCode DuplicateId{DiagArea::HTML, 11};
        inline constexpr DiagCode NoscriptRendered{DiagArea::HTML, 12};
        inline constexpr DiagCode ReservedName{DiagArea::HTML, 13};
        inline constexpr DiagCode BindingAttributeOutsideModel{DiagArea::HTML, 14};
        inline constexpr DiagCode TitleTooltip{DiagArea::HTML, 15};
        inline constexpr DiagCode BaseIgnored{DiagArea::HTML, 16};
        inline constexpr DiagCode ImageCandidateChosen{DiagArea::HTML, 17};
        inline constexpr DiagCode UnknownCustomElement{DiagArea::HTML, 18};
        inline constexpr DiagCode TabindexOrder{DiagArea::HTML, 19};
        inline constexpr DiagCode FieldsetDisabled{DiagArea::HTML, 20};
        inline constexpr DiagCode UnknownLucideIcon{DiagArea::HTML, 21};
        inline constexpr DiagCode LuaScriptError{DiagArea::HTML, 22};
        inline constexpr DiagCode LinkNotNavigable{DiagArea::HTML, 23};
        inline constexpr DiagCode UnsupportedImageFormat{DiagArea::HTML, 24};
        inline constexpr DiagCode DialogNotModal{DiagArea::HTML, 25};
        inline constexpr DiagCode SelfClosingTag{DiagArea::HTML, 26};   // p2-html
        inline constexpr DiagCode UnsupportedMediaFormat{DiagArea::HTML, 27};   // phase 6, p6-video
        inline constexpr DiagCode MediaAttributeIgnored{DiagArea::HTML, 28};    // phase 6, p6-video
        inline constexpr DiagCode VirtualViewHeight{DiagArea::HTML, 29};        // S17: runtime
        inline constexpr DiagCode VirtualViewTemplate{DiagArea::HTML, 30};      // S17
        inline constexpr DiagCode LargeDataFor{DiagArea::HTML, 31};             // S17: runtime
        inline constexpr DiagCode MarkdownRawHtml{DiagArea::HTML, 32};          // S17
        inline constexpr DiagCode ParticleSystemSource{DiagArea::HTML, 33};     // ui-scaling-text (29 upstream; oe-port's S17 took 29-32)
        // CSS
        inline constexpr DiagCode UnknownProperty{DiagArea::CSS, 1};
        inline constexpr DiagCode UnsupportedProperty{DiagArea::CSS, 2};
        inline constexpr DiagCode IgnoredProperty{DiagArea::CSS, 3};
        inline constexpr DiagCode UnsupportedValue{DiagArea::CSS, 4};
        inline constexpr DiagCode ApproximatedMinor{DiagArea::CSS, 5};
        inline constexpr DiagCode ApproximatedVisible{DiagArea::CSS, 6};
        inline constexpr DiagCode ForkFallback{DiagArea::CSS, 7};
        inline constexpr DiagCode CssSyntaxError{DiagArea::CSS, 8};
        inline constexpr DiagCode VendorPrefix{DiagArea::CSS, 9};
        inline constexpr DiagCode ImportantMayLose{DiagArea::CSS, 10};
        inline constexpr DiagCode InheritApproximated{DiagArea::CSS, 11};
        inline constexpr DiagCode VarUntyped{DiagArea::CSS, 12};
        inline constexpr DiagCode ViewportDependent{DiagArea::CSS, 13};
        inline constexpr DiagCode UnsupportedUnit{DiagArea::CSS, 14};
        inline constexpr DiagCode UnsupportedFunction{DiagArea::CSS, 15};
        inline constexpr DiagCode ColorUnresolvable{DiagArea::CSS, 16};
        inline constexpr DiagCode MergedFromBaseRule{DiagArea::CSS, 17};
        inline constexpr DiagCode LonghandWithoutBase{DiagArea::CSS, 18};
        inline constexpr DiagCode EasingApproximated{DiagArea::CSS, 19};
        inline constexpr DiagCode StepsApproximated{DiagArea::CSS, 20};
        inline constexpr DiagCode AnimationApproximated{DiagArea::CSS, 21};
        inline constexpr DiagCode FontSource{DiagArea::CSS, 22};
        inline constexpr DiagCode UnknownFontFamily{DiagArea::CSS, 23};
        inline constexpr DiagCode BitmapFontEffect{DiagArea::CSS, 24};   // S51 R15
        // SEL
        inline constexpr DiagCode UnsupportedSelector{DiagArea::SEL, 1};
        inline constexpr DiagCode SelectorRewritten{DiagArea::SEL, 2};
        inline constexpr DiagCode PseudoElementPending{DiagArea::SEL, 3};
        inline constexpr DiagCode IsWhereExpanded{DiagArea::SEL, 4};
        inline constexpr DiagCode SelectorExpansionLimit{DiagArea::SEL, 5};
        inline constexpr DiagCode VisitedDropped{DiagArea::SEL, 6};
        inline constexpr DiagCode InvalidSelector{DiagArea::SEL, 7};
        inline constexpr DiagCode SelectorIgnored{DiagArea::SEL, 8};   // added by p2-css
        // AT
        inline constexpr DiagCode UnsupportedAtRule{DiagArea::AT, 1};
        inline constexpr DiagCode LayerFlattened{DiagArea::AT, 2};
        inline constexpr DiagCode SupportsEvaluated{DiagArea::AT, 3};
        inline constexpr DiagCode MediaFeatureUnsupported{DiagArea::AT, 4};
        inline constexpr DiagCode MediaNormalized{DiagArea::AT, 5};
        inline constexpr DiagCode MediaStatic{DiagArea::AT, 6};
        inline constexpr DiagCode ImportProblem{DiagArea::AT, 7};
        inline constexpr DiagCode NestingFlattened{DiagArea::AT, 8};
        inline constexpr DiagCode KeyframesInvalid{DiagArea::AT, 9};
        inline constexpr DiagCode PropertyApproximated{DiagArea::AT, 10};
        inline constexpr DiagCode FontFaceDescriptor{DiagArea::AT, 11};
        inline constexpr DiagCode AtPropertyUnsupportedSyntax{DiagArea::AT, 12};   // phase 5, p5-property
        inline constexpr DiagCode AtPropertyConflict{DiagArea::AT, 13};            // phase 5, p5-property
        inline constexpr DiagCode AtPropertyInvalid{DiagArea::AT, 14};             // phase 5, p5-property
        // IMP
        inline constexpr DiagCode NotImported{DiagArea::IMP, 1};
        inline constexpr DiagCode DownloadFailed{DiagArea::IMP, 2};
        inline constexpr DiagCode UnsupportedContent{DiagArea::IMP, 3};
        inline constexpr DiagCode Imported{DiagArea::IMP, 4};
        inline constexpr DiagCode CurlMissing{DiagArea::IMP, 5};
        inline constexpr DiagCode FontVariantUnavailable{DiagArea::IMP, 6};
        inline constexpr DiagCode WoffOnly{DiagArea::IMP, 7};
        inline constexpr DiagCode LicenseNote{DiagArea::IMP, 8};
        inline constexpr DiagCode ManifestFileMissing{DiagArea::IMP, 9};
        inline constexpr DiagCode VideoNotConverted{DiagArea::IMP, 10};        // phase 6, p6-video
        inline constexpr DiagCode VideoConversionFailed{DiagArea::IMP, 11};    // phase 6, p6-video
        // SVG
        inline constexpr DiagCode SvgRecolor{DiagArea::SVG, 1};
        inline constexpr DiagCode SvgUnsupportedContent{DiagArea::SVG, 2};
        inline constexpr DiagCode SvgInternalCss{DiagArea::SVG, 3};
        // FX (phase 5, the UI effects: phase-5-effects.md 3.13)
        inline constexpr DiagCode EffectUnknown{DiagArea::FX, 1};
        inline constexpr DiagCode EffectWrongContext{DiagArea::FX, 2};
        inline constexpr DiagCode EffectPositionalArg{DiagArea::FX, 3};
        inline constexpr DiagCode EffectUnknownParam{DiagArea::FX, 4};
        inline constexpr DiagCode EffectBadValue{DiagArea::FX, 5};
        inline constexpr DiagCode EffectOutOfRange{DiagArea::FX, 6};
        inline constexpr DiagCode EffectDuplicateParam{DiagArea::FX, 7};
        inline constexpr DiagCode EffectBlendOnFilter{DiagArea::FX, 8};
        inline constexpr DiagCode EffectIgnoredBackgroundProp{DiagArea::FX, 9};
        inline constexpr DiagCode EffectUnsupportedBlend{DiagArea::FX, 10};
        inline constexpr DiagCode EffectTextFill{DiagArea::FX, 11};
        inline constexpr DiagCode EffectDeclareConflict{DiagArea::FX, 12};
        inline constexpr DiagCode EffectFileMissing{DiagArea::FX, 13};
        inline constexpr DiagCode EffectManifest{DiagArea::FX, 14};
        inline constexpr DiagCode EffectShaderCompile{DiagArea::FX, 15};
        inline constexpr DiagCode EffectCost{DiagArea::FX, 16};
        // phase 6 (stateful effects)
        inline constexpr DiagCode EffectStateUnsupported{DiagArea::FX, 17};
        inline constexpr DiagCode EffectStatefulBudget{DiagArea::FX, 18};
    }

    // Hints shared by several reporters (phase-3 spec 2.2's replacement hints).
    namespace hints {
        // direction: rtl and <html dir="rtl">: the text runs right to left, layouts are not mirrored
        inline constexpr const char* kMirroredLayout =
            "Mirrored layout is not supported; mirror the layout explicitly (e.g. flex-direction: row-reverse).";
    }

}

#endif //COMPAT_DIAGNOSTICCODES_H
