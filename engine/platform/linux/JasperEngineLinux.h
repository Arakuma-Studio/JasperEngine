// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineLinux_h
#define JasperEngineLinux_h

// Entry point of the X11 backend for exported games. The window is WindowLinux,
// input translation is LinuxInputRouter and the jasperengine::System surface is
// SystemLinux, so what is left here is the frame loop and the GLX context.
class JasperEngineLinux {
public:

    static int init(int argc, char **argv);

    // Engine.vSyncCount: the presenter's swap interval (GLX_EXT_swap_control, or the Vulkan
    // present mode); before the context exists it is read when the context is made
    static void applySwapInterval(int interval);
};

#endif /* JasperEngineLinux_h */
