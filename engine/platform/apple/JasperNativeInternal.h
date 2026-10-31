// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Internal: between JasperNative.m (the Objective-C helper) and NativePlatformApple.mm (the
// engine's side, C++). Not for plugin code.

#import "JasperNative.h"

NS_ASSUME_NONNULL_BEGIN

@interface JasperNativeCall ()
- (instancetype)initWithId:(uint64_t)callId plugin:(NSString*)plugin method:(NSString*)method args:(NSArray*)args;
// the Lua side gave up on it
- (void)markCancelled;
// a failure the bridge reports (an exception of the method): ignored when the call completed
- (void)failWithCode:(NSString*)code message:(NSString*)message;
@end

#ifdef __cplusplus
extern "C" {
#endif

// NativePlatformApple.mm: any thread
void jasperNativeBridgeComplete(uint64_t callId, id _Nullable value, NSString* _Nullable code, NSString* _Nullable message);
void jasperNativeBridgeEmit(NSString* plugin, NSString* event, id _Nullable payload);
void jasperNativeBridgePause(BOOL pause);
void jasperNativeBridgePost(dispatch_block_t block);
// the call left the engine's pending list (completed or cancelled)
void jasperNativeBridgeForget(uint64_t callId);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END
