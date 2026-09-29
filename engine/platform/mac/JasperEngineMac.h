// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineMac_h
#define JasperEngineMac_h

// Entry point of the macOS backend for exported games, on every graphic backend:
// the sokol define picks the drawable, a MacViewMetal, MacViewVulkan or MacViewGL.
// The window is WindowMac and the jasperengine::System surface is SystemMac, both shared
// with the editor, so what is left here is the frame loop and the build settings.
class JasperEngineMac {
public:

    static int init(int argc, char **argv);
};

#endif /* JasperEngineMac_h */
