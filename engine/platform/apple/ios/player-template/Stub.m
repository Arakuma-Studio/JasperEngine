// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// The prebuilt iOS player's app target compiles nothing of the engine: its code, main() included,
// is JasperEnginePlayer.xcframework (linked with -all_load). This keeps the Sources phase non-empty.
void jasperEnginePlayerTemplate(void) {}
