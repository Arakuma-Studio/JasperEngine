// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <vector>
#include <string>
#include "Backend.h"
#include "nfd.hpp"
#include "diag/HangWatchdog.h"
#include "window/EditorFilePicker.h"

namespace jasperengine::editor{

    enum FileDialogType {
        FILE_DIALOG_ALL = 0,
        FILE_DIALOG_IMAGE = 1 << 0,
        FILE_DIALOG_FONT = 1 << 1,
        FILE_DIALOG_MODEL = 1 << 2,
        FILE_DIALOG_AUDIO = 1 << 3,
        FILE_DIALOG_SHADER = 1 << 4,
        FILE_DIALOG_SCRIPT = 1 << 5,
        FILE_DIALOG_PREFAB = 1 << 6,
        FILE_DIALOG_MATERIAL = 1 << 7,
        FILE_DIALOG_HTML = 1 << 8,
        FILE_DIALOG_COLOR_LUT = 1 << 9,
        FILE_DIALOG_POSE_ANIMATION = 1 << 10,
        FILE_DIALOG_ANIMATION_CLIP = 1 << 11,
        FILE_DIALOG_ANIMATOR_CONTROLLER = 1 << 12,
        FILE_DIALOG_AVATAR_MASK = 1 << 13,
        FILE_DIALOG_ANIMATION_MONTAGE = 1 << 14,
        FILE_DIALOG_VERTEX_ANIMATION = 1 << 15,
        // a Unity prefab or scene (Assets > Import Unity Particle System...)
        FILE_DIALOG_UNITY_PREFAB_SCENE = 1 << 16,
        // upstream (audio-mixer, video-mp4): 15 and 16 there, after ours here
        FILE_DIALOG_AUDIO_MIXER = 1 << 17,
        FILE_DIALOG_VIDEO = 1 << 18
    };

    class FileDialogs{

    private:
        static std::vector<nfdfilteritem_t> filterItemsFor(int filterFlags) {
            std::vector<nfdfilteritem_t> filterItems;
            if (filterFlags & FILE_DIALOG_IMAGE) {
                filterItems.push_back({ "Image files", "png,jpg,jpeg,bmp,tga,gif,hdr,psd,pic,pnm,svg" });
            }
            if (filterFlags & FILE_DIALOG_FONT) {
                filterItems.push_back({ "Font files", "ttf,otf" });
            }
            if (filterFlags & FILE_DIALOG_MODEL) {
                filterItems.push_back({ "Model files", "gltf,glb,obj,fbx" });
            }
            if (filterFlags & FILE_DIALOG_AUDIO) {
                filterItems.push_back({ "Sound files", "wav,ogg,mp3,flac" });
            }
            if (filterFlags & FILE_DIALOG_VIDEO) {
                filterItems.push_back({ "Video files", "mp4,m4v,mov,webm,mkv" });
            }
            if (filterFlags & FILE_DIALOG_SHADER) {
                filterItems.push_back({ "Shader files", "vert,frag" });
            }
            if (filterFlags & FILE_DIALOG_SCRIPT) {
                filterItems.push_back({ "Script files", "lua,cpp,cc,cxx,h,hh,hpp,hxx" });
            }
            if (filterFlags & FILE_DIALOG_PREFAB) {
                filterItems.push_back({ "Prefabs", "prefab" });
            }
            if (filterFlags & FILE_DIALOG_AUDIO_MIXER) {
                filterItems.push_back({ "Audio mixers", "mixer" });
            }
            if (filterFlags & FILE_DIALOG_MATERIAL) {
                filterItems.push_back({ "Material files", "material" });
            }
            if (filterFlags & FILE_DIALOG_HTML) {
                filterItems.push_back({ "HTML documents", "html,htm,rml" });
            }
            if (filterFlags & FILE_DIALOG_COLOR_LUT) {
                filterItems.push_back({ "Color LUTs", "cube" });
            }
            if (filterFlags & FILE_DIALOG_POSE_ANIMATION) {
                filterItems.push_back({ "Pose animations", "poseanim" });
            }
            if (filterFlags & FILE_DIALOG_ANIMATION_CLIP) {
                filterItems.push_back({ "Animation clips", "anim" });
            }
            if (filterFlags & FILE_DIALOG_ANIMATOR_CONTROLLER) {
                filterItems.push_back({ "Animator controllers", "controller" });
            }
            if (filterFlags & FILE_DIALOG_AVATAR_MASK) {
                filterItems.push_back({ "Avatar masks", "mask" });
            }
            if (filterFlags & FILE_DIALOG_ANIMATION_MONTAGE) {
                filterItems.push_back({ "Animation montages", "montage" });
            }
            if (filterFlags & FILE_DIALOG_VERTEX_ANIMATION) {
                filterItems.push_back({ "Vertex Animations", "vat" });
            }
            if (filterFlags & FILE_DIALOG_UNITY_PREFAB_SCENE) {
                filterItems.push_back({ "Unity prefabs and scenes", "prefab,unity" });
            }
            if (filterFlags & FILE_DIALOG_AVATAR_MASK) {
                filterItems.push_back({ "Avatar Masks", "mask" });
            }
            if (filterFlags & FILE_DIALOG_ANIMATOR_CONTROLLER) {
                filterItems.push_back({ "Animator Controllers", "controller" });
            }
            return filterItems;
        }

