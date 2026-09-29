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
};

#endif /* JasperEngineLinux_h */
