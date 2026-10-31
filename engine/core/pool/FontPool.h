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

    class JASPERENGINE_API FontPool{
    private:
        static fonts_t& getMap();

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

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();

    };
}

#endif /* FONTPOOL_H */
