// macOS menu-bar item + popover status widget (platform/MenuBar.h, ARCHITECTURE §16 *macOS*). Plain AppKit, built
// in code (no nib): NSStatusItem, NSPopover (transient), NSStackView layout, SF Symbols, system colours, so light /
// dark mode and accessibility settings follow the system.

#include "platform/MenuBar.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSTextField* label(NSString* text, CGFloat size, NSFontWeight weight, NSColor* color) {
    NSTextField* l = [NSTextField labelWithString:text];
    l.font = [NSFont systemFontOfSize:size weight:weight];
    l.textColor = color;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

NSImageView* symbol(NSString* name, NSColor* tint) {
    NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    NSImageView* v = [NSImageView imageViewWithImage:img ?: [[NSImage alloc] init]];
    v.contentTintColor = tint;
    v.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:12 weight:NSFontWeightRegular];
    [v.widthAnchor constraintEqualToConstant:18].active = YES;
    return v;
}

NSStackView* hstack(NSArray<NSView*>* views, CGFloat spacing) {
    NSStackView* s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = NSLayoutAttributeCenterY;
    s.spacing = spacing;
    return s;
}

NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
    NSStackView* s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = spacing;
    return s;
}

NSView* spacer() {
    NSView* v = [[NSView alloc] init];
    [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return v;
}

// Rounded filled container. NSBox (not a layer colour): dynamic system colours re-resolve when the appearance
// (light / dark) changes while the app runs.
NSBox* roundedBox(NSView* content, NSColor* fill, NSColor* border, CGFloat radius, NSEdgeInsets insets) {
    NSBox* box = [[NSBox alloc] init];
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.contentViewMargins = NSZeroSize;
    box.cornerRadius = radius;
    box.fillColor = fill;
    box.borderColor = border ?: [NSColor clearColor];
    box.borderWidth = border ? 0.5 : 0;
    NSView* inner = [[NSView alloc] init];
    box.contentView = inner;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [inner addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:inner.leadingAnchor constant:insets.left],
        [content.trailingAnchor constraintEqualToAnchor:inner.trailingAnchor constant:-insets.right],
        [content.topAnchor constraintEqualToAnchor:inner.topAnchor constant:insets.top],
        [content.bottomAnchor constraintEqualToAnchor:inner.bottomAnchor constant:-insets.bottom],
    ]];
    return box;
}

// Grouped rows (like System Settings): a faint label-colour fill works on the popover's vibrant background in both
// appearances.
NSView* card(NSStackView* content) {
    return roundedBox(content, [[NSColor labelColor] colorWithAlphaComponent:0.045], [NSColor separatorColor], 8,
                      NSEdgeInsetsMake(10, 12, 10, 12));
}


NSLevelIndicator* meter(double max, double warning, double critical) {
    NSLevelIndicator* m = [[NSLevelIndicator alloc] init];
    m.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
    m.minValue = 0;
    m.maxValue = max;
    m.warningValue = warning;
    m.criticalValue = critical;
    m.editable = NO;
    [m.heightAnchor constraintEqualToConstant:8].active = YES;
    [m setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return m;
}

} // namespace

@interface KsMenuBarController : NSObject <NSPopoverDelegate, NSMenuDelegate>
@end

@implementation KsMenuBarController {
  @public
    ks::platform::MenuBarActions _a;
    BOOL _ru;
    NSString* _noticeTitle;
    NSString* _noticeText;
    NSStatusItem* _item;
    NSImage* _icon;
    NSPopover* _popover;
    NSTimer* _timer;
    // widget fields
    NSBox* _pill;
    NSTextField* _pillText;
    NSBox* _pillDot;
    NSTextField* _version;
    NSBox* _banner;
    NSTextField* _bannerText;
    NSTextField* _device;
    NSTextField* _deviceDetail;
    NSLevelIndicator* _cpu;
    NSTextField* _cpuText;
    NSLevelIndicator* _level;
    NSTextField* _levelText;
    NSTextField* _voices;
    NSTextField* _preset;
    NSTextField* _midi;
    NSTextField* _clients;
    NSTextField* _cloud;
    NSButton* _openLocal;
    NSSwitch* _autostart;
    NSTextField* _autostartLabel;
}

