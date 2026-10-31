// The clone sheet: an address, or one of the repositories of the connected accounts picked in the list, the folder
// the clone goes in, and the clone's progress. The repository then opens in place of the start page, or in a new tab.

#import "MacUI.h"

#include "clone.h"

// A repository: its name (and a lock when it is private), when it last changed, its path and description.
@interface MGCloneCell : NSTableCellView
@property(nonatomic, strong) NSTextField* detail;
@property(nonatomic, strong) NSTextField* when;
@property(nonatomic, strong) NSImageView* lock;
@end

@implementation MGCloneCell

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    NSTextField* name = MakeLabel(@"", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _detail = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _when = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _lock = [NSImageView imageViewWithImage:Symbol(@"lock.fill", 10, NSFontWeightRegular)];
    _lock.contentTintColor = NSColor.secondaryLabelColor;
    _lock.toolTip = @"Private";
    for (NSView* v in @[ name, _detail, _when, _lock ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:v];
    }
    self.textField = name;
    [name setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_detail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_when setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [name.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:6],
        [name.topAnchor constraintEqualToAnchor:self.topAnchor constant:4],
        [_lock.leadingAnchor constraintEqualToAnchor:name.trailingAnchor constant:4],
        [_lock.centerYAnchor constraintEqualToAnchor:name.centerYAnchor],
        [_when.leadingAnchor constraintGreaterThanOrEqualToAnchor:_lock.trailingAnchor constant:8],
        [_when.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-6],
        [_when.centerYAnchor constraintEqualToAnchor:name.centerYAnchor],
        [_detail.leadingAnchor constraintEqualToAnchor:name.leadingAnchor],
        [_detail.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-6],
        [_detail.topAnchor constraintEqualToAnchor:name.bottomAnchor constant:2],
    ]];
    return self;
}

@end

@implementation MGCloneSheet {
    __weak MGRepoWindowController* _owner;
    std::shared_ptr<CloneModel> _model;
    NSTextField* _field;
    NSTableView* _table;
    NSTextField* _folder;
    NSButton* _choose;
    NSButton* _cancel;
    NSButton* _clone;
    NSProgressIndicator* _spinner;
    NSTextField* _status;
    std::vector<std::pair<int, int>> _rows;  // (account, repository): repository -1 is the account's header, account -1 a note
    NSString* _note;
    int _listed;
    BOOL _filling;     // the table is being refilled: its selection changes are not the user's
    MGCloneSheet* _keep;  // alive while the sheet is up
}