        // What MCP sees of this picker (EditorFilePicker): the extensions of the filter's "png,jpg" lists
        static EditorFilePicker::Request pickerRequest(EditorFilePicker::Mode mode, const std::string& defaultPath, const std::string& defaultName,
                                                       const std::vector<nfdfilteritem_t>& filters) {
            EditorFilePicker::Request request;
            request.mode = mode;
            request.defaultPath = defaultPath;
            request.defaultName = defaultName;
            for (const nfdfilteritem_t& filter : filters) {
                std::string extension;
                for (const char* c = filter.spec;; c++) {
                    if (*c == ',' || *c == '\0') {
                        if (!extension.empty()) request.extensions.push_back(extension);
                        extension.clear();
                        if (*c == '\0') break;
                    } else {
                        extension += *c;
                    }
                }
            }
            return request;
        }

        static std::string firstOf(const std::vector<std::string>& paths) {
            return paths.empty() ? std::string() : paths.front();
        }

    public:
        inline static std::string openFileDialog(std::string defaultPath = "", int filterFlags = FILE_DIALOG_ALL, bool selectDirectory = false) {
            std::string retPath;

            if (selectDirectory) {
                const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::OpenFolder, defaultPath, "", {});
                if (const auto answered = EditorFilePicker::ask(request)) return firstOf(*answered);
                const EditorFilePicker::NativeScope picker(request);
                // Directory selection
                nfdu8char_t* outPath;
                nfdpickfolderu8args_t args = {0};
                args.defaultPath = defaultPath.c_str();
                args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

                // a native dialog runs its own loop for as long as the user wants
                const diag::BusyScope busy("Native file dialog", 0);
                nfdresult_t result = NFD_PickFolderU8_With(&outPath, &args);

                if (result == NFD_OKAY) {
                    retPath = outPath;
                    NFD_FreePathU8(outPath);
                } else if (result == NFD_ERROR) {
                    printf("Error: %s\n", NFD_GetError());
                }
            } else {
                // File selection
                nfdu8char_t* path;
                nfdopendialogu8args_t args = {0};

                std::vector<nfdfilteritem_t> filterItems = filterItemsFor(filterFlags);

                const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::OpenFile, defaultPath, "", filterItems);
                if (const auto answered = EditorFilePicker::ask(request)) return firstOf(*answered);
                const EditorFilePicker::NativeScope picker(request);

                if (!filterItems.empty()) {
                    args.filterCount = (nfdfiltersize_t)filterItems.size();
                    args.filterList = filterItems.data();
                }

                args.defaultPath = defaultPath.c_str();
                args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

                // a native dialog runs its own loop for as long as the user wants
                const diag::BusyScope busy("Native file dialog", 0);
                const nfdresult_t res = NFD_OpenDialogU8_With(&path, &args);
                switch (res) {
                    case NFD_OKAY:
                        retPath = path;
                        NFD_FreePathU8(path);
                        break;
                    case NFD_ERROR:
                        printf("Error: %s", NFD_GetError());
                        break;
                    default:
                        break;
                }
            }

