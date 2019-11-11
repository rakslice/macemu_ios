//
//  B2SettingsViewController.m
//  BasiliskII
//
//  Created by Jesús A. Álvarez on 05/07/2015.
//  Copyright (c) 2015 namedfork. All rights reserved.
//

#import "B2SettingsViewController.h"
#import "B2AppDelegate.h"

@interface B2SettingsViewController () <UISplitViewControllerDelegate>

@end

@implementation B2SettingsViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.splitViewController.delegate = self;
    [self showSidebarIfNeededForSize:self.view.bounds.size];
}

- (UISplitViewController *)splitViewController {
    return self.childViewControllers.firstObject;
}

- (BOOL)splitViewController:(UISplitViewController *)splitViewController collapseSecondaryViewController:(UIViewController *)secondaryViewController ontoPrimaryViewController:(UIViewController *)primaryViewController {
    return YES;
}

- (void)showSidebarIfNeededForSize:(CGSize)size {
    if (size.width > size.height && size.width >= 480.0) {
        UITraitCollection *traits = [UITraitCollection traitCollectionWithHorizontalSizeClass:UIUserInterfaceSizeClassRegular];
        [self setOverrideTraitCollection:traits forChildViewController:self.splitViewController];
    } else {
        [self setOverrideTraitCollection:nil forChildViewController:self.splitViewController];
    }
}

- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [self showSidebarIfNeededForSize:size];
}

@end
