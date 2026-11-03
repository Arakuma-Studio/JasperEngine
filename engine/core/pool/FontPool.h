// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FONTPOOL_H
#define FONTPOOL_H

#include "util/STBText.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    typedef std::map< std::string, std::shared_ptr<STBText> > fonts_t;

    // What a pool entry was loaded from (S05: a hot reload loads it again the same way)
    struct FontSource {
        std::string fontpath;
        std::vector<std::string> fallbackPaths;
        unsigned int fontSize = 0;
    };

    class JASPERENGINE_API FontPool{
    private:
        static fonts_t& getMap();
        static std::map<std::string, FontSource>& getSources();
        static std::map<std::string, uint32_t>& getGenerations();

    public:
        static std::shared_ptr<STBText> get(const std::string& id);
        static std::shared_ptr<STBText> get(const std::string& id, const std::string& fontpath, const std::vector<std::string>& fallbackPaths, unsigned int fontSize);
        // an atlas of a raster scale or an SDF (the id must name them: UISystem::getFontId)
        static std::shared_ptr<STBText> get(const std::string& id, const std::string& fontpath, const std::vector<std::string>& fallbackPaths, unsigned int fontSize,
                                            const STBTextOptions& options);
        static void remove(const std::string& id);
        // drops `text` (reset) and its pool entry when no one else uses that atlas
        static void release(std::shared_ptr<STBText>& text);
        // an atlas of that id is pooled
        static bool contains(const std::string& id);

        // Hot reload (S05 R13): the entry becomes `font`, its generation goes up; the texts
        // using it rebuild their atlas when flagged (needReloadAtlas). Main thread.
        static void replace(const std::string& id, std::shared_ptr<STBText> font);
        static uint32_t generation(const std::string& id);
        // The ids loaded from `systemPath` (absolute), as their font or as a fallback
        static std::vector<std::string> findIdsUsingFile(const std::string& systemPath);
        static bool getSource(const std::string& id, FontSource& out);
        // Loads `source` into a new font; any thread. nullptr when it cannot be read.
        static std::shared_ptr<STBText> decode(const FontSource& source);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();

    };
}

#endif /* FONTPOOL_H */
