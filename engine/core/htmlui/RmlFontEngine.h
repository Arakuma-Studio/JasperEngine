// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTENGINE_H
#define RMLFONTENGINE_H

#include "Export.h"
#include "RmlFontEffects.h"
#include "util/GlyphAtlas.h"
#include "util/TextShaper.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine::pixelart {
    class BitmapFont;
}

namespace jasperengine {

    class FontFace;

    // RmlUi's font engine on the engine text stack (FontFace, FontRegistry, TextShaper,
    // GlyphAtlas): HarfBuzz shaping, stb_truetype glyphs in atlas pages.
    //
    // - font-family lists are fallback chains: RmlUi 6.3 passes "A, B" as one string, split
    //   here; generic families resolve to the built-in Roboto (through FontRegistry, so a
    //   project's Roboto Bold serves sans-serif bold); every chain ends with the registered
    //   fallback faces, Roboto and Noto Sans Arabic.
    // - Sizes are CSS font-size (em size), not STBText's ascent-to-descent pixel height.
    // - Glyphs are rasterized at size * raster scale (physical pixels per document pixel) into
    //   1024x1024 R8 coverage pages (S16 R12), each with a 4x4 white block that untextured
    //   geometry batched with the page samples. A glyph never moves while its page lives, so
    //   geometry generated earlier in a frame stays valid while new glyphs are added: the
    //   renderer uploads the page's texture again before the frame draws. GetVersion is per
    //   handle (and an epoch for reloads and scale resets): what changes the text a handle drew
    //   (its page cleared) regenerates exactly that text.
    // - The pages live within a memory budget (R13, setAtlasBudget): when a glyph fits no page
    //   and no new page fits the budget, the least recently used page that was not drawn in this
    //   frame or the last one is cleared, its glyphs dropped and its users' versions bumped.
    // - The raster scale is per render manager (one per HtmlUIContext: the screen context and
    //   each texture-mode context have their own pixel ratio); glyphs are keyed by their
    //   raster size, so contexts share faces and pages and keep their own glyphs. With
    //   transform-aware text (R14, fork S16-F3) an element passes its own on-screen scale
    //   (TextShapingContext::raster_scale), keyed by quarter device pixels of the em.
    // - Distance-field glyphs (R16, RmlFontMsdf): MTSDF glyphs made once per face, glyph and
    //   synthetic bold on the job system's background lane, in RGBA8 pages of their own; text
    //   draws from them while its scale animates, above 128 device px, under perspective and
    //   with text-rendering: geometricPrecision (ElementText decides).
    // - font-effect (RmlFontEffects): outline, glow and blur within the distance range are drawn
    //   from the distance glyphs with a text effect (R17: a variant of the page's texture);
    //   beyond it, or until the distance glyphs exist, they get CPU glyphs in RGBA8 pages of
    //   their own, made at document resolution; shadow redraws the base glyphs offset.
    //   Back-layer effects draw first, then the text, then front-layer effects.
    // - Synthetic bold (the face found is >= 300 lighter than asked) and italic (italic asked,
    //   upright face found), as CSS font-synthesis: a dilated bitmap with a longer advance,
    //   and quads sheared by 12 degrees.
    class JASPERENGINE_INTERNAL RmlFontEngine : public Rml::FontEngineInterface {
    public:
        static constexpr int pageSize = 1024;
        static constexpr int whiteBlockSize = 4;
        static constexpr size_t maxWidthCacheEntries = 8192;
        // R15: bitmap glyphs one frame may rasterize for text settling at a new scale (the
        // first generation of a text is never held back), and the time it may take
        static constexpr int settleGlyphBudget = 256;
        static constexpr double settleTimeBudgetMs = 2.0;
        // R16: distance glyphs over 128 device px per em draw from the distance atlas
        static constexpr float distanceFieldMinPixels = 128.0f;

        RmlFontEngine();
        ~RmlFontEngine() override;