- (instancetype)initWithOwner:(MGRepoWindowController*)owner {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 580, 560)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.minSize = NSMakeSize(460, 420);
    if (!(self = [super initWithWindow:w])) return nil;
    _owner = owner;
    _model = std::make_shared<CloneModel>();
    _listed = -1;

    NSTextField* title = MakeLabel(@"Clone a Repository", [NSFont systemFontOfSize:13 weight:NSFontWeightBold], NSColor.labelColor);
    _field = [NSTextField textFieldWithString:@""];
    _field.placeholderString = @"An address, or words of one of your repositories";
    _field.delegate = self;
    _table = [[NSTableView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _table.style = NSTableViewStyleInset;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(doubleClicked:);
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSBezelBorder;
    [scroll setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;

    _folder = [NSTextField textFieldWithString:@""];
    _folder.delegate = self;
    _choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(choose:)];
    NSStackView* folderRow = [NSStackView stackViewWithViews:@[ MakeLabel(@"Folder", [NSFont systemFontOfSize:13], NSColor.secondaryLabelColor), _folder, _choose ]];
    folderRow.spacing = 8;
    [_folder setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.hidden = YES;
    _status = [NSTextField wrappingLabelWithString:@""];
    _status.font = [NSFont systemFontOfSize:11];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.maximumNumberOfLines = 3;
    _status.selectable = YES;
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* statusRow = [NSStackView stackViewWithViews:@[ _spinner, _status ]];
    statusRow.alignment = NSLayoutAttributeTop;
    statusRow.spacing = 6;

    _cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    _cancel.keyEquivalent = @"\033";
    _clone = [NSButton buttonWithTitle:@"Clone" target:self action:@selector(start:)];
    _clone.keyEquivalent = @"\r";
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ spacer, _cancel, _clone ]];
    buttons.spacing = 10;

    NSStackView* stack = [NSStackView stackViewWithViews:@[ title, _field, scroll, folderRow, statusRow, buttons ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView* v in @[ _field, scroll, folderRow, statusRow, buttons ]) [v.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-40].active = YES;
    NSView* content = w.contentView;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    return self;
}

- (void)begin {
    NSWindow* parent = _owner.window;
    if (!parent) return;
    _keep = self;
    __weak MGCloneSheet* weakSelf = self;
    _model->onChange = [weakSelf] { [weakSelf modelChanged]; };
    _model->folder = DefaultCloneFolder();
    _folder.stringValue = NS(_model->folder);
    _model->Load();
    _listed = _model->listed;
    [self fill];
    [self update];
    [parent beginSheet:self.window completionHandler:nil];
    [self.window makeFirstResponder:_field];
}

- (void)finish {
    _model->onChange = nullptr;
    [_owner.window endSheet:self.window];
    _keep = nil;
}

- (void)modelChanged {
    if (!_model->cloned.empty()) {  // done: the sheet goes, once this call returned, and the repository opens
        NSString* path = NS(_model->cloned);
        dispatch_async(dispatch_get_main_queue(), ^{
            MGRepoWindowController* owner = self->_owner;
            [self finish];
            [App() openRepository:path from:owner];
        });
        return;
    }
    if (_listed != _model->listed) {
        _listed = _model->listed;
        [self fill];
    }
    [self update];
}

// The accounts and their repositories that match the words typed.
- (void)fill {
    _filling = YES;
    _rows.clear();
    _note = nil;
    const CloneModel& m = *_model;
    if (IsGitAddress(m.text)) _note = @"Mangrove clones this address.";
    else if (m.sources.empty())
        _note = @"Connect your GitHub, GitLab or other accounts in Settings › Integrations to pick your repositories here, or paste an address above.";
    if (_note) _rows.push_back({ -1, -1 });
    for (size_t s = 0; s < m.sources.size() && !IsGitAddress(m.text); ++s) {
        _rows.push_back({ (int)s, -1 });
        for (size_t i = 0; i < m.sources[s].repos.size(); ++i)
            if (m.Matches(m.sources[s].repos[i])) _rows.push_back({ (int)s, (int)i });
    }
    [_table reloadData];
    NSInteger picked = -1;
    for (size_t k = 0; k < _rows.size(); ++k)
        if (_rows[k].second >= 0 && !m.picked.empty() && m.sources[(size_t)_rows[k].first].repos[(size_t)_rows[k].second].url == m.picked) picked = (NSInteger)k;
    if (picked >= 0) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)picked] byExtendingSelection:NO];
    else [_table deselectAll:nil];
    _filling = NO;
}

// The buttons, and what the clone does or did.
- (void)update {
    const CloneModel& m = *_model;
    _clone.enabled = !m.running && m.Check().empty();
    _clone.title = m.running ? @"Cloning…" : @"Clone";
    for (NSControl* c in @[ _field, _folder, _choose, _cancel, _table ]) c.enabled = !m.running;
    _spinner.hidden = !m.running;
    if (m.running) [_spinner startAnimation:nil];
    else [_spinner stopAnimation:nil];
    const std::wstring target = m.Target();
    std::wstring text;
    if (m.running) text = m.progress.empty() ? L"Cloning into " + target + L"…" : m.progress;
    else if (!m.error.empty()) text = m.error;
    else if (!target.empty()) text = L"Clones into " + target;
    _status.stringValue = NS(text);
    _status.textColor = !m.running && !m.error.empty() ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

// ---- actions

- (void)start:(id)sender {
    if (!_model->Start()) [self update];  // why it can't
}

- (void)cancel:(id)sender {
    if (!_model->running) [self finish];
}

- (void)choose:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES;
    panel.prompt = @"Choose";
    panel.message = @"Choose the folder the clone goes in.";
    if (!_model->folder.empty()) panel.directoryURL = [NSURL fileURLWithPath:NS(_model->folder) isDirectory:YES];
    __weak NSOpenPanel* weakPanel = panel;
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse r) {
                      if (r != NSModalResponseOK || !weakPanel.URL) return;
                      self->_folder.stringValue = weakPanel.URL.path;
                      self->_model->folder = WS(weakPanel.URL.path);
                      self->_model->error.clear();
                      [self update];
                  }];
}

- (void)doubleClicked:(id)sender {
    const NSInteger row = _table.clickedRow;
    if (row < 0 || row >= (NSInteger)_rows.size() || _rows[(size_t)row].second < 0) return;
    [self pick:row];
    [self start:nil];
}

