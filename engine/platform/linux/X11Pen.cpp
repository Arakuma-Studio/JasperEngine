// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "X11Pen.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <vector>

using namespace jasperengine;

namespace {

    // The few XInput 2 declarations this needs, from the protocol's ABI (X11/extensions/XI2.h and
    // XInput2.h, libXi 1.7), written here: libXi is loaded when there, and its development header
    // must not be needed to build a game either. Where the header exists the layouts are checked
    // against it below.
    constexpr int PEN_XI_ALL_DEVICES = 0;
    constexpr int PEN_XI_SLAVE_POINTER = 3;
    constexpr int PEN_XI_VALUATOR_CLASS = 2;
    constexpr int PEN_XI_DEVICE_CHANGE = 2;            // XIDeviceChangedEvent::reason
    constexpr int PEN_XI_DEVICE_CHANGED = 1;
    constexpr int PEN_XI_BUTTON_PRESS = 4;
    constexpr int PEN_XI_BUTTON_RELEASE = 5;
    constexpr int PEN_XI_MOTION = 6;
    constexpr int PEN_XI_HIERARCHY_CHANGED = 11;
    // bits for event types 0..31: every one selected here
    constexpr int PEN_XI_MASK_BYTES = 4;

    void penSetMask(unsigned char* mask, int event){
        mask[event >> 3] = static_cast<unsigned char>(mask[event >> 3] | (1 << (event & 7)));
    }

    bool penMaskIsSet(const unsigned char* mask, int bit){
        return (mask[bit >> 3] & (1 << (bit & 7))) != 0;
    }

    struct PenXIEventMask {
        int deviceid;
        int mask_len;
        unsigned char* mask;
    };

    struct PenXIAnyClassInfo {
        int type;
        int sourceid;
    };

    struct PenXIValuatorClassInfo {
        int type;
        int sourceid;
        int number;
        Atom label;
        double min;
        double max;
        double value;
        int resolution;
        int mode;
    };

    struct PenXIDeviceInfo {
        int deviceid;
        char* name;
        int use;
        int attachment;
        Bool enabled;
        int num_classes;
        PenXIAnyClassInfo** classes;
    };

    struct PenXIButtonState {
        int mask_len;
        unsigned char* mask;
    };

    struct PenXIValuatorState {
        int mask_len;
        unsigned char* mask;
        double* values;
    };

    struct PenXIModifierState {
        int base;
        int latched;
        int locked;
        int effective;
    };

    struct PenXIDeviceEvent {
        int type;
        unsigned long serial;
        Bool send_event;
        Display* display;
        int extension;
        int evtype;
        Time time;
        int deviceid;
        int sourceid;
        int detail;
        Window root;
        Window event;
        Window child;
        double root_x;
        double root_y;
        double event_x;
        double event_y;
        int flags;
        PenXIButtonState buttons;
        PenXIValuatorState valuators;
        PenXIModifierState mods;
        PenXIModifierState group;
    };

    struct PenXIDeviceChangedEvent {
        int type;
        unsigned long serial;
        Bool send_event;
        Display* display;
        int extension;
        int evtype;
        Time time;
        int deviceid;
        int sourceid;
        int reason;
        int num_classes;
        PenXIAnyClassInfo** classes;
    };

    using PenQueryVersion = Status (*)(Display*, int*, int*);
    using PenQueryDevice = PenXIDeviceInfo* (*)(Display*, int, int*);
    using PenFreeDeviceInfo = void (*)(PenXIDeviceInfo*);
    using PenSelectEvents = int (*)(Display*, Window, PenXIEventMask*, int);

}