- (NSString*)t:(const char*)ru en:(const char*)en {
    return [NSString stringWithUTF8String:_ru ? ru : en];
}

- (BOOL)inBundle {
    return [[NSBundle mainBundle] bundleIdentifier] != nil;
}

// ---------------------------------------------------------------------------------------------------------------
- (void)installWithTemplate:(NSImage*)tmpl {
    _item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSStatusBarButton* b = _item.button;
    b.image = tmpl;
    b.target = self;
    b.action = @selector(clicked:);
    [b sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    _popover = [[NSPopover alloc] init];
    _popover.behavior = NSPopoverBehaviorTransient;
    _popover.animates = YES;
    _popover.delegate = self;
    NSViewController* vc = [[NSViewController alloc] init];
    vc.view = [self buildContent];
    _popover.contentViewController = vc;
    [self refreshTooltip];
}

- (void)uninstall {
    [_timer invalidate];
    _timer = nil;
    [_popover close];
    if (_item) [[NSStatusBar systemStatusBar] removeStatusItem:_item];
    _item = nil;
}

- (void)clicked:(id)sender {
    (void)sender;
    NSEvent* e = NSApp.currentEvent;
    const BOOL secondary = e.type == NSEventTypeRightMouseUp || (e.modifierFlags & NSEventModifierFlagControl);
    if (secondary) {
        [_popover close];
        _item.menu = [self buildMenu]; // shown by the click, removed again in menuDidClose:
        [_item.button performClick:nil];
        return;
    }
    if (_popover.shown) {
        [_popover performClose:nil];
        return;
    }
    [self refreshWidget];
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
    [_popover showRelativeToRect:_item.button.bounds ofView:_item.button preferredEdge:NSRectEdgeMinY];
}

// ---------------------------------------------------------------------------------------------------------------
- (NSMenu*)buildMenu {
    NSMenu* m = [[NSMenu alloc] init];
    m.delegate = self;
    m.autoenablesItems = NO;
    auto add = [&](NSString* title, SEL sel, NSString* key) {
        NSMenuItem* it = [m addItemWithTitle:title action:sel keyEquivalent:key ?: @""];
        it.target = self;
        return it;
    };
    NSMenuItem* head = [m addItemWithTitle:[NSString stringWithFormat:@"keysynth %@", ns(_a.status ? _a.status().version : "")]
                                    action:nil
                             keyEquivalent:@""];
    head.enabled = NO;
    [m addItem:[NSMenuItem separatorItem]];
    add([self t:"Открыть keysynth" en:"Open keysynth"], @selector(openUi:), @"o");
    NSMenuItem* local = add([self t:"Открыть локально" en:"Open Locally"], @selector(openLocal:), nil);
    local.enabled = _a.status ? _a.status().localAvailable : NO;
    [m addItem:[NSMenuItem separatorItem]];
    add([self t:"Перезапустить звук" en:"Restart Audio"], @selector(restartAudio:), @"r");
    NSMenuItem* login = add([self t:"Открывать при входе" en:"Open at Login"], @selector(toggleAutostartMenu:), nil);
    login.state = _a.status && _a.status().autostart ? NSControlStateValueOn : NSControlStateValueOff;
    login.enabled = [self inBundle];
    add([self t:"Показать журнал" en:"Show Log"], @selector(openLogs:), nil);
    [m addItem:[NSMenuItem separatorItem]];
    add([self t:"Выйти из keysynth" en:"Quit keysynth"], @selector(quit:), @"q");
    return m;
}

- (void)menuDidClose:(NSMenu*)menu {
    (void)menu;
    _item.menu = nil; // left click must reach clicked: again
}

// ---------------------------------------------------------------------------------------------------------------
- (NSView*)buildContent {
    NSColor* secondary = [NSColor secondaryLabelColor];

    // Header: icon, name + version, status pill.
    NSImageView* icon = [NSImageView imageViewWithImage:_icon ?: [NSImage imageNamed:NSImageNameApplicationIcon]];
    [icon.widthAnchor constraintEqualToConstant:32].active = YES;
    [icon.heightAnchor constraintEqualToConstant:32].active = YES;
    NSTextField* name = label(@"keysynth", 15, NSFontWeightSemibold, [NSColor labelColor]);
    _version = label(@"", 11, NSFontWeightRegular, secondary);
    _pillDot = roundedBox([[NSView alloc] init], [NSColor systemGreenColor], nil, 3.5, NSEdgeInsetsMake(0, 0, 0, 0));
    [_pillDot.widthAnchor constraintEqualToConstant:7].active = YES;
    [_pillDot.heightAnchor constraintEqualToConstant:7].active = YES;
    _pillText = label(@"", 11, NSFontWeightMedium, [NSColor labelColor]);
    _pill = roundedBox(hstack(@[ _pillDot, _pillText ], 5), [NSColor clearColor], nil, 10, NSEdgeInsetsMake(3, 8, 3, 9));
    NSStackView* header = hstack(@[ icon, vstack(@[ name, _version ], 1), spacer(), _pill ], 10);

    // Notice banner (port busy, failed restart, ...).
    _bannerText = label(@"", 11, NSFontWeightRegular, [NSColor labelColor]);
    _bannerText.lineBreakMode = NSLineBreakByWordWrapping;
    _bannerText.maximumNumberOfLines = 4;
    _bannerText.preferredMaxLayoutWidth = 270;
    NSStackView* bannerRow = hstack(@[ symbol(@"exclamationmark.triangle.fill", [NSColor systemOrangeColor]), _bannerText ], 6);
    bannerRow.alignment = NSLayoutAttributeTop;
    _banner = roundedBox(bannerRow, [[NSColor systemOrangeColor] colorWithAlphaComponent:0.15], nil, 8,
                         NSEdgeInsetsMake(8, 10, 8, 10));
    _banner.hidden = YES;

    // Audio card.
    _device = label(@"", 12, NSFontWeightSemibold, [NSColor labelColor]);
    _deviceDetail = label(@"", 11, NSFontWeightRegular, secondary);
    _cpu = meter(100, 70, 90);
    _cpuText = label(@"", 11, NSFontWeightRegular, secondary);
    _cpuText.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    _level = meter(60, 54, 59); // dBFS + 60: warning at -6, critical at -1
    _levelText = label(@"", 11, NSFontWeightRegular, secondary);
    _levelText.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSTextField* cpuLbl = label(@"CPU", 11, NSFontWeightRegular, secondary);
    NSTextField* lvlLbl = label([self t:"Выход" en:"Output"], 11, NSFontWeightRegular, secondary);
    for (NSTextField* l in @[ cpuLbl, lvlLbl ]) [l.widthAnchor constraintEqualToConstant:48].active = YES;
    for (NSTextField* l in @[ _cpuText, _levelText ]) {
        l.alignment = NSTextAlignmentRight;
        [l.widthAnchor constraintEqualToConstant:58].active = YES;
    }
    _voices = label(@"", 11, NSFontWeightRegular, secondary);
    NSStackView* audio = vstack(@[
        hstack(@[ symbol(@"hifispeaker.fill", [NSColor controlAccentColor]), vstack(@[ _device, _deviceDetail ], 1) ], 8),
        hstack(@[ cpuLbl, _cpu, _cpuText ], 8),
        hstack(@[ lvlLbl, _level, _levelText ], 8),
        _voices,
    ], 7);
    for (NSView* row in @[ audio.arrangedSubviews[1], audio.arrangedSubviews[2] ])
        [row.widthAnchor constraintEqualToAnchor:audio.widthAnchor].active = YES;

    // Session card.
    _preset = label(@"", 12, NSFontWeightRegular, [NSColor labelColor]);
    _midi = label(@"", 12, NSFontWeightRegular, [NSColor labelColor]);
    _clients = label(@"", 12, NSFontWeightRegular, [NSColor labelColor]);
    _cloud = label(@"", 12, NSFontWeightRegular, [NSColor labelColor]);
    NSStackView* session = vstack(@[
        hstack(@[ symbol(@"music.note", secondary), _preset ], 6),
        hstack(@[ symbol(@"pianokeys", secondary), _midi ], 6),
        hstack(@[ symbol(@"display", secondary), _clients ], 6),
        hstack(@[ symbol(@"icloud", secondary), _cloud ], 6),
    ], 6);

    // Actions.
    NSButton* open = [NSButton buttonWithTitle:[self t:"Открыть keysynth" en:"Open keysynth"] target:self action:@selector(openUi:)];
    open.keyEquivalent = @"\r";
    open.controlSize = NSControlSizeLarge;
    [open setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    _openLocal = [NSButton buttonWithTitle:[self t:"Локально" en:"Locally"] target:self action:@selector(openLocal:)];
    _openLocal.controlSize = NSControlSizeLarge;
    _openLocal.toolTip = [self t:"Интерфейс с этого компьютера (без интернета)" en:"The UI served by this computer (offline)"];
    NSStackView* openRow = hstack(@[ open, _openLocal ], 8);

    NSButton* restart = [NSButton buttonWithTitle:[self t:"Перезапустить звук" en:"Restart Audio"] target:self action:@selector(restartAudio:)];
    restart.bezelStyle = NSBezelStyleRounded;
    _autostart = [[NSSwitch alloc] init];
    _autostart.controlSize = NSControlSizeMini;
    _autostart.target = self;
    _autostart.action = @selector(toggleAutostartSwitch:);
    _autostartLabel = label([self t:"При входе" en:"At login"], 11, NSFontWeightRegular, secondary);
    _autostartLabel.toolTip = [self t:"Запускать keysynth при входе в систему" en:"Start keysynth when you log in"];
    NSStackView* toolsRow = hstack(@[ restart, spacer(), _autostartLabel, _autostart ], 6);

    NSButton* logs = [NSButton buttonWithTitle:[self t:"Журнал" en:"Log"] target:self action:@selector(openLogs:)];
    logs.bezelStyle = NSBezelStyleInline;
    logs.bordered = NO;
    logs.contentTintColor = secondary;
    logs.image = [NSImage imageWithSystemSymbolName:@"doc.text" accessibilityDescription:nil];
    logs.imagePosition = NSImageLeading;
    NSButton* quit = [NSButton buttonWithTitle:[self t:"Выйти" en:"Quit"] target:self action:@selector(quit:)];
    quit.bordered = NO;
    quit.contentTintColor = secondary;
    quit.image = [NSImage imageWithSystemSymbolName:@"power" accessibilityDescription:nil];
    quit.imagePosition = NSImageLeading;
    NSStackView* footer = hstack(@[ logs, spacer(), quit ], 8);

    NSBox* sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;

    NSView* audioCard = card(audio);
    NSView* sessionCard = card(session);
    NSStackView* root = vstack(@[ header, _banner, audioCard, sessionCard, openRow, toolsRow, sep, footer ], 10);
    root.edgeInsets = NSEdgeInsetsMake(14, 14, 10, 14);
    root.alignment = NSLayoutAttributeLeading;
    for (NSView* v in @[ header, _banner, audioCard, sessionCard, openRow, toolsRow, sep, footer ])
        [v.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-28].active = YES;
    [root.widthAnchor constraintEqualToConstant:320].active = YES;
    [root setCustomSpacing:6 afterView:sep];
    return root;
}

// ---------------------------------------------------------------------------------------------------------------
- (void)setPill:(NSString*)text color:(NSColor*)c {
    _pillText.stringValue = text;
    _pillDot.fillColor = c;
    _pill.fillColor = [c colorWithAlphaComponent:0.16];
}

- (void)refreshWidget {
    if (!_a.status) return;
    const ks::platform::MenuBarStatus s = _a.status();
    _version.stringValue = [NSString stringWithFormat:@"%@ %@", [self t:"версия" en:"version"], ns(s.version)];

    if (!s.engineOk) [self setPill:[self t:"Порт занят" en:"Port busy"] color:[NSColor systemOrangeColor]];
    else if (!s.audioRunning) [self setPill:[self t:"Нет звука" en:"No audio"] color:[NSColor systemOrangeColor]];
    else [self setPill:[self t:"Работает" en:"Running"] color:[NSColor systemGreenColor]];

    NSString* notice = _noticeText;
    if (!s.engineOk && s.portProblem.size()) notice = ns(s.portProblem);
    if (notice.length) {
        _bannerText.stringValue = _noticeTitle.length && notice == _noticeText
                                      ? [NSString stringWithFormat:@"%@\n%@", _noticeTitle, notice]
                                      : notice;
        _banner.hidden = NO;
    } else {
        _banner.hidden = YES;
    }

    if (s.audioRunning) {
        _device.stringValue = ns(s.audioDevice);
        const double ms = s.sampleRate > 0 ? 1000.0 * s.bufferSize / s.sampleRate : 0.0;
        _deviceDetail.stringValue = [NSString stringWithFormat:@"%@ · %g kHz · %d · %.1f %@", ns(s.audioType),
                                                               s.sampleRate / 1000.0, s.bufferSize, ms,
                                                               [self t:"мс" en:"ms"]];
    } else {
        _device.stringValue = [self t:"Аудиоустройство не открыто" en:"No audio device open"];
        _deviceDetail.stringValue = [self t:"Подключите устройство и нажмите «Перезапустить звук»"
                                        en:"Connect a device and press Restart Audio"];
    }
    const double cpu = std::clamp(static_cast<double>(s.cpu) * 100.0, 0.0, 100.0);
    _cpu.doubleValue = cpu;
    _cpuText.stringValue = [NSString stringWithFormat:@"%.0f %%", cpu];
    const double db = std::max(-60.0, static_cast<double>(s.masterDb));
    _level.doubleValue = db + 60.0;
    _levelText.stringValue = db <= -60.0 ? @"—" : [NSString stringWithFormat:@"%.0f dB", db];
    _voices.stringValue = [NSString stringWithFormat:@"%@: %d", [self t:"Голоса" en:"Voices"], s.voices];

    _preset.stringValue = s.preset.empty() ? [self t:"Пресет не загружен" en:"No preset"] : ns(s.preset);
    if (s.midiInputs.empty()) {
        _midi.stringValue = [self t:"Нет MIDI-входов" en:"No MIDI inputs"];
        _midi.textColor = [NSColor secondaryLabelColor];
    } else {
        NSMutableArray* names = [NSMutableArray array];
        for (const auto& n : s.midiInputs) [names addObject:ns(n)];
        _midi.stringValue = [names componentsJoinedByString:@", "];
        _midi.textColor = [NSColor labelColor];
    }
    _clients.stringValue = s.localClients + s.remoteClients == 0
                               ? [self t:"Интерфейс не открыт" en:"No UI connected"]
                               : [NSString stringWithFormat:@"%@: %d · %@: %d", [self t:"Интерфейс" en:"UI"], s.localClients,
                                                            [self t:"пульт" en:"remote"], s.remoteClients];
    if (!s.cloudEnabled) _cloud.stringValue = [self t:"Облако выключено" en:"Cloud off"];
    else if (s.cloudLinked) _cloud.stringValue = ns(s.cloudAccount);
    else _cloud.stringValue = [self t:"Облако не подключено" en:"Cloud not linked"];

    _openLocal.enabled = s.localAvailable;
    _autostart.state = s.autostart ? NSControlStateValueOn : NSControlStateValueOff;
    _autostart.enabled = [self inBundle];
}

- (void)refreshTooltip {
    if (_a.tooltip) _item.button.toolTip = ns(_a.tooltip());
    // Attention tint while a notice is pending (template images take the button's tint).
    _item.button.contentTintColor = _noticeText.length ? [NSColor systemOrangeColor] : nil;
}

- (void)popoverWillShow:(NSNotification*)n {
    (void)n;
    _timer = [NSTimer timerWithTimeInterval:0.25 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)popoverDidClose:(NSNotification*)n {
    (void)n;
    [_timer invalidate];
    _timer = nil;
}

- (void)tick:(NSTimer*)t {
    (void)t;
    [self refreshWidget];
}

// ---------------------------------------------------------------------------------------------------------------
- (void)openUi:(id)s {
    (void)s;
    [_popover performClose:nil];
    if (_a.openUi) _a.openUi();
}
- (void)openLocal:(id)s {
    (void)s;
    [_popover performClose:nil];
    if (_a.openLocal) _a.openLocal();
}
- (void)openLogs:(id)s {
    (void)s;
    [_popover performClose:nil];
    if (_a.openLogs) _a.openLogs();
}
- (void)quit:(id)s {
    (void)s;
    [_popover close];
    if (_a.quit) _a.quit();
}
- (void)restartAudio:(id)s {
    (void)s;
    if (!_a.restartAudio) return;
    const std::string err = _a.restartAudio();
    if (err.empty()) [self clearNotice];
    else [self notice:[self t:"Звук не перезапущен" en:"Audio restart failed"] text:ns(err)];
    [self refreshWidget];
}
- (void)setAutostartTo:(BOOL)on {
    if (!_a.setAutostart || !_a.setAutostart(on))
        [self notice:@"keysynth" text:[self t:"Не удалось изменить автозапуск" en:"Could not change Open at Login"]];
    [self refreshWidget];
}
- (void)toggleAutostartSwitch:(NSSwitch*)sw {
    [self setAutostartTo:sw.state == NSControlStateValueOn];
}
- (void)toggleAutostartMenu:(NSMenuItem*)it {
    [self setAutostartTo:it.state != NSControlStateValueOn];
}

- (void)notice:(NSString*)title text:(NSString*)text {
    _noticeTitle = title;
    _noticeText = text;
    [self refreshTooltip];
    if (_popover.shown) [self refreshWidget];
}
- (void)clearNotice {
    _noticeTitle = nil;
    _noticeText = nil;
    [self refreshTooltip];
}
@end

namespace ks::platform {

struct MenuBar::Impl {
    KsMenuBarController* c = nil;
};

MenuBar::MenuBar(MenuBarActions actions, bool russian, const void* templatePng, size_t templateSize,
                 const void* iconPng, size_t iconSize)
    : impl_(std::make_unique<Impl>()) {
    @autoreleasepool {
        KsMenuBarController* c = [[KsMenuBarController alloc] init];
        c->_a = std::move(actions);
        c->_ru = russian;
        NSImage* tmpl = [[NSImage alloc] initWithData:[NSData dataWithBytes:templatePng length:templateSize]];
        tmpl.size = NSMakeSize(18, 18);
        [tmpl setTemplate:YES];
        c->_icon = [[NSImage alloc] initWithData:[NSData dataWithBytes:iconPng length:iconSize]];
        [c installWithTemplate:tmpl];
        impl_->c = c;
    }
}

MenuBar::~MenuBar() { [impl_->c uninstall]; }

void MenuBar::notify(const std::string& title, const std::string& text) {
    if (text.empty()) [impl_->c clearNotice];
    else [impl_->c notice:ns(title) text:ns(text)];
}

void MenuBar::refresh() { [impl_->c refreshTooltip]; }

} // namespace ks::platform
