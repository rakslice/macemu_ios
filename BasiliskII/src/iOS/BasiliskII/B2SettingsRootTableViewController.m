//
//  B2SettingsRootTableViewController.m
//  BasiliskII
//
//  Created by Jesús A. Álvarez on 19/04/2016.
//  Copyright © 2016 namedfork. All rights reserved.
//

#import "B2SettingsRootTableViewController.h"

@interface B2SettingsRootTableViewController ()

@end

@implementation B2SettingsRootTableViewController

- (void)tableView:(UITableView *)tableView willDisplayCell:(UITableViewCell *)cell forRowAtIndexPath:(NSIndexPath *)indexPath {
    BOOL isSidebar = self.splitViewController.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
    cell.accessoryType = isSidebar ? UITableViewCellAccessoryNone : UITableViewCellAccessoryDisclosureIndicator;
}

- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext>  _Nonnull context) {
        [self.tableView reloadData];
    }];
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
}

@end
