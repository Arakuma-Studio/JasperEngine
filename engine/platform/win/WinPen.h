// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WinPen_h
#define WinPen_h

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Pointer input is Windows 8 and later, and the headers only declare it for
// WINVER >= 0x0602, which older MinGW does not default to. The values are
// fixed by the ABI; the functions are resolved by name, so Windows 7 starts.
#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#endif
#ifndef WM_POINTERUP
#define WM_POINTERUP 0x0247
#endif
#ifndef GET_POINTERID_WPARAM
#define GET_POINTERID_WPARAM(wParam) (LOWORD(wParam))
#endif

namespace jasperengine {

    // PT_TOUCH and PT_PEN of POINTER_INPUT_TYPE
    constexpr DWORD WIN_POINTER_TYPE_TOUCH = 2;
    constexpr DWORD WIN_POINTER_TYPE_PEN = 3;

    // POINTER_PEN_INFO's layout, under names of its own for the headers that lack it
    struct WinPointerInfo {
        DWORD pointerType;
        UINT32 pointerId;
        UINT32 frameId;
        UINT32 pointerFlags;
        HANDLE sourceDevice;
        HWND hwndTarget;
        POINT ptPixelLocation;
        POINT ptHimetricLocation;
        POINT ptPixelLocationRaw;
        POINT ptHimetricLocationRaw;
        DWORD dwTime;
        UINT32 historyCount;
        INT32 inputData;
        DWORD dwKeyStates;
        UINT64 performanceCount;
        int buttonChangeType;
    };

    struct WinPointerPenInfo {
        WinPointerInfo pointerInfo;
        UINT32 penFlags;
        UINT32 penMask;
        UINT32 pressure;            // 0..1024
        UINT32 rotation;
        INT32 tiltX;
        INT32 tiltY;
    };

    // the pointer's POINTER_INPUT_TYPE, 0 when unknown (Windows 7, a pointer already gone)
    inline DWORD winPointerType(UINT32 pointerId) {
        using GetPointerTypeFn = BOOL (WINAPI*)(UINT32, DWORD*);
        static const GetPointerTypeFn getPointerType = []() -> GetPointerTypeFn {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32 ? reinterpret_cast<GetPointerTypeFn>(GetProcAddress(user32, "GetPointerType")) : nullptr;
        }();
        DWORD type = 0;
        if (!getPointerType || !getPointerType(pointerId, &type)) return 0;
        return type;
    }

    // a WM_POINTER* pointer's pen pressure, 0..1, or -1 when it is not a pen (a finger) or the
    // pen reports no pressure (it then presses as a mouse does)
    inline float winPenPressure(UINT32 pointerId) {
        if (winPointerType(pointerId) != WIN_POINTER_TYPE_PEN) return -1.0f;
        using GetPointerPenInfoFn = BOOL (WINAPI*)(UINT32, WinPointerPenInfo*);
        static const GetPointerPenInfoFn getPointerPenInfo = []() -> GetPointerPenInfoFn {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32 ? reinterpret_cast<GetPointerPenInfoFn>(GetProcAddress(user32, "GetPointerPenInfo")) : nullptr;
        }();
        // PEN_MASK_PRESSURE
        constexpr UINT32 penMaskPressure = 0x1;
        WinPointerPenInfo info{};
        if (!getPointerPenInfo || !getPointerPenInfo(pointerId, &info) || !(info.penMask & penMaskPressure))
            return -1.0f;
        return info.pressure >= 1024 ? 1.0f : float(info.pressure) / 1024.0f;
    }

    // a WM_MOUSE* message Windows made from a pen or a finger (MI_WP_SIGNATURE in the extra
    // info): the pointer message before it gave the pressure, which this must not undo
    inline bool winIsPointerMouseMessage() {
        return (GetMessageExtraInfo() & 0xFFFFFF00) == 0xFF515700;
    }

}

#endif /* WinPen_h */
