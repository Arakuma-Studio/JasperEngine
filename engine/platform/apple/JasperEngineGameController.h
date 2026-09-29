// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineGameController_h
#define JasperEngineGameController_h

#import <Foundation/Foundation.h>

// Bridges Apple's GameController framework (GCController) to the engine's
// systemGamepad* events. Shared by the iOS and macOS backends.
@interface JasperEngineGameController : NSObject

// Idempotent: starts observing controller connect/disconnect and registers any
// already-connected controllers. Call once after Engine::systemInit.
+ (void)start;

@end

#endif /* JasperEngineGameController_h */
