#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

extern "C" void arya_on_quit(void);
extern "C" bool arya_should_quit(void);

static NSString *const kAuthor = @"Ali Mohammadi Ruzbahani";
static NSString *const kAuthorTitle = @"AI Security Researcher";
static NSString *const kWebsite = @"https://ruzbahani.com";

@interface AryaWebView : WKWebView
@end

@implementation AryaWebView
- (NSArray<NSURL *> *)aryaFiles:(id<NSDraggingInfo>)info {
    if (!info) return @[];
    return [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                  options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}] ?: @[];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return [self aryaFiles:info].count ? NSDragOperationCopy : [super draggingEntered:info];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    return [self aryaFiles:info].count ? NSDragOperationCopy : [super draggingUpdated:info];
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
    if (![self aryaFiles:info].count) [super draggingExited:info];
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info {
    return [self aryaFiles:info].count ? YES : [super prepareForDragOperation:info];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray<NSURL *> *urls = [self aryaFiles:info];
    if (!urls.count) return [super performDragOperation:info];
    NSMutableArray *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:u.path];
    NSData *json = [NSJSONSerialization dataWithJSONObject:paths options:0 error:nil];
    NSString *arg = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    [self evaluateJavaScript:[NSString stringWithFormat:@"window.aryaDrop && window.aryaDrop(%@)", arg] completionHandler:nil];
    return YES;
}
- (void)concludeDragOperation:(id<NSDraggingInfo>)info {
    if (![self aryaFiles:info].count) [super concludeDragOperation:info];
}
@end

@interface AryaApp : NSObject <NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
@property(strong) NSWindow *window;
@property(strong) AryaWebView *web;
@property(copy) NSString *url;
@end

@implementation AryaApp

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    [self buildMenu];
    NSRect vis = NSScreen.mainScreen.visibleFrame;
    NSRect frame = NSMakeRect(0, 0, MIN(1280, vis.size.width - 40), MIN(820, vis.size.height - 40));
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Arya Terminal Assistant";
    self.window.minSize = NSMakeSize(720, 480);
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = [NSColor colorWithSRGBRed:11 / 255.0 green:13 / 255.0 blue:18 / 255.0 alpha:1];
    self.window.delegate = self;
    [self.window center];
    [self.window setFrameAutosaveName:@"AryaMainWindow"];

    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    self.web = [[AryaWebView alloc] initWithFrame:self.window.contentView.bounds configuration:cfg];
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self.web setValue:@NO forKey:@"drawsBackground"];
    self.web.navigationDelegate = self;
    self.web.UIDelegate = self;
    [self.web addObserver:self forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:nil];
    [self.window.contentView addSubview:self.web];
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:self.url]]];

    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.web];
    [NSApp activateIgnoringOtherApps:YES];
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t;
        if (arya_should_quit()) [NSApp terminate:nil];
    }];
}

- (void)observeValueForKeyPath:(NSString *)path ofObject:(id)obj change:(NSDictionary *)change context:(void *)ctx {
    if (self.web.title.length) self.window.title = self.web.title;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (void)applicationWillTerminate:(NSNotification *)note { arya_on_quit(); }

- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    NSURL *u = action.request.URL;
    NSURL *home = [NSURL URLWithString:self.url];
    BOOL ours = [u.host isEqualToString:home.host] && [u.port isEqual:home.port];
    if (ours && action.targetFrame) { decide(WKNavigationActionPolicyAllow); return; }
    decide(WKNavigationActionPolicyCancel);
    if ([u.scheme isEqualToString:@"http"] || [u.scheme isEqualToString:@"https"] || [u.scheme isEqualToString:@"mailto"])
        [NSWorkspace.sharedWorkspace openURL:u];
}
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)cfg
    forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    NSURL *u = action.request.URL;
    if (u) [NSWorkspace.sharedWorkspace openURL:u];
    return nil;
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)wv { [wv reload]; }

- (void)showAbout:(id)sender {
    NSMutableAttributedString *credits = [NSMutableAttributedString new];
    NSMutableParagraphStyle *center = [NSMutableParagraphStyle new];
    center.alignment = NSTextAlignmentCenter;
    NSDictionary *base = @{NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : NSColor.labelColor,
                           NSParagraphStyleAttributeName : center};
    [credits appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:@"A compact graphical terminal: every command is a key with a full explanation.\n\nCreated by\n"
                                            attributes:base]];
    NSMutableDictionary *bold = [base mutableCopy];
    bold[NSFontAttributeName] = [NSFont boldSystemFontOfSize:12];
    [credits appendAttributedString:[[NSAttributedString alloc] initWithString:kAuthor attributes:bold]];
    [credits appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:[NSString stringWithFormat:@"\n%@\n", kAuthorTitle] attributes:base]];
    NSMutableDictionary *link = [base mutableCopy];
    link[NSLinkAttributeName] = [NSURL URLWithString:kWebsite];
    [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"ruzbahani.com" attributes:link]];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionCredits : credits}];
}

- (void)openWebsite:(id)sender { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:kWebsite]]; }

- (void)buildMenu {
    NSMenu *bar = [NSMenu new];
    NSString *name = @"Arya Terminal Assistant";

    NSMenu *app = [NSMenu new];
    [app addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(showAbout:) keyEquivalent:@""].target = self;
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = app;

    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem *redo = [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""].submenu = edit;

    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Reload" action:@selector(reload:) keyEquivalent:@"r"];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [bar addItemWithTitle:@"View" action:nil keyEquivalent:@""].submenu = view;

    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [win addItem:NSMenuItem.separatorItem];
    [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""].submenu = win;
    NSApp.windowsMenu = win;

    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [help addItemWithTitle:@"Visit ruzbahani.com" action:@selector(openWebsite:) keyEquivalent:@""].target = self;
    [bar addItemWithTitle:@"Help" action:nil keyEquivalent:@""].submenu = help;
    NSApp.helpMenu = help;

    NSApp.mainMenu = bar;
}
@end

extern "C" void arya_mac_run(const char *url) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        AryaApp *delegate = [AryaApp new];
        delegate.url = [NSString stringWithUTF8String:url];
        NSApp.delegate = delegate;
        [NSApp run];
    }
}
