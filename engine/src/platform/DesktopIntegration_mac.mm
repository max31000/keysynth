// macOS part of DesktopIntegration.h (ARCHITECTURE §16): NSApplication setup, Apple Events, login item.

#include "platform/DesktopIntegration.h"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include <atomic>

namespace {
std::atomic<bool> g_reopen{false};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_url{false};

// Out-of-bundle runs (dev builds, tests) have no login item and no bundle identifier.
bool inAppBundle() { return [[NSBundle mainBundle] bundleIdentifier] != nil; }
} // namespace

@interface KsAppEventHandler : NSObject
@end

@implementation KsAppEventHandler
- (void)handleReopen:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    (void)event;
    (void)reply;
    g_reopen.store(true);
}
- (void)handleQuit:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    (void)event;
    (void)reply;
    g_quit.store(true);
}
- (void)handleUrl:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    (void)event;
    (void)reply;
    g_url.store(true); // the page that sent the link connects by itself: nothing else to do
}
@end

namespace ks::platform {

void initApplication(bool menuBarApp) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // Bundled builds declare LSUIElement; a console/dev run in desktop mode needs the policy set explicitly
        // (the status item and its menu work in both).
        if (menuBarApp) [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

void installAppEventHandlers() {
    // After JUCE's initialiser: its AppDelegate registers its own kAEGetURL handler, ours replaces it.
    static KsAppEventHandler* handler = [[KsAppEventHandler alloc] init];
    NSAppleEventManager* m = [NSAppleEventManager sharedAppleEventManager];
    [m setEventHandler:handler
           andSelector:@selector(handleReopen:withReplyEvent:)
         forEventClass:kCoreEventClass
            andEventID:kAEReopenApplication];
    [m setEventHandler:handler
           andSelector:@selector(handleQuit:withReplyEvent:)
         forEventClass:kCoreEventClass
            andEventID:kAEQuitApplication];
    [m setEventHandler:handler
           andSelector:@selector(handleUrl:withReplyEvent:)
         forEventClass:kInternetEventClass
            andEventID:kAEGetURL];
}

bool takeAppReopenRequest() { return g_reopen.exchange(false); }
bool takeAppQuitRequest() { return g_quit.exchange(false); }
bool appLaunchedByUrl() { return g_url.load(); }

bool autostartEnabled(const std::string&, const std::string&) {
    if (!inAppBundle()) return false;
    return [SMAppService mainAppService].status == SMAppServiceStatusEnabled;
}

bool setAutostart(bool enabled, const std::filesystem::path&, const std::string&, const std::string&) {
    if (!inAppBundle()) return false;
    NSError* error = nil;
    SMAppService* s = [SMAppService mainAppService];
    const BOOL ok = enabled ? [s registerAndReturnError:&error] : [s unregisterAndReturnError:&error];
    // Unregistering an item that is not registered is success (as deleting a missing Run value on Windows).
    return ok || (!enabled && s.status == SMAppServiceStatusNotRegistered);
}

} // namespace ks::platform
