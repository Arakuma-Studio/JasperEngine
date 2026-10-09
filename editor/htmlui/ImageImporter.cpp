// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImageImporter.h"

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using namespace jasperengine::htmlui::compat;

ImageImporter::ImageImporter(RemoteImporter& importer): importer(importer) {}

std::string ImageImporter::localPath(const RemoteImporter& importer, const std::string& url, const SniffedContent& sniffed){
    std::string host, path, query;
    splitUrl(url, host, path, query);
    std::string name = safeFileName(urlBaseName(url));
    if (name.empty())
        name = "image";
    return importer.importPath("images/" + safeFileName(host) + "/" + urlHash(url, 12) + "-" + name + "." + sniffed.extension());
}

ImportedResource ImageImporter::run(const std::string& url, const std::string& finalUrl, const std::string& body,
                                    const SniffedContent& sniffed, const ImportLocation& at){
    (void)finalUrl;
    ImportedResource result;
    result.url = url;

    const std::string local = localPath(importer, url, sniffed);
    ImportStaging staging;
    staging.add(local, body);

    std::string host, path, query;
    splitUrl(url, host, path, query);
    ImportEntry entry;
    entry.url = url;
    entry.kind = sniffed.type == SniffedContent::Type::Svg ? "svg" : "image";
    entry.path = local;
    entry.files = {local};
    entry.licenseName = "unknown";
    entry.source = host;
    if (!importer.finish(staging, {entry}, result))
        return result;

    if (!sniffed.isDecodableImage()){
        importer.report(codes::UnsupportedContent, at, url, "'" + url + "' is " + sniffed.name() +
                        ", which the engine cannot decode; saved as " + local + ", but it will not be drawn",
                        "use a PNG, JPEG or SVG version of the image (convert it and point the document at the file)");
    }
    importer.report(codes::LicenseNote, at, url, "the license of the image '" + url + "' is unknown: verify that the game may ship it");
    return result;
}