            return retPath;
        }

        // A save dialog (Create New Clip...): the chosen path, empty when cancelled. The
        // dialog asks before replacing an existing file on most platforms; callers that must
        // not replace one check it themselves.
        inline static std::string saveFileDialog(std::string defaultPath = "", std::string defaultName = "", int filterFlags = FILE_DIALOG_ALL) {
            std::string retPath;
            nfdu8char_t* path;
            nfdsavedialogu8args_t args = {0};

            std::vector<nfdfilteritem_t> filterItems = filterItemsFor(filterFlags);
            const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::SaveFile, defaultPath, defaultName, filterItems);
            if (const auto answered = EditorFilePicker::ask(request)) return firstOf(*answered);
            const EditorFilePicker::NativeScope picker(request);
            if (!filterItems.empty()) {
                args.filterCount = (nfdfiltersize_t)filterItems.size();
                args.filterList = filterItems.data();
            }
            args.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
            args.defaultName = defaultName.empty() ? nullptr : defaultName.c_str();
            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            const nfdresult_t res = NFD_SaveDialogU8_With(&path, &args);
            switch (res) {
                case NFD_OKAY:
                    retPath = path;
                    NFD_FreePathU8(path);
                    break;
                case NFD_ERROR:
                    printf("Error: %s\n", NFD_GetError());
                    break;
                default:
                    break;
            }
            return retPath;
        }

        // One file of a type of our own ("Layouts", "yaml"); "" when cancelled
        inline static std::string openFileDialogFiltered(const std::string& defaultPath, const char* filterName, const char* filterSpec) {
            const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::OpenFile, defaultPath, "", {{filterName, filterSpec}});
            if (const auto answered = EditorFilePicker::ask(request)) return firstOf(*answered);
            const EditorFilePicker::NativeScope picker(request);
            std::string retPath;
            nfdu8char_t* path;
            nfdopendialogu8args_t args = {0};
            const nfdu8filteritem_t filter = {filterName, filterSpec};
            args.filterList = &filter;
            args.filterCount = 1;
            args.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());
            const nfdresult_t res = NFD_OpenDialogU8_With(&path, &args);
            if (res == NFD_OKAY) {
                retPath = path;
                NFD_FreePathU8(path);
            } else if (res == NFD_ERROR) {
                printf("Error: %s\n", NFD_GetError());
            }
            return retPath;
        }

        // Where to write a file ("Save Layout to File..."); "" when cancelled
        inline static std::string saveFileDialog(const std::string& defaultPath, const std::string& defaultName,
                                                 const char* filterName, const char* filterSpec) {
            const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::SaveFile, defaultPath, defaultName, {{filterName, filterSpec}});
            if (const auto answered = EditorFilePicker::ask(request)) return firstOf(*answered);
            const EditorFilePicker::NativeScope picker(request);
            std::string retPath;
            nfdu8char_t* path;
            nfdsavedialogu8args_t args = {0};
            const nfdu8filteritem_t filter = {filterName, filterSpec};
            args.filterList = &filter;
            args.filterCount = 1;
            args.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
            args.defaultName = defaultName.empty() ? nullptr : defaultName.c_str();
            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());
            const nfdresult_t res = NFD_SaveDialogU8_With(&path, &args);
            if (res == NFD_OKAY) {
                retPath = path;
                NFD_FreePathU8(path);
            } else if (res == NFD_ERROR) {
                printf("Error: %s\n", NFD_GetError());
            }
            return retPath;
        }

        inline static std::vector<std::string> openFileDialogMultiple(){
            const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::OpenFiles, "", "", {});
            if (const auto answered = EditorFilePicker::ask(request)) return *answered;
            const EditorFilePicker::NativeScope picker(request);
            std::vector<std::string> filePaths;
            const nfdpathset_t* pathSet;
            nfdopendialogu8args_t args = {0};

            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            // a native dialog runs its own loop for as long as the user wants
            const diag::BusyScope busy("Native file dialog", 0);
            const nfdresult_t res = NFD_OpenDialogMultipleU8_With(&pathSet, &args);
            switch (res) {
                case NFD_OKAY:
                    nfdpathsetsize_t num_paths;
                    if (NFD_PathSet_GetCount(pathSet, &num_paths) != NFD_OKAY) {
                        printf("Error: NFD_PathSet_GetCount failed: %s\n", NFD_GetError());
                        break;
                    }
                    nfdpathsetsize_t i;
                    for (i = 0; i != num_paths; ++i) {
                        char* path;
                        if (NFD_PathSet_GetPathU8(pathSet, i, &path) != NFD_OKAY) {
                            printf("Error: NFD_PathSet_GetPathU8 failed: %s\n", NFD_GetError());
                            break;
                        }
                        filePaths.push_back(path);
                        NFD_PathSet_FreePathU8(path);
                    }
                    NFD_PathSet_Free(pathSet);

                    break;
                case NFD_ERROR:
                    printf("Error: %s", NFD_GetError());
                    break;
                default:
                    break;
            }

            return filePaths;
        }

        inline static std::vector<std::string> pickFolderDialogMultiple(){
            const EditorFilePicker::Request request = pickerRequest(EditorFilePicker::Mode::OpenFolders, "", "", {});
            if (const auto answered = EditorFilePicker::ask(request)) return *answered;
            const EditorFilePicker::NativeScope picker(request);
            std::vector<std::string> folderPaths;
            const nfdpathset_t* pathSet;
            nfdpickfolderu8args_t args = {0};

            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            // a native dialog runs its own loop for as long as the user wants
            const diag::BusyScope busy("Native file dialog", 0);
            const nfdresult_t res = NFD_PickFolderMultipleU8_With(&pathSet, &args);
            switch (res) {
                case NFD_OKAY:
                    nfdpathsetsize_t num_paths;
                    if (NFD_PathSet_GetCount(pathSet, &num_paths) != NFD_OKAY) {
                        printf("Error: NFD_PathSet_GetCount failed: %s\n", NFD_GetError());
                        break;
                    }
                    nfdpathsetsize_t i;
                    for (i = 0; i != num_paths; ++i) {
                        char* path;
                        if (NFD_PathSet_GetPathU8(pathSet, i, &path) != NFD_OKAY) {
                            printf("Error: NFD_PathSet_GetPathU8 failed: %s\n", NFD_GetError());
                            break;
                        }
                        folderPaths.push_back(path);
                        NFD_PathSet_FreePathU8(path);
                    }
                    NFD_PathSet_Free(pathSet);

                    break;
                case NFD_ERROR:
                    printf("Error: %s", NFD_GetError());
                    break;
                default:
                    break;
            }

            return folderPaths;
        }

    };

}