- (void)pick:(NSInteger)row {
    const auto [s, i] = _rows[(size_t)row];
    _model->picked = _model->sources[(size_t)s].repos[(size_t)i].url;
    _model->error.clear();
    [self update];
}

// ---- the fields

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _field) {
        _model->text = WS(_field.stringValue);
        _model->picked.clear();  // the list changed under it
        _model->error.clear();
        [self fill];
    } else if (note.object == _folder) {
        _model->folder = TrimW(WS(_folder.stringValue));
        _model->error.clear();
    }
    [self update];
}

// The arrows in the address field go through the repositories of the list.
- (BOOL)control:(NSControl*)control textView:(NSTextView*)tv doCommandBySelector:(SEL)cmd {
    if (control != _field || (cmd != @selector(moveDown:) && cmd != @selector(moveUp:))) return NO;
    const int dir = cmd == @selector(moveDown:) ? 1 : -1;
    for (NSInteger r = _table.selectedRow + dir; r >= 0 && r < (NSInteger)_rows.size(); r += dir) {
        if (_rows[(size_t)r].second < 0) continue;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
        [_table scrollRowToVisible:r];
        break;
    }
    return YES;
}

// ---- the table

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)_rows.size();
}

- (BOOL)tableView:(NSTableView*)tv isGroupRow:(NSInteger)row {
    return _rows[(size_t)row].first >= 0 && _rows[(size_t)row].second < 0;
}

- (CGFloat)tableView:(NSTableView*)tv heightOfRow:(NSInteger)row {
    return _rows[(size_t)row].first < 0 ? 48 : _rows[(size_t)row].second < 0 ? 26 : 40;
}

- (BOOL)tableView:(NSTableView*)tv shouldSelectRow:(NSInteger)row {
    return _rows[(size_t)row].second >= 0 && !_model->running;
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    const auto [s, i] = _rows[(size_t)row];
    if (s < 0) {  // a note in place of the list
        NSTextField* t = [NSTextField wrappingLabelWithString:_note ? _note : @""];
        t.textColor = NSColor.secondaryLabelColor;
        return t;
    }
    const CloneModel::Source& src = _model->sources[(size_t)s];
    if (i < 0) {  // the account
        NSTableCellView* v = [tv makeViewWithIdentifier:@"account" owner:self];
        if (!v) {
            v = [[NSTableCellView alloc] init];
            v.identifier = @"account";
            NSTextField* who = MakeLabel(@"", [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold], NSColor.secondaryLabelColor);
            NSTextField* state = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
            state.tag = 1;
            for (NSView* x in @[ who, state ]) {
                x.translatesAutoresizingMaskIntoConstraints = NO;
                [v addSubview:x];
            }
            v.textField = who;
            [who setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
            [NSLayoutConstraint activateConstraints:@[
                [who.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:4],
                [who.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
                [state.leadingAnchor constraintGreaterThanOrEqualToAnchor:who.trailingAnchor constant:8],
                [state.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-4],
                [state.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
            ]];
        }
        v.textField.stringValue = NS(std::wstring(InfoOf(src.account.forge).name) + L" · " + src.account.login + L" · " + src.account.Host());
        NSTextField* state = [v viewWithTag:1];
        state.stringValue = NS(src.loading ? std::wstring(L"Loading…") : !src.error.empty() ? src.error : Plural(src.repos.size(), L"repository", L"repositories"));
        state.textColor = src.error.empty() ? NSColor.secondaryLabelColor : NSColor.systemRedColor;
        state.toolTip = src.error.empty() ? nil : NS(src.error);
        return v;
    }
    const RemoteRepository& r = src.repos[(size_t)i];
    MGCloneCell* v = [tv makeViewWithIdentifier:@"repo" owner:self];
    if (!v) {
        v = [[MGCloneCell alloc] initWithFrame:NSZeroRect];
        v.identifier = @"repo";
    }
    v.textField.stringValue = NS(r.name);
    v.detail.stringValue = NS(r.description.empty() ? r.path : r.path + L" · " + r.description);
    v.when.stringValue = r.updated ? NS(FormatRelative(r.updated)) : @"";
    v.lock.hidden = !r.isPrivate;
    v.toolTip = NS(r.url);
    return v;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    const NSInteger row = _table.selectedRow;
    if (_filling || row < 0 || row >= (NSInteger)_rows.size() || _rows[(size_t)row].second < 0) return;
    [self pick:row];
}

@end
