// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "AdMobAdapter.h"

#import "AppDelegate.h"
#import "ViewController.h"
#import "Renderer.h"

#include <UserMessagingPlatform/UserMessagingPlatform.h>

@import GoogleMobileAds;

@interface AdMobAdapter () <GADFullScreenContentDelegate>

@property(nonatomic, strong) GADInterstitialAd *interstitial;

@property(nonatomic, strong) ViewController *rootController;

@end

@implementation AdMobAdapter

- (nonnull instancetype)init {
    
    _rootController = (ViewController*)[[(AppDelegate*) [[UIApplication sharedApplication]delegate] window] rootViewController];
    
    return self;
}

- (void)initializeAdMob:(Boolean)tagForChildDirectedTreatment and:(Boolean)tagForUnderAgeOfConsent {
    [GADMobileAds.sharedInstance startWithCompletionHandler:nil];

    GADMobileAds.sharedInstance.requestConfiguration.tagForChildDirectedTreatment = tagForChildDirectedTreatment ? @YES : @NO;
    GADMobileAds.sharedInstance.requestConfiguration.tagForUnderAgeOfConsent = tagForUnderAgeOfConsent ? @YES : @NO;
    
    [self requestConsent: (tagForChildDirectedTreatment || tagForUnderAgeOfConsent)];
}

- (void)setMaxAdContentRating:(int)rating {
    if (rating == 1){
        GADMobileAds.sharedInstance.requestConfiguration.maxAdContentRating = 
            GADMaxAdContentRatingGeneral;
    }else if (rating == 2){
        GADMobileAds.sharedInstance.requestConfiguration.maxAdContentRating = GADMaxAdContentRatingParentalGuidance;
    }else if (rating == 3){
        GADMobileAds.sharedInstance.requestConfiguration.maxAdContentRating =
            GADMaxAdContentRatingTeen;
    }else if (rating == 4){
        GADMobileAds.sharedInstance.requestConfiguration.maxAdContentRating =
            GADMaxAdContentRatingMatureAudience;
    }
}

- (void)requestConsent:(Boolean)tagForUnderAgeOfConsent {
    UMPRequestParameters *parameters = [[UMPRequestParameters alloc] init];
    parameters.tagForUnderAgeOfConsent = tagForUnderAgeOfConsent;
    
    [UMPConsentInformation.sharedInstance
     requestConsentInfoUpdateWithParameters:parameters
     completionHandler:^(NSError* _Nullable error) {
        if (error) {
            NSLog(@"Failed to request consent form with error: %@", [error localizedDescription]);
        }
    }];
}

- (void)loadInterstitial:(NSString *)adUnitID {
    UMPFormStatus formStatus = UMPConsentInformation.sharedInstance.formStatus;
    if (formStatus == UMPFormStatusAvailable) {
        [UMPConsentForm loadWithCompletionHandler:^(UMPConsentForm *form, NSError *loadError) {
            if (loadError) {
                NSLog(@"Failed to load consent form with error: %@", [loadError localizedDescription]);
            } else {
                if (UMPConsentInformation.sharedInstance.consentStatus == UMPConsentStatusRequired) {
                    [form
                     presentFromViewController:self->_rootController
                          completionHandler:^(NSError *_Nullable dismissError) {
                            [self loadInterstitial:adUnitID];
                          }];
                }
            }
        }];
    }
    
    if (UMPConsentInformation.sharedInstance.consentStatus == UMPConsentStatusObtained ||
        UMPConsentInformation.sharedInstance.consentStatus == UMPConsentStatusNotRequired){
        GADRequest *request = [GADRequest request];
        [GADInterstitialAd
         loadWithAdUnitID:adUnitID
         request:request
         completionHandler:^(GADInterstitialAd *ad, NSError *error) {
            if (error) {
                NSLog(@"Failed to load interstitial ad with error: %@", [error localizedDescription]);
                return;
            }
            self.interstitial = ad;
            self.interstitial.fullScreenContentDelegate = self;
        }];
    }
}

- (bool)isInterstitialAdLoaded {
    return (self.interstitial != nil);
}

- (void)showInterstitial {
    if (self.interstitial) {
        [self.interstitial presentFromRootViewController:_rootController];
    } else {
        NSLog(@"Ad wasn't ready");
    }
}

- (void)ad:(nonnull id<GADFullScreenPresentingAd>)ad
didFailToPresentFullScreenContentWithError:(nonnull NSError *)error {
    [Renderer resumeGame];
    NSLog(@"Ad did fail to present full screen content.");
}

- (void)adWillPresentFullScreenContent:(nonnull id<GADFullScreenPresentingAd>)ad {
    [Renderer pauseGame];
    NSLog(@"Ad will present full screen content.");
}

- (void)adDidDismissFullScreenContent:(nonnull id<GADFullScreenPresentingAd>)ad {
    [Renderer resumeGame];
   NSLog(@"Ad did dismiss full screen content.");
}

@end
