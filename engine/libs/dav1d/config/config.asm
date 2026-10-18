; dav1d's NASM configuration for the engine's build (engine/libs/dav1d/CMakeLists.txt and the Xcode
; targets): what upstream's meson writes into config.asm for x86-64. Only x86-64 is assembled
; (32-bit x86 stays C). Mach-O builds pass -DDAV1D_NASM_PREFIX (symbols with a leading underscore).
%define private_prefix dav1d
%define ARCH_X86_32 0
%define ARCH_X86_64 1
%define PIC 1
%define STACK_ALIGNMENT 16
; the engine does not build with -mavx: the SSE functions stay SSE-encoded
%define FORCE_VEX_ENCODING 0
%ifdef DAV1D_NASM_PREFIX
%define PREFIX 1
%endif