#if __has_include(<X11/extensions/XInput2.h>)
#include <X11/extensions/XInput2.h>
namespace {
    static_assert(PEN_XI_ALL_DEVICES == XIAllDevices && PEN_XI_SLAVE_POINTER == XISlavePointer && PEN_XI_VALUATOR_CLASS == XIValuatorClass &&
                  PEN_XI_DEVICE_CHANGE == XIDeviceChange && PEN_XI_DEVICE_CHANGED == XI_DeviceChanged && PEN_XI_BUTTON_PRESS == XI_ButtonPress &&
                  PEN_XI_BUTTON_RELEASE == XI_ButtonRelease && PEN_XI_MOTION == XI_Motion && PEN_XI_HIERARCHY_CHANGED == XI_HierarchyChanged,
                  "XInput 2's constants");
    static_assert(sizeof(PenXIEventMask) == sizeof(XIEventMask) && offsetof(PenXIEventMask, mask) == offsetof(XIEventMask, mask), "XIEventMask");
    static_assert(sizeof(PenXIValuatorClassInfo) == sizeof(XIValuatorClassInfo) && offsetof(PenXIValuatorClassInfo, label) == offsetof(XIValuatorClassInfo, label) &&
                  offsetof(PenXIValuatorClassInfo, value) == offsetof(XIValuatorClassInfo, value), "XIValuatorClassInfo");
    static_assert(sizeof(PenXIDeviceInfo) == sizeof(XIDeviceInfo) && offsetof(PenXIDeviceInfo, use) == offsetof(XIDeviceInfo, use) &&
                  offsetof(PenXIDeviceInfo, classes) == offsetof(XIDeviceInfo, classes), "XIDeviceInfo");
    static_assert(sizeof(PenXIDeviceEvent) == sizeof(XIDeviceEvent) && offsetof(PenXIDeviceEvent, sourceid) == offsetof(XIDeviceEvent, sourceid) &&
                  offsetof(PenXIDeviceEvent, valuators) == offsetof(XIDeviceEvent, valuators), "XIDeviceEvent");
    static_assert(sizeof(PenXIDeviceChangedEvent) == sizeof(XIDeviceChangedEvent) &&
                  offsetof(PenXIDeviceChangedEvent, reason) == offsetof(XIDeviceChangedEvent, reason), "XIDeviceChangedEvent");
    static_assert(XI_HierarchyChanged < PEN_XI_MASK_BYTES * 8, "the selected events fit the mask");
}
#endif

namespace {

    struct PenDevice {
        int id = 0;
        int valuator = -1;          // the "Abs Pressure" valuator's number; -1: not a pen
        double min = 0.0;
        double max = 1.0;
        float pressure = 0.0f;      // an event leaves out the valuators that did not change
    };

    struct PenState {
        Display* display = nullptr;
        int opcode = -1;
        PenQueryDevice queryDevice = nullptr;
        PenFreeDeviceInfo freeDeviceInfo = nullptr;
        PenSelectEvents selectEvents = nullptr;
        std::vector<Window> windows;
        std::vector<PenDevice> devices;     // the slave pointers
        float pressure = -1.0f;
    };

    PenState& penState(){
        static PenState s;
        return s;
    }

    float penNormalize(const PenDevice& device, double value){
        return (float)std::clamp((value - device.min) / (device.max - device.min), 0.0, 1.0);
    }

    // a device unplugged between the query and the selection fails it with BadValue, which
    // Xlib's default handler would exit on; its hierarchy event selects again
    int penIgnoreError(Display*, XErrorEvent*){
        return 0;
    }

    void penEnumerate(PenState& s){
        s.devices.clear();
        // only_if_exists: no driver named the label yet, so no device has it
        const Atom pressureLabel = XInternAtom(s.display, "Abs Pressure", True);
        int count = 0;
        PenXIDeviceInfo* infos = s.queryDevice(s.display, PEN_XI_ALL_DEVICES, &count);
        if (!infos) return;
        for (int i = 0; i < count; i++){
            const PenXIDeviceInfo& info = infos[i];
            if (info.use != PEN_XI_SLAVE_POINTER) continue;
            PenDevice device;
            device.id = info.deviceid;
            for (int c = 0; c < info.num_classes; c++){
                if (info.classes[c]->type != PEN_XI_VALUATOR_CLASS) continue;
                const PenXIValuatorClassInfo* valuator = reinterpret_cast<const PenXIValuatorClassInfo*>(info.classes[c]);
                if (pressureLabel == None || valuator->label != pressureLabel || valuator->max <= valuator->min) continue;
                device.valuator = valuator->number;
                device.min = valuator->min;
                device.max = valuator->max;
                device.pressure = penNormalize(device, valuator->value);
            }
            s.devices.push_back(device);
        }
        s.freeDeviceInfo(infos);
    }

    void penSelect(PenState& s, const Window* windows, size_t windowCount){
        if (s.devices.empty() || windowCount == 0) return;
        unsigned char bits[PEN_XI_MASK_BYTES] = {};
        penSetMask(bits, PEN_XI_MOTION);
        penSetMask(bits, PEN_XI_BUTTON_PRESS);
        penSetMask(bits, PEN_XI_BUTTON_RELEASE);
        penSetMask(bits, PEN_XI_DEVICE_CHANGED);
        std::vector<PenXIEventMask> masks;
        masks.reserve(s.devices.size());
        for (const PenDevice& device : s.devices)
            masks.push_back(PenXIEventMask{device.id, (int)sizeof(bits), bits});

        XSync(s.display, False);
        XErrorHandler previous = XSetErrorHandler(penIgnoreError);
        for (size_t i = 0; i < windowCount; i++)
            s.selectEvents(s.display, windows[i], masks.data(), (int)masks.size());
        XSync(s.display, False);
        XSetErrorHandler(previous);
    }