        void Initialize() override;     // registers built-ins in FontRegistry
        void Shutdown() override;       // ReleaseFontResources(), prepared effect sets
        bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override;
        bool LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;
        bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;  // copies the data
        Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                              Rml::Style::FontWeight weight, int size) override;
        // 0 for an empty list; else a set cached by the effects' fingerprints (any face)
        Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override;
        const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
        int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                           Rml::Character prior_character = Rml::Character::Null) override;
        int GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                           Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                           const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list) override;
        // the bidi runs shapeRange lays out (SheenBidi, the paragraph's direction as shapeRange takes it), in visual order, as
        // byte ranges of the string: the line-clamp ellipsis cuts them (fork P6-L1)
        void GetBidiRuns(Rml::StringView string, const Rml::TextShapingContext& context, Rml::Vector<Rml::TextBidiRun>& runs) override;
        // per handle: changes when a page it drew from changed or went, and with the epoch
        int GetVersion(Rml::FontFaceHandle handle) override;
        void ReleaseFontResources() override;                  // drops handles, glyph cache, pages; faces stay registered
        // fork S16-F3 (transform-aware text, distance glyphs, GPU text shadows)
        float GetBaseRasterScale(Rml::RenderManager& render_manager) override;
        bool PrepareGlyphs(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context) override;
        bool PrepareDistanceGlyphs(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context) override;
        bool SupportsGpuTextShadow(Rml::FontFaceHandle handle, float blur_radius) override;
        void ReportTextEffectFallback(Rml::RenderManager& render_manager) override;
        void ReportTextRegeneration(Rml::RenderManager& render_manager) override;

        // hot reload (R18): the font file changed on disk. Its faces are parsed again (FontRegistry::reloadFile);
        // their bitmap and distance glyphs go and only the handles that use them are retired (and bumped).
        // False when no loaded face uses it.
        bool reloadFontFile(const std::string& path);

        // physical px per document px of the text one render manager (context) draws; a change
        // bumps the epoch (text is regenerated), pages and glyphs stay
        void setRasterScale(Rml::RenderManager& renderManager, float scale);
        float getRasterScale(const Rml::RenderManager& renderManager) const;   // its own, else the default
        // the default for render managers without their own scale (phase 0's global scale);
        // change -> clear glyphs/pages, epoch++
        void setRasterScale(float scale);
        float getRasterScale() const;
        size_t getPageCount() const;        // tests: base glyph pages
        size_t getEffectPageCount() const;  // tests: RGBA8 pages of effect glyphs
        size_t getDistancePageCount() const;   // tests: RGBA8 pages of distance glyphs
        size_t getPixelPageCount() const;   // tests: pages of bitmap faces' glyphs (both kinds)
        size_t getBitmapGlyphCount(uint64_t serial) const;   // tests: glyphs held for that bitmap font
        size_t getEffectSetCount() const;   // tests: prepared effect sets

        // R13: the atlas budget in MiB of base pages (R8 coverage, 1 MiB each, and the RGBA8
        // pages of CPU font effects, 4 MiB each) and of distance pages (RGBA8, 4 MiB each); 0 is
        // the platform's default (8 + 4 on Android, iOS and HTML5, 16 + 8 elsewhere: the
        // project's htmlUiGlyphAtlasMiB is their sum). A smaller budget applies as pages go
        static void setAtlasBudget(size_t baseMiB, size_t distanceMiB);
        static void setAtlasBudgetMiB(size_t totalMiB);   // split 2:1, as the defaults
        static size_t getBaseBudgetBytes();
        static size_t getDistanceBudgetBytes();
        // GPU bytes of every page, one copy each (per render manager that draws it)
        size_t getAtlasBytes() const;
        uint64_t getEvictionCount() const;  // pages cleared to make room, over the engine's life
        // distance glyphs asked for and not made yet; runs what finished (tests, the frame's statistics)
        size_t getPendingDistanceGlyphCount() const;
        // waits for the distance glyphs in flight and places them (tests)
        void finishDistanceGlyphsForTesting();
        // tests: distance glyphs made at once when asked for (the frame waits), so that what a frame
        // shows does not depend on the background lane's timing (goldens); the engine makes them in
        // the background
        static void setSynchronousDistanceGlyphsForTesting(bool synchronous);

        // R14/R15 (D1): false keeps text at its render manager's scale whatever its transform,
        // with the scale quantized as before S16; the project's htmlUiTransformAwareText
        static void setTransformAwareTextForTesting(bool enabled);
        static bool isTransformAwareText();
        // R17: false draws every font effect and blurred text-shadow the CPU or layer way (tests)
        static void setGpuTextEffectsForTesting(bool enabled);
        static bool isGpuTextEffects();

        // the counters of one context's frame (bracketing its Rml::Context::Render): glyphs
        // rasterized, text regenerated, effects drawn the slow way, pages evicted
        struct FrameCounters {
            uint32_t glyphsRasterized = 0;
            uint32_t textRegenerations = 0;
            uint32_t textEffectFallbacks = 0;
            uint32_t atlasEvictions = 0;
            double glyphRasterMs = 0.0;      // the time those glyphs took (R22's budget)
        };
        void beginContextFrame(const Rml::RenderManager& renderManager);
        FrameCounters takeContextFrame(const Rml::RenderManager& renderManager);

        // phase-2 hook H6: whether a family is known (a generic family, a built-in or a loaded
        // face), for HTMLUI-CSS-0023 (FontRegistry::hasFamily)
        static bool hasFamily(const std::string& name);
        // A family that is not loaded and that something else reports to the user (the compat
        // layer names it at the document that asks for it, HTMLUI-CSS-0023): the engine's own
        // "font-family '<name>' is not loaded" warning is not logged for it.
        void markFamilyReported(const std::string& family);
        // tests (phase-2 golden pages, "no-text" variant): false keeps text layout and metrics but
        // generates no glyph quads (base text and font effects); a change bumps the epoch
        void setRenderTextForTesting(bool render);
        bool getRenderTextForTesting() const;

        // tests: the faces of a handle's chain, primary first; the "family, family" list split
        // and normalized as GetFontFaceHandle does
        std::vector<const FontFace*> getChain(Rml::FontFaceHandle handle) const;
        static std::vector<std::string> splitFamilyList(const std::string& families);

    private:
        struct FaceHandle {
            std::vector<std::shared_ptr<FontFace>> chain;
            std::vector<const FontFace*> faces;
            std::vector<float> emScales;   // document px per font unit of each face
            std::vector<float> boldStrength; // per face: synthetic bold in document px, 0 = the face has the weight
            std::vector<float> italicShear;  // per face: synthetic italic shear, 0 = none
            int size = 0;
            Rml::FontMetrics metrics = {};
            int version = 1;
            int epoch = 0;                 // the engine epoch version last saw
            // a bitmap face (S51 R15): its font and pixel scale k; then chain and faces are empty
            std::shared_ptr<pixelart::BitmapFont> bitmap;
            int pixelScale = 0;
        };

        struct GlyphKey {
            const FontFace* face;
            uint32_t glyphIndex;
            int rasterPixelSize;   // round(size * rasterScale * 64), transform-aware text round(size * scale * 4); effect glyphs: raster scale 1
            int bold;              // synthetic bold dilation in raster px, 0 = none
            size_t effect;         // RmlFontEffects::effectId, 0 = the base glyph
            uint64_t bitmap = 0;   // a bitmap face's font (BitmapFont::getSerial, face null), 0 = a FontFace's glyph

            bool operator==(const GlyphKey& other) const{
                return face == other.face && glyphIndex == other.glyphIndex && rasterPixelSize == other.rasterPixelSize &&
                       bold == other.bold && effect == other.effect && bitmap == other.bitmap;
            }
        };

        struct GlyphKeyHash {
            size_t operator()(const GlyphKey& key) const;
        };

        // PIXEL, PIXEL_EFFECT: a bitmap face's glyphs (S51 R15), sampled nearest so a transform
        // magnifies their pixels as blocks
        enum class PageKind : uint8_t { BASE, EFFECT, DISTANCE, PIXEL, PIXEL_EFFECT };
        static bool isCoverageKind(PageKind kind){ return kind == PageKind::BASE || kind == PageKind::PIXEL; }

        struct GlyphEntry {
            int page = -1;          // -1: nothing to draw (no ink, or it fits no page)
            PageKind kind = PageKind::BASE;
            int x = 0, y = 0;       // in the page, raster px
            int width = 0, height = 0;
            int xoff = 0, yoff = 0; // top-left relative to pen/baseline, raster px, y down
        };

        // a distance glyph: pending until its job is done, then placed (page -1: no outline,
        // or it fits no page)
        struct DistanceEntry {
            bool ready = false;
            int page = -1;
            int x = 0, y = 0, width = 0, height = 0;
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;   // font units, y up
        };
        struct DistanceKey {
            const FontFace* face;
            uint32_t glyphIndex;
            int bold;               // synthetic bold in 1/1024 em
            bool operator==(const DistanceKey& other) const{
                return face == other.face && glyphIndex == other.glyphIndex && bold == other.bold;
            }
        };
        struct DistanceKeyHash {
            size_t operator()(const DistanceKey& key) const;
        };
        struct DistanceJob;

        // R8 coverage (base glyphs), premultiplied RGBA8 (CPU effect glyphs) or MTSDF (distance glyphs)
        struct Page {
            Page(RmlFontEngine& engine, PageKind kind);
            RmlFontEngine& engine;
            PageKind kind;
            // the texels, shared with the renderer's textures of the page: glyphs added bump the
            // generation and the textures are uploaded again, in place (no new source, no regeneration)
            std::shared_ptr<GlyphAtlas> atlas;
            std::shared_ptr<uint64_t> generation = std::make_shared<uint64_t>(0);
            // a render interface other than the engine's made a texture of it: new glyphs need a new
            // source (RmlUi uploads it again) and the text that drew from it is made again
            bool foreign = false;
            std::unique_ptr<Rml::CallbackTextureSource> source;
            // the textures its source made, by render manager (the base of its effect variants)
            std::unordered_map<const Rml::RenderManager*, Rml::TextureHandle> textures;
            // effect variants of a distance page (R17), by effect (effect, dilation, sigma)
            std::map<std::array<float, 3>, std::unique_ptr<Rml::CallbackTextureSource>> variants;
            bool dirty = false;
            int whiteX = -1, whiteY = -1;
            uint64_t lastUse = 0;           // the frame a string last drew from it
            uint64_t opened = 0;            // when it was (re)opened: the newest page is the one glyphs fill
            std::shared_ptr<uint64_t> drawFrame = std::make_shared<uint64_t>(0);   // the renderer's last draw
            std::set<FaceHandle*> users;    // handles whose text drew from it since it was cleared
            size_t bytes() const;
            uint64_t lastUsed() const;
        };
        using PageList = std::vector<std::unique_ptr<Page>>;

        static ShapeOptions toShapeOptions(const Rml::TextShapingContext& context);
        // UTF-8 to code points without the control characters (U+0000..U+001F)
        static void decodeText(Rml::StringView string, std::vector<uint32_t>& codepoints);
        // the same, and the text of a justified line (p6-justify): the space RmlUi adds around the
        // characters, by their byte offsets, per code point in options
        static void decodeText(Rml::StringView string, const Rml::TextShapingContext& context, std::vector<uint32_t>& codepoints,
                               ShapeOptions& options);
        FaceHandle* toHandle(Rml::FontFaceHandle handle) const;
        bool registerLoaded(const std::shared_ptr<FontFace>& face, const std::string& error, const std::string& family,
                            int weight, int italic, bool fallback);
        // RmlUi keeps handles it already has: new lookups see the new face, old handles stay valid
        void retireHandles();
        float measure(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options) const;
        // shaped glyphs with the synthetic bold advance added
        void shape(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options,
                   std::vector<ShapedGlyph>& shaped) const;
        float scaleFor(const Rml::RenderManager& renderManager) const;
        // the scale a string is rasterized at, and the key of its glyphs' raster size
        float rasterScaleOf(const Rml::RenderManager* renderManager, const FaceHandle& handle, const Rml::TextShapingContext& context,
                            int& rasterKey) const;
        const GlyphEntry* findGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale, int rasterKey) const;
        const GlyphEntry& getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale, int rasterKey);
        const GlyphEntry& getEffectGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex,
                                         const Rml::FontEffect& effect, size_t effectId);
        DistanceKey distanceKey(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex) const;
        // the glyph's distance entry; asks for it (a background job) when unknown
        const DistanceEntry& requestDistanceGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex);
        // places the distance glyphs whose jobs finished (wait: every job in flight first)
        void collectDistanceGlyphs(bool wait);
        // whether the effect is drawn from distance glyphs (within their reach at this size), and how
        bool distanceEffectOf(const FaceHandle& handle, const Rml::FontEffect& effect, std::array<float, 3>& parameters) const;
        Rml::Texture variantTexture(Page& page, const std::array<float, 3>& parameters, Rml::RenderManager& renderManager);
        // packs a bitmap into a page of the kind with room (a new page within the budget, else
        // the least recently used page cleared); false (logged once) when it fits no page
        bool place(PageKind kind, const unsigned char* pixels, int width, int height, int& page, int& x, int& y,
                   const std::string& label, int size);
        PageList& pagesOf(PageKind kind);
        size_t budgetBytes(PageKind kind) const;
        size_t usedBytes(PageKind kind) const;
        bool evictPage(PageKind kind);
        void clearPage(Page& page, bool bump);
        void reserveWhiteBlock(Page& page);
        void markUsed(Page& page, FaceHandle* user);
        void bump(FaceHandle* handle);
        void bumpEpoch();
        void refreshDirtyPages();
        void makeSource(Page& page);
        void clearGlyphs();
        FrameCounters* countersOf(const Rml::RenderManager* renderManager);
        void noteEviction();

        std::map<std::string, std::unique_ptr<FaceHandle>> handles;
        std::vector<std::unique_ptr<FaceHandle>> retiredHandles;
        std::set<std::string> warnedFamilies;

        std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> glyphs;
        std::unordered_map<DistanceKey, DistanceEntry, DistanceKeyHash> distanceGlyphs;
        std::vector<std::unique_ptr<DistanceJob>> distanceJobs;
        PageList pages;         // base glyphs
        PageList effectPages;   // glyphs of effects with their own texture
        PageList distancePages; // MTSDF glyphs
        PageList pixelPages;    // bitmap faces' monochrome glyphs (nearest)
        PageList pixelEffectPages;  // bitmap faces' coloured glyphs, outlines and masks (nearest)

        // prepared font-effect lists by their effect ids; kept until Shutdown, as RmlUi keeps
        // the handles it was given
        std::map<std::vector<size_t>, std::unique_ptr<RmlFontEffects::Set>> effectSets;
        std::unordered_map<const Rml::RenderManager*, float> managerScales;
        std::unordered_map<const Rml::RenderManager*, FrameCounters> managerCounters;
        std::set<const Rml::RenderManager*> warnedBudget;   // HTMLUI-GEN-0007, once per context
        std::vector<uint64_t> evictionFrames;               // of the last evictions (the diagnostic's window)
        uint64_t evictions = 0;
        const Rml::RenderManager* currentManager = nullptr; // the context whose frame is counted

        // the settle budget of the current frame (R15)
        uint64_t settleFrame = UINT64_MAX;
        int settleGlyphs = 0;
        double settleMs = 0.0;

        std::unordered_map<std::string, int> widths;
        int widthsEpoch = -1;

        int epoch = 1;
        float rasterScale = 1.0f;
        bool renderText = true;
        bool warnedOverflow = false;
        // a glyph that found no room because every page of the budget was drawn in this frame or
        // the last: not kept, its handle bumped at the next frame (its text made again, when the
        // least recently used page can go), and that frame asked for
        bool placeDeferred = false;                         // set by place
        uint64_t pagesOpened = 0;                           // Page::opened's counter
        std::unordered_set<FaceHandle*> deferredHandles;
        uint64_t deferredFrame = 0;
        int deferredStreak = 0;                             // consecutive frames that deferred glyphs (at most 3)
        void deferGlyph(const FaceHandle& handle);
        void retryDeferredGlyphs();

        Rml::FontMetrics emptyMetrics = {};

        // bitmap faces (S51 R15, RmlFontEngineBitmap.cpp): registered by family, drawn from the
        // same pages at d = max(1, round(k * raster scale)) device pixels per font pixel, never as
        // distance glyphs or re-rasterised under a transform
        bool loadBitmapFace(const std::string& file, const std::string& family);
        std::unique_ptr<FaceHandle> makeBitmapHandle(const std::shared_ptr<pixelart::BitmapFont>& font, int size);
        int bitmapStringWidth(const FaceHandle& handle, Rml::StringView string, const Rml::TextShapingContext& context) const;
        int generateBitmapString(Rml::RenderManager& renderManager, FaceHandle& handle, const RmlFontEffects::Set* effects, Rml::StringView string,
                                 Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity, const Rml::TextShapingContext& context,
                                 Rml::TexturedMeshList& meshList);
        // a bitmap glyph at d device px per font px: style 0 the glyph, 1 its mask (white), 16 * w + n a
        // pixel outline of w font px through n neighbours
        const GlyphEntry& bitmapGlyph(const FaceHandle& handle, uint32_t codepoint, int d, int style);
        bool reloadBitmapFontFile(const std::string& path);
        // drops the glyphs of a bitmap font that is no longer drawn (a re-registered family, a reload)
        void purgeBitmapGlyphs(uint64_t serial);
    public:
        // a family @font-face registered with a .bitmapfont or .fnt source, and its font
        static bool isBitmapFamily(const std::string& family);
        static std::shared_ptr<pixelart::BitmapFont> bitmapFamilyFont(const std::string& family);
        // a file a registered bitmap face read (its sheet, pages): a change reloads the face
        static bool isBitmapDependency(const std::string& path);
    };

}

#endif //RMLFONTENGINE_H
