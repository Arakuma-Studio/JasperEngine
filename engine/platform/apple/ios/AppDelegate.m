// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "AppDelegate.h"

@implementation AppDelegate


- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    return YES;
}

// Every orientation: the view controller narrows it to Screen.orientation / autorotateTo*, and
// Info.plist (Player > Resolution and Presentation) only sets the launch orientation
- (UIInterfaceOrientationMask)application:(UIApplication *)application supportedInterfaceOrientationsForWindow:(UIWindow *)window {
    return UIInterfaceOrientationMaskAll;
}

@end