    float penReading(PenState& s, const PenXIDeviceEvent& event){
        for (PenDevice& device : s.devices){
            if (device.id != event.sourceid) continue;
            if (device.valuator < 0) return -1.0f;
            // values holds only the valuators the mask sets, in order
            const double* value = event.valuators.values;
            for (int bit = 0; bit < event.valuators.mask_len * 8; bit++){
                if (!penMaskIsSet(event.valuators.mask, bit)) continue;
                if (bit == device.valuator){
                    device.pressure = penNormalize(device, *value);
                    break;
                }
                value++;
            }
            return device.pressure;
        }
        return -1.0f;
    }

}

bool X11Pen::start(Display* display, Window window){
    PenState& s = penState();
    if (s.display || !display || !window) return false;
    int opcode = 0;
    int eventBase = 0;
    int errorBase = 0;
    if (!XQueryExtension(display, "XInputExtension", &opcode, &eventBase, &errorBase)) return false;

    // It stays loaded: its first call registers an extension on the display whose close hook
    // XCloseDisplay calls (as XRandR's, HostServices::refreshRate)
    static void* library = dlopen("libXi.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!library) return false;
    auto queryVersion = reinterpret_cast<PenQueryVersion>(dlsym(library, "XIQueryVersion"));
    auto queryDevice = reinterpret_cast<PenQueryDevice>(dlsym(library, "XIQueryDevice"));
    auto freeDeviceInfo = reinterpret_cast<PenFreeDeviceInfo>(dlsym(library, "XIFreeDeviceInfo"));
    auto selectEvents = reinterpret_cast<PenSelectEvents>(dlsym(library, "XISelectEvents"));
    if (!queryVersion || !queryDevice || !freeDeviceInfo || !selectEvents) return false;
    // the server sends XInput 2 events only to a client that announced the version
    int major = 2;
    int minor = 0;
    if (queryVersion(display, &major, &minor) != Success || major < 2) return false;

    s.display = display;
    s.opcode = opcode;
    s.queryDevice = queryDevice;
    s.freeDeviceInfo = freeDeviceInfo;
    s.selectEvents = selectEvents;
    s.windows.assign(1, window);
    s.pressure = -1.0f;

    // a tablet plugged in later adds its devices
    unsigned char bits[PEN_XI_MASK_BYTES] = {};
    penSetMask(bits, PEN_XI_HIERARCHY_CHANGED);
    PenXIEventMask hierarchy{PEN_XI_ALL_DEVICES, (int)sizeof(bits), bits};
    selectEvents(display, DefaultRootWindow(display), &hierarchy, 1);

    penEnumerate(s);
    penSelect(s, s.windows.data(), s.windows.size());
    return true;
}

void X11Pen::addWindow(Window window){
    PenState& s = penState();
    if (!s.display || !window || std::find(s.windows.begin(), s.windows.end(), window) != s.windows.end()) return;
    s.windows.push_back(window);
    penSelect(s, &window, 1);
}

void X11Pen::removeWindow(Window window){
    PenState& s = penState();
    s.windows.erase(std::remove(s.windows.begin(), s.windows.end(), window), s.windows.end());
}

bool X11Pen::handleEvent(XEvent& event){
    PenState& s = penState();
    if (!s.display || event.type != GenericEvent || event.xcookie.extension != s.opcode) return false;
    if (!XGetEventData(s.display, &event.xcookie)) return true;
    bool devicesChanged = false;
    switch (event.xcookie.evtype){
        case PEN_XI_MOTION:
        case PEN_XI_BUTTON_PRESS:
        case PEN_XI_BUTTON_RELEASE:
            s.pressure = penReading(s, *static_cast<const PenXIDeviceEvent*>(event.xcookie.data));
            break;
        case PEN_XI_HIERARCHY_CHANGED:
            devicesChanged = true;
            break;
        case PEN_XI_DEVICE_CHANGED:
            devicesChanged = static_cast<const PenXIDeviceChangedEvent*>(event.xcookie.data)->reason == PEN_XI_DEVICE_CHANGE;
            break;
        default:
            break;
    }
    XFreeEventData(s.display, &event.xcookie);
    if (devicesChanged){
        penEnumerate(s);
        penSelect(s, s.windows.data(), s.windows.size());
    }
    return true;
}

float X11Pen::pressure(){
    return penState().pressure;
}

void X11Pen::stop(){
    PenState& s = penState();
    s.display = nullptr;
    s.opcode = -1;
    s.windows.clear();
    s.devices.clear();
    s.pressure = -1.0f;
}
