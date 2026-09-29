// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineWin_h
#define JasperEngineWin_h

// Entry point of the Win32 backend for exported games. The window is WindowWin,
// input translation is WinInputRouter and the jasperengine::System surface is
// SystemWin, so what is left here is the frame loop, the WGL context and the
// build settings.
class JasperEngineWin {
public:

    static int init(int argc, char **argv);
};

#endif /* JasperEngineWin_h */
