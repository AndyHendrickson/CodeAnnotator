// main.mm — Code Annotator macOS port: Cocoa GUI and entry point.
//
// Reuses the platform-independent core from ../cpp (annotations, anchors,
// tokenizer, --check CLI); only this file is macOS-specific. Cocoa + the
// C++17 standard library; no external dependencies.
//
// Usage:  code_annotator [folder] [-a ANN_ROOT]
//		   code_annotator [folder] --check [--fix]		  (no GUI)
//
// Shortcuts follow macOS conventions: Cmd+F find, Cmd+G / Shift+Cmd+G next
// and previous match, Cmd+L go to line, Cmd+C copy, Cmd+Z restore deleted
// annotation, Cmd+= / Cmd+- / Cmd+0 zoom, Option+Down / Option+Up next and
// previous annotation. Like the Windows port, the in-app source editor and
// "insert note as comment" are not included; sources are never modified.

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core.hpp"
#include "highlight.hpp"
#include "json.hpp"

namespace fs = std::filesystem;
using core::Annotation;

// ---------------------------------------------------------------- constants

static NSColor* Col(int r, int g, int b)
{
	return [NSColor colorWithCalibratedRed:r / 255.0 green:g / 255.0
									  blue:b / 255.0 alpha:1.0];
}

#define BG			Col(0x1e, 0x1e, 0x1e)
#define BG_PANEL	Col(0x25, 0x25, 0x26)
#define BG_ACTIVE	Col(0x37, 0x37, 0x3d)
#define FG			Col(0xd4, 0xd4, 0xd4)
#define FG_DIM		Col(0x85, 0x85, 0x85)
#define GUTTER_FG	Col(0x6e, 0x76, 0x81)
#define STATUS_BG	Col(0x00, 0x7a, 0xcc)

static NSColor* TokColor(hl::Tok t)
{
	switch (t)
	{
		case hl::Tok::Com:	return Col(0x6a, 0x99, 0x55);
		case hl::Tok::Str:	return Col(0xce, 0x91, 0x78);
		case hl::Tok::Num:	return Col(0xb5, 0xce, 0xa8);
		case hl::Tok::Kw:	return Col(0xc5, 0x86, 0xc0);
		case hl::Tok::Lit:	return Col(0x56, 0x9c, 0xd6);
		case hl::Tok::Typ:	return Col(0x4e, 0xc9, 0xb0);
		case hl::Tok::Fn:	return Col(0xdc, 0xdc, 0xaa);
		case hl::Tok::Tag:	return Col(0x56, 0x9c, 0xd6);
		case hl::Tok::Attr: return Col(0x9c, 0xdc, 0xfe);
		default:			return FG;
	}
}

static NSColor* BlendBG(NSColor* base, int r, int g, int b, double a)
{
	NSColor*	c	= [base colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
	auto		mix	= [a](CGFloat x, int y) { return x + (y / 255.0 - x) * a; };

	return [NSColor colorWithCalibratedRed:mix(c.redComponent, r)
									 green:mix(c.greenComponent, g)
									  blue:mix(c.blueComponent, b)
									 alpha:1.0];
}

static const char* SKIP_DIRS[] =
{
	".git",
	".hg",
	".svn",
	"node_modules",
	"__pycache__",
	".vs",
	".idea",
	"bin",
	"obj",
	"dist",
	"build",
	".next",
	"venv",
	".venv",
	"target",
	".mypy_cache",
	".pytest_cache"};

// ---------------------------------------------------------------- app state

struct Match { int ln, col, len; };

@class CodeView;
@class TreeNode;
@class AppDelegate;

struct State
{
	core::Workspace	ws;
	bool			wsOpen		= false;

	bool			hasFile		= false;
	fs::path		filePath;
	std::vector<std::wstring>			lines;
	std::vector<std::vector<hl::Token>>	toks;
	std::vector<int>					lineCols;
	int				maxCols		= 0;
	std::string		langId		= "plain";
	bool			truncated	= false;
	bool			annError	= false;
	std::vector<Annotation>				anns;
	std::optional<fs::file_time_type>	sidecarTime;
	bool			welcome		= true;
	std::string		viewMsg;			// UTF-8; centered message instead of file

	std::optional<std::pair<int, int>>	sel;
	int				selAnchor = -1;
	bool			dragged = false;
	int				gutterDrag = -1;
	std::optional<std::pair<int, int>>	pending;
	int				annIdx = -1;
	int				flashIdx = -1;
	std::optional<std::pair<fs::path, Annotation>>	deletedAnn;

	std::vector<Match>					matches;
	int				matchIdx = -1;
	std::string		lastQuery;			// UTF-8

	bool			showAnns = true;
	double			opacity = 55.0;
	std::set<std::string>				visColors;
	int				fontPt = 11;
	fs::path		settingsPath;
};

static State S;

// UI singletons
static NSWindow*		gWindow;
static AppDelegate*		gDelegate;
static NSOutlineView*	gTree;
static NSTableView*		gAnnTable;
static CodeView*		gCodeView;
static NSScrollView*	gCodeScroll;
static NSTextField*		gFolderLabel;
static NSTextField*		gStatusFile;
static NSTextField*		gStatusHint;
static NSView*			gFindBar;
static NSView* 			gGotoBar;
static NSTextField*		gFindField;
static NSTextField*		gFindCount;
static NSTextField*		gGotoField;
static NSButton*		gAnnCheckbox;
static NSButton*		gSwatches[core::ANN_COLOR_COUNT];
static NSTimer*			gPollTimer;
static NSTimer*			gFlashTimer;
static NSTimer*			gAnnFlashTimer;
static NSFont*			gCodeFont;
static NSFont*			gNoteFont;

static CGFloat			gCharW			= 8;
static CGFloat			gLineH			= 18;
static const CGFloat	kPad			= 8;
static CGFloat			gGutterW		= 56;
static bool				gFlashActive	= false;
static std::vector<std::pair<NSRect, int>>	gNoteRects;	 // doc coords

// ------------------------------------------------------------------ helpers

static NSString* NSStr(const std::string& u)
{
	return [NSString stringWithUTF8String:u.c_str()] ?: @"";
}
static NSString* NSStrW(const std::wstring& w)
{
	return NSStr(core::wide_to_utf8(w));
}
static std::string U8(NSString* s)
{
	return s ? std::string(s.UTF8String ?: "") : std::string();
}

static int LineCount() { return (int)S.lines.size(); }
static CGFloat ContentH() { return kPad * 2 + LineCount() * gLineH; }
static CGFloat ContentW()
{
	return gGutterW + kPad * 2 + S.maxCols * gCharW + 300;
}

static void MakeFonts()
{
	gCodeFont = [NSFont fontWithName:@"Menlo" size:S.fontPt]
				?: [NSFont userFixedPitchFontOfSize:S.fontPt];

	NSFont* base = [NSFont systemFontOfSize:MAX(7, S.fontPt - 2)];

	gNoteFont = [[NSFontManager sharedFontManager]
		convertFont:base toHaveTrait:NSItalicFontMask] ?: base;

	NSSize sz = [@"0" sizeWithAttributes:@{NSFontAttributeName: gCodeFont}];

	gCharW = sz.width;
	gLineH = ceil(gCodeFont.ascender - gCodeFont.descender + gCodeFont.leading) + 4;
}

static void ComputeMetrics()
{
	int n = LineCount();

	S.maxCols = 0;

	S.lineCols.assign((size_t)n, 0);

	for (int i = 0; i < n; ++i)
	{
		S.lineCols[(size_t)i]	= (int)S.lines[(size_t)i].size();
		S.maxCols				= std::max(S.maxCols, S.lineCols[(size_t)i]);
	}

	int digits = (int)std::to_string(std::max(1, n)).size();

	gGutterW = kPad * 2 + std::max(2, digits) * gCharW;
}

static void ResizeContent()
{
	NSSize avail = gCodeScroll.contentSize;

	[gCodeView setFrameSize:NSMakeSize(std::max(ContentW(), avail.width),
									   std::max(ContentH(), avail.height))];

	[gCodeView setNeedsDisplay:YES];
}

// forward declarations
static void DefaultHint();
static void FlashStatus(const std::string& text, double secs = 4.0);
static void Persist();
static void RefreshOutlinePane();
static void RefreshBadge(const fs::path& p);
static void OpenFile(const fs::path& p);
static void EditAnnotation(Annotation ann, bool isNew, int existingIdx);
static void EscapeAction();

// ------------------------------------------------------------------- status

static void SetHint(const std::string& text)
{
	gStatusHint.stringValue = NSStr(text);
}

static void DefaultHint()
{
	if (gFlashActive) return;

	std::string	hint	=	"Click or drag on line numbers to annotate • "
							"click a highlight to edit • Cmd+F to search";
	size_t		count	= S.anns.size();

	if (count)
	{
		hint	=	std::to_string(count) +
					(count == 1 ? " annotation" : " annotations") +
					"	•	 " +
					hint;
	}

	SetHint(hint);
}

static void FlashStatus(const std::string& text, double secs)
{
	gFlashActive = true;

	SetHint(text);

	[gFlashTimer invalidate];

	gFlashTimer = [NSTimer scheduledTimerWithTimeInterval:secs repeats:NO
		block:^(NSTimer*)
		{
			gFlashActive = false;
			DefaultHint();
		}
	];
}

// --------------------------------------------------------------- navigation

static void ScrollToLine(int ln)
{
	NSRect	vis	= gCodeView.visibleRect;
	CGFloat	y	= kPad + (ln - 1) * gLineH;

	if (!(vis.origin.y + gLineH <= y &&
		y <= vis.origin.y + vis.size.height - 2 * gLineH))
	{
		CGFloat target = std::max(0.0, y - vis.size.height / 3);

		[gCodeView scrollPoint:NSMakePoint(vis.origin.x, target)];
	}

	[gCodeView setNeedsDisplay:YES];
}

static bool LineVisible(int ln)
{
	NSRect	vis	= gCodeView.visibleRect;
	CGFloat	y	= kPad + (ln - 1) * gLineH;

	return	vis.origin.y <= y &&
			y <= vis.origin.y + vis.size.height - gLineH;
}

static void ShowAnnotationAt(int idx)
{
	if (!S.hasFile || S.anns.empty()) return;

	idx			= std::max(0, std::min(idx, (int)S.anns.size() - 1));
	S.annIdx	= idx;

	const Annotation& a = S.anns[(size_t)idx];

	ScrollToLine(a.start_line);

	S.flashIdx = idx;

	[gAnnFlashTimer invalidate];

	gAnnFlashTimer = [NSTimer scheduledTimerWithTimeInterval:0.7 repeats:NO
													   block:^(NSTimer*)
		{
			S.flashIdx = -1;
			[gCodeView setNeedsDisplay:YES];
		}
	];

	std::string note = a.text;

	for (auto& c : note)
		if (c == '\n') c = ' ';

	NSString* ns = NSStr(note);

	if (ns.length > 60) ns = [[ns substringToIndex:57]
						   stringByAppendingString:@"..."];

	std::string text =	"Annotation " +
						std::to_string(idx + 1) +
						" of " +
						std::to_string(S.anns.size());

	if (ns.length) text += "  •  " + U8(ns);

	SetHint(text);

	[gCodeView setNeedsDisplay:YES];
}

static void GotoAnnotation(int step)
{
	if (!S.hasFile || S.anns.empty()) return;

	int	n	= (int)S.anns.size();
	int	idx	= S.annIdx;

	if (idx >= 0 && idx < n && LineVisible(S.anns[(size_t)idx].start_line))
	{
		idx = ((idx + step) % n + n) % n;
	}
	else
	{
		NSRect	vis		= gCodeView.visibleRect;
		int		top		= (int)((vis.origin.y - kPad) / gLineH) + 1;
		int		bottom	= top + (int)(vis.size.height / gLineH);

		if (step > 0)
		{
			idx = 0;

			for (int i = 0; i < n; ++i)
			{
				if (S.anns[(size_t)i].start_line > top)
				{
					idx = i;

					break;
				}
			}
		}
		else
		{
			idx = n - 1;

			for (int i = n - 1; i >= 0; --i)
			{
				if (S.anns[(size_t)i].start_line < bottom)
				{
					idx = i;

					break;
				}
			}
		}
	}

	ShowAnnotationAt(idx);
}

// --------------------------------------------------------------- find / goto

static void PositionBars()
{
	// bars live in the content view; the scroll view lives in the split view
	NSRect	r	= [gWindow.contentView convertRect:gCodeScroll.bounds
										  fromView:gCodeScroll];
	CGFloat	fw	= 320;
	CGFloat	fh	= 34;

	gFindBar.frame = NSMakeRect(NSMaxX(r) - fw - 24,
								NSMaxY(r) - fh - 8, fw, fh);

	gGotoBar.frame = NSMakeRect(NSMaxX(r) - 170 - 24,
								NSMaxY(r) - fh - 8, 170, fh);
}

static void GotoMatch(int idx)
{
	if (S.matches.empty()) return;

	int n = (int)S.matches.size();

	S.matchIdx = ((idx % n) + n) % n;

	const Match& m = S.matches[(size_t)S.matchIdx];

	ScrollToLine(m.ln);

	NSRect	vis	= gCodeView.visibleRect;
	CGFloat	x0	= gGutterW + kPad + m.col * gCharW;

	if (!(vis.origin.x + gGutterW <= x0 && x0 <= vis.origin.x + vis.size.width - 80))
	{
		CGFloat tx = std::max(0.0, x0 - vis.size.width / 3);

		[gCodeView scrollPoint:NSMakePoint(tx, gCodeView.visibleRect.origin.y)];
	}

	gFindCount.stringValue = [NSString stringWithFormat:@"%d of %d", S.matchIdx + 1, n];

	[gCodeView setNeedsDisplay:YES];
}

static void FindRefresh()
{
	std::string query = U8(gFindField.stringValue);

	S.lastQuery = query;

	S.matches.clear();

	S.matchIdx = -1;

	std::wstring wq = core::utf8_to_wide(query);

	std::transform(wq.begin(), wq.end(), wq.begin(), towlower);

	if (!wq.empty() && S.hasFile)
	{
		for (int row = 0; row < LineCount() && S.matches.size() < 2000; ++row)
		{
			std::wstring low = S.lines[(size_t)row];

			std::transform(low.begin(), low.end(), low.begin(), towlower);

			size_t start = 0;

			while (S.matches.size() < 2000)
			{
				size_t col = low.find(wq, start);

				if (col == std::wstring::npos) break;

				S.matches.push_back({row + 1, (int)col, (int)wq.size()});

				start = col + wq.size();
			}
		}
	}

	if (!S.matches.empty())
	{
		GotoMatch(0);
	}
	else
	{
		gFindCount.stringValue = wq.empty() ? @"" : @"no results";

		[gCodeView setNeedsDisplay:YES];
	}
}

static void FindNext() { if (!S.matches.empty()) GotoMatch(S.matchIdx + 1); }
static void FindPrev() { if (!S.matches.empty()) GotoMatch(S.matchIdx - 1); }

static void OpenFindBar()
{
	if (!S.hasFile) return;

	gGotoBar.hidden = YES;

	PositionBars();

	gFindBar.hidden = NO;

	[gWindow makeFirstResponder:gFindField];

	[gFindField selectText:nil];

	FindRefresh();
}

static void OpenGotoBar()
{
	if (!S.hasFile) return;

	gFindBar.hidden = YES;

	PositionBars();

	gGotoBar.hidden			= NO;
	gGotoField.stringValue	= @"";

	[gWindow makeFirstResponder:gGotoField];
}

static void EscapeAction()
{
	if (gFindBar.hidden && gGotoBar.hidden && !S.sel && S.matches.empty())
		return;

	gFindBar.hidden = YES;
	gGotoBar.hidden = YES;

	S.matches.clear();

	S.matchIdx = -1;

	S.lastQuery.clear();
	S.sel.reset();

	DefaultHint();

	[gCodeView setNeedsDisplay:YES];

	[gWindow makeFirstResponder:gCodeView];
}

static void GotoLineAction()
{
	int ln = gGotoField.intValue;

	if (!S.hasFile || ln <= 0) return;

	ln				= std::max(1, std::min(LineCount(), ln));
	gGotoBar.hidden	= YES;

	ScrollToLine(ln);

	S.sel = {ln, ln};

	SetHint("1 line selected  •  Cmd+C to copy");

	[gWindow makeFirstResponder:gCodeView];

	[gCodeView setNeedsDisplay:YES];
}

// ------------------------------------------------------------------- zoom

static void Zoom(int delta)
{
	int pt = delta == 0 ? 11 : std::max(6, std::min(28, S.fontPt + delta));

	if (pt == S.fontPt) return;

	NSRect	vis		= gCodeView.visibleRect;
	double	frac	= ContentH() > 0 ? vis.origin.y / ContentH() : 0;

	S.fontPt = pt;

	MakeFonts();

	if (S.hasFile)
	{
		ComputeMetrics();

		ResizeContent();

		[gCodeView scrollPoint:NSMakePoint(0, frac * ContentH())];
	}

	[gCodeView setNeedsDisplay:YES];
}

// ---------------------------------------------------------------- clipboard

static void CopySelection()
{
	if (!S.hasFile || !S.sel) return;

	auto			[lo, hi]	= *S.sel;
	std::wstring	text;

	for (int i = lo; i <= hi && i <= LineCount(); ++i)
	{
		if (i > lo) text += L"\n";

		text += S.lines[(size_t)i - 1];
	}

	NSPasteboard* pb = NSPasteboard.generalPasteboard;

	[pb clearContents];
	[pb setString:NSStrW(text) forType:NSPasteboardTypeString];

	int n = hi - lo + 1;

	FlashStatus("Copied " + std::to_string(n) +
				(n == 1 ? " line" : " lines") +
				" to clipboard");
}

// -------------------------------------------------------------- persistence

static void RefreshOutlinePane()
{
	[gAnnTable reloadData];
}

static void Persist()
{
	if (S.annError)
	{
		NSAlert* a = [NSAlert new];

		a.messageText		= @"Annotations locked";
		a.informativeText 	= [NSString stringWithFormat:
			@"This file's annotation file exists but could not be parsed:\n"
			@"%@\n\nSaving is disabled so its contents are not overwritten. "
			@"Fix or delete it, then reopen the file.",
				NSStr(S.ws.sidecar_for(S.filePath).string())
		];

		[a runModal];

		return;
	}

	if (!S.ws.save(S.filePath, S.anns))
	{
		NSAlert* a = [NSAlert new];

		a.messageText		= @"Save failed";
		a.informativeText	= @"Could not write the annotation file.";

		[a runModal];

		return;
	}

	RefreshBadge(S.filePath);
	RefreshOutlinePane();

	std::error_code ec;

	auto t = fs::last_write_time(S.ws.sidecar_for(S.filePath), ec);

	S.sidecarTime = ec ? std::nullopt : std::optional(t);
}

static void UndoDelete()
{
	if (!S.deletedAnn) return;

	auto& [path, ann] = *S.deletedAnn;

	if (path != S.filePath || S.annError) return;

	S.anns.push_back(ann);

	core::sort_annotations(S.anns);

	S.deletedAnn.reset();

	Persist();

	[gCodeView setNeedsDisplay:YES];

	FlashStatus("Annotation restored");
}

static void PollSidecar()
{
	if (!S.hasFile || S.annError) return;

	fs::path		sidecar = S.ws.sidecar_for(S.filePath);
	std::error_code	ec;
	auto			t = fs::last_write_time(sidecar, ec);
	std::optional<fs::file_time_type> mtime = ec ? std::nullopt : std::optional(t);

	if (mtime == S.sidecarTime) return;

	S.sidecarTime = mtime;

	core::AnnLoad la = core::load_annotations(sidecar);

	if (!la.ok)
	{
		S.annError = true;

		S.anns.clear();

		RefreshOutlinePane();

		[gCodeView setNeedsDisplay:YES];

		FlashStatus("Annotation file became unreadable — annotating disabled",
					8.0);

		return;
	}

	S.anns = std::move(la.anns);

	core::reconcile_annotations(S.anns, S.lines);	// heal in memory only

	S.annIdx = -1;

	RefreshOutlinePane();
	RefreshBadge(S.filePath);

	[gCodeView setNeedsDisplay:YES];

	FlashStatus("Annotations reloaded (changed outside the app)");
}

// -------------------------------------------------------------- folder tree

@interface TreeNode : NSObject
@property(strong) NSString* path;
@property(strong) NSString* name;
@property BOOL isDir;
@property BOOL loaded;
@property BOOL badge;
@property(strong) NSMutableArray<TreeNode*>* kids;
@end
@implementation TreeNode
@end

static TreeNode* gRoot;

static void LoadChildren(TreeNode* node)
{
	node.loaded	= YES;
	node.kids	= [NSMutableArray array];

	std::error_code			ec;
	fs::path				dir = U8(node.path);
	std::vector<fs::path>	dirs;
	std::vector<fs::path>	files;
	fs::directory_iterator	it(dir, ec);
	fs::directory_iterator	end;

	if (ec) return;

	for (; it != end; it.increment(ec))
	{
		if (ec) break;

		std::error_code ec2;

		if (it->is_directory(ec2))
			dirs.push_back(it->path());
		else
			files.push_back(it->path());
	}

	auto by_name =
		[](const fs::path& a, const fs::path& b)
		{
			std::string x = a.filename().string(), y = b.filename().string();

			std::transform(x.begin(), x.end(), x.begin(), ::tolower);
			std::transform(y.begin(), y.end(), y.begin(), ::tolower);

			return x < y;
		};

	std::sort(dirs.begin(), dirs.end(), by_name);
	std::sort(files.begin(), files.end(), by_name);

	fs::path ann_abs = fs::absolute(S.ws.ann_root, ec).lexically_normal();

	for (const auto& d : dirs)
	{
		std::string	name	= d.filename().string();
		bool		skip	= false;

		for (const char* sd : SKIP_DIRS)
		{
			if (name == sd)
			{
				skip = true;

				break;
			}
		}

		if (skip) continue;

		if (fs::absolute(d, ec).lexically_normal() == ann_abs)
			continue;

		TreeNode* child = [TreeNode new];

		child.path	= NSStr(d.string());
		child.name	= NSStr(name);
		child.isDir	= YES;

		[node.kids addObject:child];
	}

	for (const auto& f : files)
	{
		TreeNode* child = [TreeNode new];

		child.path	= NSStr(fs::absolute(f, ec).lexically_normal().string());
		child.name	= NSStr(f.filename().string());
		child.isDir	= NO;
		child.badge	= fs::is_regular_file(S.ws.sidecar_for(f), ec);

		[node.kids addObject:child];
	}
}

static TreeNode* FindNode(TreeNode* node, NSString* path)
{
	if (!node) return nil;

	if (!node.isDir && [node.path isEqualToString:path])
		return node;

	for (TreeNode* k in node.kids)
	{
		TreeNode* hit = FindNode(k, path);

		if (hit) return hit;
	}

	return nil;
}

static void RefreshBadge(const fs::path& p)
{
	std::error_code	ec;
	NSString*		abs		= NSStr(fs::absolute(p, ec).lexically_normal().string());
	TreeNode*		node	= FindNode(gRoot, abs);

	if (!node) return;

	node.badge = fs::is_regular_file(S.ws.sidecar_for(p), ec);

	[gTree reloadItem:node];
}

static void OpenFolder(const fs::path& folder)
{
	S.ws.open(folder);

	S.wsOpen					= true;

	gFolderLabel.stringValue	= [NSString stringWithFormat:
		@"%@   •	 annotations: %@", NSStr(S.ws.folder.string()),
			NSStr(S.ws.ann_root.string())];

	gRoot						= [TreeNode new];
	gRoot.path					= NSStr(S.ws.folder.string());

	std::string name = S.ws.folder.filename().string();

	gRoot.name	= NSStr(name.empty() ? S.ws.folder.string() : name);
	gRoot.isDir	= YES;

	[gTree reloadData];

	[gTree expandItem:gRoot];
}

// -------------------------------------------------------------- file view

static void ShowMessageView(const std::string& msg)
{
	S.hasFile = false;

	S.filePath.clear();
	S.lines.clear();
	S.toks.clear();
	S.anns.clear();
	S.matches.clear();

	S.matchIdx	= -1;
	S.annIdx	= -1;
	S.flashIdx	= -1;

	S.sel.reset();
	S.pending.reset();

	S.annError	= false;

	S.sidecarTime.reset();

	S.viewMsg				= msg;
	S.welcome				= false;
	gStatusFile.stringValue	= @"Ready";

	RefreshOutlinePane();

	[gCodeView setNeedsDisplay:YES];
}

static void OpenFile(const fs::path& path)
{
	std::error_code	ec;
	uintmax_t		size	= fs::file_size(path, ec);
	std::string		name	= path.filename().string();

	if (!ec && size > core::MAX_FILE_BYTES)
	{
		char buf[64];

		snprintf(buf, sizeof buf, " is too large to display (%.1f MB).", size / 1048576.0);

		ShowMessageView(name + buf);

		return;
	}

	auto bytes = core::read_file_bytes(path);

	if (!bytes)
	{
		ShowMessageView("Could not open file: " + name);

		return;
	}

	size_t head = std::min<size_t>(bytes->size(), 8192);

	if (bytes->find('\0') < head)
	{
		ShowMessageView(name + " looks like a binary file.");

		return;
	}

	std::vector<std::wstring>	lines		= core::decode_source(*bytes);
	bool						truncated	= (int)lines.size() > core::MAX_LINES;

	if (truncated) lines.resize(core::MAX_LINES);

	S.truncated	= truncated;
	S.hasFile	= true;
	S.welcome	= false;

	S.viewMsg.clear();

	S.filePath	= fs::absolute(path, ec).lexically_normal();
	S.lines		= std::move(lines);

	fs::path		sidecar		= S.ws.sidecar_for(S.filePath);
	int				repaired	= 0;
	int				lost		= 0;
	core::AnnLoad	la			= core::load_annotations(sidecar);

	if (la.ok)
	{
		S.anns		= std::move(la.anns);
		S.annError	= false;

		if (!truncated)
		{
			auto [r, l] = core::reconcile_annotations(S.anns, S.lines);

			repaired	= r;
			lost		= l;

			if (repaired || lost) Persist();
		}
	}
	else
	{
		S.anns.clear();

		S.annError = true;
	}

	auto t = fs::last_write_time(sidecar, ec);

	S.sidecarTime	= ec ? std::nullopt : std::optional(t);
	S.annIdx		= -1;
	S.flashIdx		= -1;

	S.sel.reset();
	S.pending.reset();

	S.selAnchor		= -1;
	S.gutterDrag	= -1;

	S.deletedAnn.reset();

	S.langId		= hl::lang_for(core::utf8_to_wide(name));
	S.toks			= hl::highlight_lines(S.lines, S.langId);

	ComputeMetrics();
	ResizeContent();

	[gCodeView scrollPoint:NSMakePoint(0, 0)];

	if (!gFindBar.hidden)
		FindRefresh();
	else
	{
		S.matches.clear();

		S.matchIdx = -1;
	}

	RefreshOutlinePane();

	std::string status = name;

	if (S.langId != "plain")
	{
		std::string lang = S.langId;

		std::transform(lang.begin(), lang.end(), lang.begin(), ::toupper);

		status += "	 •  " + lang;
	}

	if (truncated)
	{
		status +=	"	 •  showing first " +
					std::to_string(core::MAX_LINES) +
					" lines";
	}

	gStatusFile.stringValue = NSStr(status);

	if (S.annError)
	{
		FlashStatus("Annotation file is unreadable — annotating is disabled "
					"for this file until it is fixed or deleted",
					8.0);
	}
	else if (repaired || lost)
	{
		std::string parts;

		if (repaired)
			parts = std::to_string(repaired) + " annotation(s) re-anchored to moved code";

		if (lost)
		{
			if (!parts.empty()) parts += " • ";

			parts += std::to_string(lost) + " annotation(s) orphaned — their anchored code is gone";
		}

		FlashStatus(parts, 6.0);
	}
	else
	{
		DefaultHint();
	}

	[gCodeView setNeedsDisplay:YES];
}

// --------------------------------------------------------- annotation dialog

static NSImage* SwatchImage(int idx, bool filled)
{
	const auto&	c		= core::ANN_COLORS[idx];
	NSColor*	color	= Col(c.r, c.g, c.b);

	return [NSImage imageWithSize:NSMakeSize(16, 16) flipped:NO
				   drawingHandler:^BOOL(NSRect r)
		{
			NSBezierPath* p = [NSBezierPath
				bezierPathWithOvalInRect:NSInsetRect(r, filled ? 2 : 4, filled ? 2 : 4)];

			if (filled)
			{
				[color setFill];
				[p fill];
			}
			else
			{
				[color setStroke];
				p.lineWidth = 2;
				[p stroke];
			}

			return YES;
		}
	];
}

// returns 0 cancel, 1 save, 2 delete; on save text/color are updated
static int RunAnnotationDialog(Annotation& ann, bool isNew)
{
	NSAlert*	alert	= [NSAlert new];
	std::string	rng		=
		ann.start_line == ann.end_line ?
			"line " + std::to_string(ann.start_line) :
			"lines " + std::to_string(ann.start_line) + "–" + std::to_string(ann.end_line);

	alert.messageText = NSStr((isNew ? "New annotation — " : "Edit annotation — ") + rng);

	NSView*			box	= [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 130)];
	NSScrollView*	sv	= [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 34, 320, 96)];

	sv.hasVerticalScroller	= YES;
	sv.borderType			= NSBezelBorder;

	NSTextView* tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 320, 96)];

	tv.autoresizingMask	= NSViewWidthSizable;
	tv.font				= [NSFont systemFontOfSize:13];
	tv.richText			= NO;
	tv.string			= NSStr(ann.text);
	sv.documentView		= tv;

	[box addSubview:sv];

	NSSegmentedControl* seg = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(0, 0, 200, 26)];

	seg.segmentCount	= core::ANN_COLOR_COUNT;
	seg.trackingMode	= NSSegmentSwitchTrackingSelectOne;

	for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
	{
		[seg setImage:SwatchImage(i, true) forSegment:i];

		[seg setWidth:30 forSegment:i];

		if (ann.color == core::ANN_COLORS[i].name)
			seg.selectedSegment = i;
	}

	if (seg.selectedSegment < 0) seg.selectedSegment = 0;

	[box addSubview:seg];

	alert.accessoryView = box;

	[alert addButtonWithTitle:@"Save"];
	[alert addButtonWithTitle:@"Cancel"];
	if (!isNew) [alert addButtonWithTitle:@"Delete"];

	alert.window.initialFirstResponder = tv;

	NSModalResponse resp = [alert runModal];

	if (resp == NSAlertFirstButtonReturn)
	{
		NSString* text = [tv.string stringByTrimmingCharactersInSet:
			NSCharacterSet.whitespaceAndNewlineCharacterSet];

		ann.text = U8(text);

		long si = seg.selectedSegment;

		if (si >= 0 && si < core::ANN_COLOR_COUNT)
			ann.color = core::ANN_COLORS[si].name;

		return 1;
	}

	if (resp == NSAlertThirdButtonReturn) return 2;

	return 0;
}

static void EditAnnotation(Annotation ann, bool isNew, int existingIdx)
{
	int result = RunAnnotationDialog(ann, isNew);

	S.pending.reset();

	if (result == 1)
	{
		ann.orphaned = false;

		if (ann.start_line >= 1 && ann.start_line <= LineCount())
			ann.anchor = core::anchor_text(S.lines[(size_t)ann.start_line - 1]);

		if (isNew)
		{
			S.anns.push_back(std::move(ann));
		}
		else if (existingIdx >= 0 && existingIdx < (int)S.anns.size())
		{
			S.anns[(size_t)existingIdx] = std::move(ann);
		}

		core::sort_annotations(S.anns);

		S.annIdx = -1;

		Persist();

		DefaultHint();
	}
	else if (result == 2 && !isNew)
	{
		std::string id = ann.id;

		S.anns.erase(
			std::remove_if(
				S.anns.begin(),
				S.anns.end(),
				[&](const Annotation& a)
				{
					return a.id == id;
				}
			),
			S.anns.end()
		);

		S.annIdx		= -1;
		S.deletedAnn	= {S.filePath, ann};

		Persist();

		FlashStatus("Annotation deleted — Cmd+Z restores", 6.0);
	}
	else
	{
		DefaultHint();
	}

	[gCodeView setNeedsDisplay:YES];

	[gWindow makeFirstResponder:gCodeView];
}

// ---------------------------------------------------------------- code view

@interface CodeView : NSView
@end

@implementation CodeView

- (BOOL)isFlipped				{ return YES; }
- (BOOL)acceptsFirstResponder	{ return YES; }

- (void)drawWelcome:(NSRect)vis
{
	CGFloat						cx		= vis.origin.x + std::max(360.0, vis.size.width / 2);
	NSMutableParagraphStyle*	center	= [NSMutableParagraphStyle new];

	center.alignment = NSTextAlignmentCenter;

	NSDictionary* titleAttrs = @{
		NSFontAttributeName:			[NSFont systemFontOfSize:20],
		NSForegroundColorAttributeName:	FG,
		NSParagraphStyleAttributeName:	center};

	[@"Code Annotator" drawWithRect:NSMakeRect(cx - 400, 90, 800, 40)
							options:NSStringDrawingUsesLineFragmentOrigin
						 attributes:titleAttrs];

	NSString* body =
		@"Open a project folder, pick a code file, and see its\n"
		@"annotations overlaid transparently on the source.\n\n"
		@"Click or drag on the line numbers to add an annotation.\n"
		@"Click an existing highlight to edit it.\n\n"
		@"Cmd+F finds text, Cmd+L jumps to a line, Cmd+C copies\n"
		@"selected lines, Cmd+= / Cmd+- zoom the code,\n"
		@"Option+Down/Option+Up jump between annotations.\n\n"
		@"Notes are saved in a parallel tree that mirrors the\n"
		@"source layout (default: <project>/.annotations),\n"
		@"so the source files themselves are never touched.";

	NSDictionary* bodyAttrs = @{
		NSFontAttributeName:			[NSFont systemFontOfSize:12],
		NSForegroundColorAttributeName:	FG_DIM,
		NSParagraphStyleAttributeName:	center};

	[body drawWithRect:NSMakeRect(cx - 400, 160, 800, 600)
			   options:NSStringDrawingUsesLineFragmentOrigin
			attributes:bodyAttrs];
}

- (void)drawRect:(NSRect)dirty
{
	[BG setFill];

	NSRectFill(dirty);

	NSRect vis = self.visibleRect;

	gNoteRects.clear();

	if (!S.hasFile)
	{
		if (S.welcome)
		{
			[self drawWelcome:vis];
		}
		else if (!S.viewMsg.empty())
		{
			NSMutableParagraphStyle* center = [NSMutableParagraphStyle new];

			center.alignment = NSTextAlignmentCenter;

			CGFloat cx = vis.origin.x + std::max(360.0, vis.size.width / 2);

			[NSStr(S.viewMsg)
				drawWithRect:NSMakeRect(cx - 400, 110, 800, 40)
					 options:NSStringDrawingUsesLineFragmentOrigin
				  attributes:@{
					  NSFontAttributeName: [NSFont systemFontOfSize:12],
					  NSForegroundColorAttributeName: FG_DIM,
					  NSParagraphStyleAttributeName: center}];
		}

		return;
	}

	int		n		= LineCount();
	CGFloat	lh		= gLineH;
	int		r0		= std::max(0, (int)((vis.origin.y - kPad) / lh));
	int		r1		=	std::min(
						n - 1,
						(int)((vis.origin.y + vis.size.height - kPad) / lh) + 1);
	CGFloat	gx		= vis.origin.x;				// pinned gutter left edge
	double	alpha	= S.opacity / 100.0;
	auto	lineTop	= [&](int ln) { return kPad + (ln - 1) * lh; };
	CGFloat	bandL	= gGutterW;
	CGFloat	bandR	= std::max(ContentW(), vis.origin.x + vis.size.width);

	[NSGraphicsContext saveGraphicsState];

	NSRectClip(
		NSMakeRect(
			gx + gGutterW,
			vis.origin.y,
			vis.size.width - gGutterW,
			vis.size.height
		)
	);

	if (S.sel)
	{
		[BlendBG(BG, 130, 160, 190, 0.16) setFill];

		NSRectFill(
			NSMakeRect(
				bandL,
				lineTop(S.sel->first),
				bandR - bandL,
				(S.sel->second - S.sel->first + 1) * lh
			)
		);
	}

	if (S.pending)
	{
		[BlendBG(BG, 14, 99, 156, 0.35) setFill];

		NSRectFill(
			NSMakeRect(
				bandL,
				lineTop(S.pending->first),
				bandR - bandL,
				(S.pending->second - S.pending->first + 1) * lh
			)
		);
	}

	if (S.showAnns)
	{
		for (const auto& a : S.anns)
		{
			if (!S.visColors.count(a.color)) continue;

			const core::AnnColor* c = core::ann_color(a.color);

			if (!c) c = &core::ANN_COLORS[0];

			CGFloat y0 = lineTop(a.start_line);
			CGFloat y1 = lineTop(std::min(a.end_line, n) + 1);

			if (y1 < vis.origin.y - lh || y0 > vis.origin.y + vis.size.height + lh)
				continue;

			double f = a.orphaned ? 0.18 : 0.45;

			[BlendBG(BG, c->r, c->g, c->b, alpha * f) setFill];

			NSRectFill(NSMakeRect(bandL, y0, bandR - bandL, y1 - y0));

			NSColor* edge =
				BlendBG(BG, c->r, c->g, c->b, std::min(1.0, alpha * 1.5));

			[edge setFill];

			if (a.orphaned)
			{
				for (CGFloat y = y0 + 1; y < y1 - 1; y += 6)
				{
					NSRectFill(
						NSMakeRect(
							bandL + 1,
							y,
							3,
							std::min(3.0, y1 - 1 - y)
						)
					);
				}
			}
			else
			{
				NSRectFill(NSMakeRect(bandL, y0, 3, y1 - y0));
			}
		}
	}

	for (int i = 0; i < (int)S.matches.size(); ++i)
	{
		const Match&	m	= S.matches[(size_t)i];
		CGFloat			y0	= lineTop(m.ln);

		if (y0 < vis.origin.y - lh || y0 > vis.origin.y + vis.size.height)
			continue;

		CGFloat	x0	= gGutterW + kPad + m.col * gCharW;
		bool	cur	= i == S.matchIdx;
		NSRect	r	= NSMakeRect(x0 - 1, y0 + 1, m.len * gCharW + 2, lh - 2);

		[BlendBG(BG, 210, 170, 60, cur ? 0.5 : 0.25) setFill];

		NSRectFill(r);

		if (cur)
		{
			[Col(0xd7, 0xba, 0x7d) setStroke];

			[NSBezierPath strokeRect:r];
		}
	}

	// code text
	for (int row = r0; row <= r1; ++row)
	{
		CGFloat y = kPad + row * lh + 2;

		for (const auto& t : S.toks[(size_t)row])
		{
			if (t.cls == hl::Tok::None &&
				t.text.find_first_not_of(L" \t") == std::wstring::npos)
			{
				continue;
			}

			NSDictionary* attrs = @{
				NSFontAttributeName:			gCodeFont,
				NSForegroundColorAttributeName:	TokColor(t.cls)};

			[NSStrW(t.text)
				drawAtPoint:NSMakePoint(gGutterW + kPad + t.col * gCharW, y)
			 withAttributes:attrs];
		}
	}

	// notes
	if (S.showAnns)
	{
		NSColor*					noteFill	=
			BlendBG(BG, 255, 255, 255, std::min(1.0, 0.35 + alpha * 0.65));
		CGFloat						nx			= vis.origin.x + vis.size.width - 12;   // pinned right
		NSMutableParagraphStyle*	right		= [NSMutableParagraphStyle new];

		right.alignment = NSTextAlignmentRight;

		NSDictionary* attrs = @{
			NSFontAttributeName:			gNoteFont,
			NSForegroundColorAttributeName:	noteFill,
			NSParagraphStyleAttributeName:	right};

		for (int ai = 0; ai < (int)S.anns.size(); ++ai)
		{
			const Annotation& a = S.anns[(size_t)ai];

			if (!S.visColors.count(a.color) || a.text.empty())
				continue;

			int			lo		= a.start_line;
			auto		avail	=
				[&](int hi)
				{
					hi = std::max(lo, std::min(hi, (int)S.lineCols.size()));

					int cols = 0;

					for (int i = lo - 1; i < hi; ++i)
						cols = std::max(cols, S.lineCols[(size_t)i]);

					return nx - 24 - (gGutterW + kPad + cols * gCharW);
				};
			int			hiCover	= std::min(a.end_line, n);
			CGFloat		width	= std::max(80.0, avail(hiCover));
			NSString*	text	= [NSStr(a.orphaned ? "⚠ orphaned — " : "⚑ ")
				stringByAppendingString:NSStr(a.text)];
			CGFloat		height	= 0;

			for (int iter = 0; iter < 6; ++iter)
			{
				NSRect calc = [text
					boundingRectWithSize:NSMakeSize(width, 100000)
								 options:NSStringDrawingUsesLineFragmentOrigin
							  attributes:attrs];

				height = ceil(calc.size.height);

				int covered =
					(int)((lineTop(lo) + 2 + height - kPad) / lh) + 1;

				if (covered <= hiCover || hiCover >= n) break;

				hiCover = std::min(covered, n);

				CGFloat newW = std::max(80.0, avail(hiCover));

				if (newW >= width) break;

				width = newW;
			}

			CGFloat top = lineTop(lo) + 2;

			if (top + height < vis.origin.y - lh ||
				top > vis.origin.y + vis.size.height + lh)
			{
				continue;
			}

			NSRect r = NSMakeRect(nx - width, top, width, height);

			[text drawWithRect:r
					   options:NSStringDrawingUsesLineFragmentOrigin
					attributes:attrs];

			gNoteRects.push_back({r, ai});
		}
	}

	if (S.flashIdx >= 0 && S.flashIdx < (int)S.anns.size())
	{
		const Annotation&		a	= S.anns[(size_t)S.flashIdx];
		const core::AnnColor*	c	= core::ann_color(a.color);

		if (!c) c = &core::ANN_COLORS[0];

		NSRect r = NSMakeRect(
			bandL + 1,
			lineTop(a.start_line),
			bandR - bandL - 2,
			lineTop(std::min(a.end_line, n) + 1) - lineTop(a.start_line));

		[Col(c->r, c->g, c->b) setStroke];

		NSBezierPath* p = [NSBezierPath bezierPathWithRect:r];

		p.lineWidth = 2;

		[p stroke];
	}

	[NSGraphicsContext restoreGraphicsState];

	// pinned gutter, drawn last so code slides beneath it
	[BG setFill];

	NSRectFill(NSMakeRect(gx, vis.origin.y, gGutterW, vis.size.height));

	NSDictionary* gattrs = @{
		NSFontAttributeName:			gCodeFont,
		NSForegroundColorAttributeName:	GUTTER_FG
	};

	for (int row = r0; row <= r1; ++row)
	{
		NSString*	num	= [NSString stringWithFormat:@"%d", row + 1];
		NSSize		sz	= [num sizeWithAttributes:gattrs];

		[num drawAtPoint:NSMakePoint(gx + gGutterW - kPad - sz.width, kPad + row * gLineH + 2)
		  withAttributes:gattrs];
	}
}

- (int)lineAtY:(CGFloat)y
{
	int ln = (int)((y - kPad) / gLineH) + 1;

	return std::max(1, std::min(LineCount(), ln));
}

- (void)mouseDown:(NSEvent*)ev
{
	[self.window makeFirstResponder:self];

	if (!S.hasFile) return;

	NSPoint	p	= [self convertPoint:ev.locationInWindow fromView:nil];
	NSRect	vis	= self.visibleRect;

	if (p.x - vis.origin.x < gGutterW)
	{
		if (S.annError)
		{
			FlashStatus("Annotating is disabled — this file's annotation "
						"file could not be parsed");

			return;
		}

		S.gutterDrag	= [self lineAtY:p.y];
		S.pending		= {S.gutterDrag, S.gutterDrag};

		[self setNeedsDisplay:YES];
	}
	else
	{
		S.dragged		= false;
		S.selAnchor		= [self lineAtY:p.y];
	}
}

- (void)mouseDragged:(NSEvent*)ev
{
	if (!S.hasFile) return;

	[self autoscroll:ev];

	NSPoint	p		= [self convertPoint:ev.locationInWindow fromView:nil];
	int		line	= [self lineAtY:p.y];

	if (S.gutterDrag >= 0)
	{
		S.pending = {std::min(S.gutterDrag, line),
					 std::max(S.gutterDrag, line)};

		[self setNeedsDisplay:YES];
	}
	else if (S.selAnchor >= 0)
	{
		if (line != S.selAnchor) S.dragged = true;

		if (S.dragged)
		{
			S.sel = {std::min(S.selAnchor, line), std::max(S.selAnchor, line)};

			int cnt = S.sel->second - S.sel->first + 1;

			SetHint(std::to_string(cnt) + (cnt == 1 ? " line" : " lines")
					+ " selected  •  Cmd+C to copy");

			[self setNeedsDisplay:YES];
		}
	}
}

- (void)mouseUp:(NSEvent*)ev
{
	if (!S.hasFile) return;

	NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];

	if (S.gutterDrag >= 0)
	{
		int lo = S.pending ? S.pending->first : S.gutterDrag;
		int hi = S.pending ? S.pending->second : S.gutterDrag;

		S.gutterDrag	= -1;

		Annotation ann;

		ann.id			= core::new_id();
		ann.start_line	= lo;
		ann.end_line	= hi;
		ann.anchor		= core::anchor_text(S.lines[(size_t)lo - 1]);

		EditAnnotation(std::move(ann), true, -1);

		return;
	}

	if (S.selAnchor < 0) return;

	S.selAnchor = -1;

	if (S.dragged) return;

	int line = [self lineAtY:p.y];

	if (S.showAnns)
	{
		int hit = -1;

		for (auto it = gNoteRects.rbegin(); it != gNoteRects.rend(); ++it)
		{
			if (NSPointInRect(p, it->first))
			{
				hit = it->second;

				break;
			}
		}

		if (hit < 0)
		{
			for (int i = (int)S.anns.size() - 1; i >= 0; --i)
			{
				const Annotation& a = S.anns[(size_t)i];

				if (S.visColors.count(a.color) &&
					a.start_line <= line &&
					line <= std::min(a.end_line, LineCount()))
				{
					hit = i;

					break;
				}
			}
		}

		if (hit >= 0)
		{
			EditAnnotation(S.anns[(size_t)hit], false, hit);

			return;
		}
	}

	bool shift = (ev.modifierFlags & NSEventModifierFlagShift) != 0;

	if (shift && S.sel)
	{
		S.sel = {std::min(S.sel->first, line), std::max(S.sel->second, line)};
	}
	else if (S.sel && *S.sel == std::make_pair(line, line))
	{
		S.sel.reset();
	}
	else
	{
		S.sel = {line, line};
	}

	if (S.sel)
	{
		int cnt = S.sel->second - S.sel->first + 1;

		SetHint(std::to_string(cnt) + (cnt == 1 ? " line" : " lines")+ " selected  •  Cmd+C to copy");
	}
	else
	{
		DefaultHint();
	}

	[self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent*)ev
{
	if (ev.modifierFlags & NSEventModifierFlagCommand)
	{
		Zoom(ev.scrollingDeltaY > 0 ? 1 : -1);

		return;
	}

	[super scrollWheel:ev];
}

- (void)magnifyWithEvent:(NSEvent*)ev
{
	Zoom(ev.magnification > 0 ? 1 : -1);
}

- (void)keyDown:(NSEvent*)ev
{
	NSRect		vis		= self.visibleRect;
	NSString*	chars	= ev.charactersIgnoringModifiers;

	if (chars.length == 0) return;

	unichar	ch			= [chars characterAtIndex:0];
	auto	scrollDY	= [&](CGFloat dy)
	{
		CGFloat y = std::max(
			0.0,
			std::min(
				vis.origin.y + dy,
				ContentH() - vis.size.height
			)
		);

		[self scrollPoint:NSMakePoint(vis.origin.x, y)];
		[self setNeedsDisplay:YES];
	};

	switch (ch)
	{
		case 27:						EscapeAction(); return;
		case NSUpArrowFunctionKey:		scrollDY(-gLineH); return;
		case NSDownArrowFunctionKey:	scrollDY(gLineH); return;
		case NSPageUpFunctionKey:		scrollDY(-vis.size.height); return;
		case NSPageDownFunctionKey:		scrollDY(vis.size.height); return;

		case NSHomeFunctionKey:
			[self scrollPoint:NSMakePoint(vis.origin.x, 0)];
			return;

		case NSEndFunctionKey:
			[self scrollPoint:NSMakePoint(
				vis.origin.x,
				std::max(0.0, ContentH() - vis.size.height))];

			return;
	}

	[super keyDown:ev];
}

- (void)copy:(id)sender				{ CopySelection(); }
- (void)undo:(id)sender				{ UndoDelete(); }
- (void)cancelOperation:(id)sender	{ EscapeAction(); }

@end

// ------------------------------------------------------------ app delegate

@interface AppDelegate
	: NSObject <NSApplicationDelegate, NSOutlineViewDataSource,
				NSOutlineViewDelegate, NSTableViewDataSource,
				NSTableViewDelegate, NSTextFieldDelegate>
@property(strong) NSString* startFolder;
@end

@implementation AppDelegate

// --- tree data source ---

- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item
{
	TreeNode* node = item ?: gRoot;

	if (!node) return 0;
	if (!item) return 1;					// the root folder itself

	if (node.isDir && !node.loaded)
		LoadChildren(node);

	return (NSInteger)node.kids.count;
}

- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)idx ofItem:(id)item
{
	if (!item) return gRoot;

	TreeNode* node = item;

	if (node.isDir && !node.loaded)
		LoadChildren(node);

	return node.kids[(NSUInteger)idx];
}

- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item
{
	return ((TreeNode*)item).isDir;
}

- (NSView*)outlineView:(NSOutlineView*)ov
	viewForTableColumn:(NSTableColumn*)col
				  item:(id)item
{
	TreeNode*		node	= item;
	NSTextField*	tf		= [ov makeViewWithIdentifier:@"cell" owner:self];

	if (!tf)
	{
		tf					= [NSTextField labelWithString:@""];

		tf.identifier		= @"cell";
		tf.font				= [NSFont systemFontOfSize:12];
		tf.lineBreakMode	= NSLineBreakByTruncatingTail;
	}

	tf.textColor			= FG;
	tf.stringValue			= node.badge ?
		[node.name stringByAppendingString:@" ●"] :
		node.name;

	return tf;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note
{
	NSInteger row = gTree.selectedRow;

	if (row < 0) return;

	TreeNode* node = [gTree itemAtRow:row];

	if (node && !node.isDir)
		OpenFile(fs::path(U8(node.path)));
}

// --- annotation outline table ---

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv
{
	return (NSInteger)S.anns.size();
}

- (NSView*)tableView:(NSTableView*)tv
	viewForTableColumn:(NSTableColumn*)col
				   row:(NSInteger)row
{
	if (row < 0 || row >= (NSInteger)S.anns.size())
		return nil;

	const Annotation&	a	= S.anns[(size_t)row];
	NSTextField*		tf	= [tv makeViewWithIdentifier:@"ann" owner:self];

	if (!tf)
	{
		tf					= [NSTextField labelWithString:@""];

		tf.identifier		= @"ann";
		tf.font				= [NSFont systemFontOfSize:11];
		tf.lineBreakMode	= NSLineBreakByTruncatingTail;
	}

	std::string rng = std::to_string(a.start_line);

	if (a.end_line != a.start_line)
		rng += "–" + std::to_string(a.end_line);

	std::wstring	collapsed;
	std::wstring	note		= core::utf8_to_wide(a.text);
	bool			word		= false;

	for (wchar_t c : note)
	{
		if (iswspace(c))
		{
			word = false;

			continue;
		}

		if (!word && !collapsed.empty())
			collapsed += L' ';

		collapsed	+= c;
		word		= true;
	}

	if (collapsed.empty()) collapsed = L"(no text)";

	if (collapsed.size() > 60) collapsed.resize(60);

	std::string label =	(a.orphaned ? "⚠ " : "") +
						rng +
						"  " +
						core::wide_to_utf8(collapsed);

	tf.stringValue = NSStr(label);

	const core::AnnColor* c = core::ann_color(a.color);

	tf.textColor = c ? Col(c->r, c->g, c->b) : FG;

	return tf;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note
{
	NSInteger row = gAnnTable.selectedRow;

	if (row >= 0) ShowAnnotationAt((int)row);
}

// --- find/goto fields ---

- (void)controlTextDidChange:(NSNotification*)note
{
	if (note.object == gFindField && U8(gFindField.stringValue) != S.lastQuery)
		FindRefresh();
}

- (BOOL)control:(NSControl*)control
		   textView:(NSTextView*)tv
	doCommandBySelector:(SEL)sel
{
	if (sel == @selector(insertNewline:))
	{
		if (control == gFindField)
		{
			if (NSApp.currentEvent.modifierFlags & NSEventModifierFlagShift)
				FindPrev();
			else
				FindNext();

			return YES;
		}

		if (control == gGotoField)
		{
			GotoLineAction();

			return YES;
		}
	}

	if (sel == @selector(cancelOperation:))
	{
		EscapeAction();

		return YES;
	}

	return NO;
}

// --- actions ---

- (void)pickFolder:(id)sender
{
	NSOpenPanel* panel = [NSOpenPanel openPanel];

	panel.canChooseFiles		= NO;
	panel.canChooseDirectories	= YES;
	panel.title					= @"Open project folder";

	if ([panel runModal] == NSModalResponseOK && panel.URL.path)
		OpenFolder(fs::path(U8(panel.URL.path)));
}

- (void)openFind:(id)sender		{ OpenFindBar(); }
- (void)findNext:(id)sender		{ FindNext(); }
- (void)findPrev:(id)sender		{ FindPrev(); }
- (void)openGoto:(id)sender		{ OpenGotoBar(); }
- (void)zoomIn:(id)sender		{ Zoom(1); }
- (void)zoomOut:(id)sender		{ Zoom(-1); }
- (void)zoomReset:(id)sender	{ Zoom(0); }
- (void)nextAnn:(id)sender		{ GotoAnnotation(1); }
- (void)prevAnn:(id)sender		{ GotoAnnotation(-1); }
- (void)closeFind:(id)sender	{ EscapeAction(); }

- (void)toggleAnns:(id)sender
{
	S.showAnns = gAnnCheckbox.state == NSControlStateValueOn;

	[gCodeView setNeedsDisplay:YES];
}

- (void)opacityChanged:(NSSlider*)slider
{
	S.opacity = slider.doubleValue;

	[gCodeView setNeedsDisplay:YES];
}

- (void)toggleSwatch:(NSButton*)btn
{
	int			i		= (int)btn.tag;
	const char*	name	= core::ANN_COLORS[i].name;

	if (S.visColors.count(name))
		S.visColors.erase(name);
	else
		S.visColors.insert(name);

	btn.image = SwatchImage(i, S.visColors.count(name) != 0);

	[gCodeView setNeedsDisplay:YES];
}

// --- settings ---

- (void)loadSettingsInto:(NSRect*)frame
{
	auto bytes = core::read_file_bytes(S.settingsPath);

	if (!bytes) return;

	json::Value v;

	if (!json::parse(*bytes, v) || v.type != json::Value::Obj)
		return;

	auto num =
		[&](const char* key, double def)
		{
			const json::Value* p = v.get(key);

			return (p && p->type == json::Value::Num) ? p->number : def;
		};

	frame->origin.x		= num("x", frame->origin.x);
	frame->origin.y		= num("y", frame->origin.y);
	frame->size.width	= num("w", frame->size.width);
	frame->size.height	= num("h", frame->size.height);

	int pt = (int)num("font_size", 11);

	if (pt >= 6 && pt <= 28)
		S.fontPt = pt;

	double op = num("opacity", 55);

	if (op >= 10 && op <= 100)
		S.opacity = op;

	const json::Value* p = v.get("show_annotations");

	if (p && p->type == json::Value::Bool) S.showAnns = p->boolean;

	p = v.get("visible_colors");

	if (p && p->type == json::Value::Arr)
	{
		std::set<std::string> colors;

		for (const auto& c : p->arr)
		{
			if (c.type == json::Value::Str && core::ann_color(c.str))
				colors.insert(c.str);
		}

		if (!colors.empty()) S.visColors = colors;
	}

	p = v.get("folder");

	std::error_code ec;

	if (p &&
		p->type == json::Value::Str && !p->str.empty() &&
		fs::is_directory(fs::path(p->str), ec) && !self.startFolder)
	{
		self.startFolder = NSStr(p->str);
	}
}

- (void)saveSettings
{
	NSRect		r	= gWindow.frame;
	json::Value	v	= json::Value::make_obj();

	v.set("x",					json::Value::make_int((long long)r.origin.x));
	v.set("y",					json::Value::make_int((long long)r.origin.y));
	v.set("w",					json::Value::make_int((long long)r.size.width));
	v.set("h",					json::Value::make_int((long long)r.size.height));
	v.set("font_size",			json::Value::make_int(S.fontPt));
	v.set("opacity",			json::Value::make_num(S.opacity));
	v.set("show_annotations",	json::Value::make_bool(S.showAnns));

	json::Value colors = json::Value::make_arr();

	for (const auto& c : S.visColors)
		colors.arr.push_back(json::Value::make_str(c));

	v.set("visible_colors",		std::move(colors));
	v.set("folder",				json::Value::make_str(
		S.wsOpen ? S.ws.folder.string() : ""));

	core::write_file_atomic(S.settingsPath, json::dump(v, 2));
}

// --- UI construction ---

- (NSMenu*)buildMenu
{
	NSMenu*		bar			= [NSMenu new];
	NSMenuItem*	appItem		= [bar addItemWithTitle:@"" action:nil
									  keyEquivalent:@""];
	NSMenu*		appMenu		= [NSMenu new];

	[appMenu addItemWithTitle:@"Quit Code Annotator"
					   action:@selector(terminate:) keyEquivalent:@"q"];

	appItem.submenu = appMenu;

	NSMenuItem*	fileItem	= [bar addItemWithTitle:@"File" action:nil
									  keyEquivalent:@""];
	NSMenu*		fileMenu	= [[NSMenu alloc] initWithTitle:@"File"];

	[[fileMenu addItemWithTitle:@"Open Folder…" action:@selector(pickFolder:)
				  keyEquivalent:@"o"] setTarget:self];

	fileItem.submenu = fileMenu;

	NSMenuItem*	editItem	= [bar addItemWithTitle:@"Edit" action:nil
									  keyEquivalent:@""];
	NSMenu*		editMenu	= [[NSMenu alloc] initWithTitle:@"Edit"];

	[editMenu addItemWithTitle:@"Undo" action:@selector(undo:)
				 keyEquivalent:@"z"];

	[editMenu addItemWithTitle:@"Copy" action:@selector(copy:)
				 keyEquivalent:@"c"];

	editItem.submenu = editMenu;

	NSMenuItem*	viewItem	= [bar addItemWithTitle:@"View" action:nil
									  keyEquivalent:@""];
	NSMenu*		viewMenu	= [[NSMenu alloc] initWithTitle:@"View"];

	[[viewMenu addItemWithTitle:@"Zoom In" action:@selector(zoomIn:)
				  keyEquivalent:@"="] setTarget:self];

	[[viewMenu addItemWithTitle:@"Zoom Out" action:@selector(zoomOut:)
				  keyEquivalent:@"-"] setTarget:self];

	[[viewMenu addItemWithTitle:@"Actual Size" action:@selector(zoomReset:)
				  keyEquivalent:@"0"] setTarget:self];

	viewItem.submenu = viewMenu;

	NSMenuItem*	navItem		= [bar addItemWithTitle:@"Navigate" action:nil
									  keyEquivalent:@""];
	NSMenu*		navMenu		= [[NSMenu alloc] initWithTitle:@"Navigate"];

	[[navMenu addItemWithTitle:@"Find…" action:@selector(openFind:)
				 keyEquivalent:@"f"] setTarget:self];

	[[navMenu addItemWithTitle:@"Find Next" action:@selector(findNext:)
				 keyEquivalent:@"g"] setTarget:self];

	NSMenuItem*	prev		= [navMenu addItemWithTitle:@"Find Previous"
												 action:@selector(findPrev:)
										  keyEquivalent:@"G"];

	prev.keyEquivalentModifierMask	=
		NSEventModifierFlagCommand | NSEventModifierFlagShift;
	prev.target						= self;

	[[navMenu addItemWithTitle:@"Go to Line…" action:@selector(openGoto:)
				 keyEquivalent:@"l"] setTarget:self];

	NSMenuItem*	na			= [navMenu addItemWithTitle:@"Next Annotation"
												 action:@selector(nextAnn:)
										  keyEquivalent:[NSString stringWithFormat:
											  @"%C", (unichar)NSDownArrowFunctionKey]];

	na.keyEquivalentModifierMask	= NSEventModifierFlagOption;
	na.target						= self;

	NSMenuItem*	pa			= [navMenu addItemWithTitle:@"Previous Annotation"
												 action:@selector(prevAnn:)
										  keyEquivalent:[NSString stringWithFormat:
											  @"%C", (unichar)NSUpArrowFunctionKey]];

	pa.keyEquivalentModifierMask	= NSEventModifierFlagOption;
	pa.target						= self;
	navItem.submenu					= navMenu;

	return bar;
}

- (NSView*)buildBar:(BOOL)isFind
{
	NSView* bar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 34)];

	bar.wantsLayer				= YES;
	bar.layer.backgroundColor	= BG_PANEL.CGColor;
	bar.layer.borderColor		= Col(0x3c, 0x3c, 0x3c).CGColor;
	bar.layer.borderWidth		= 1;
	bar.autoresizingMask		= NSViewMinXMargin | NSViewMinYMargin;

	if (isFind)
	{
		gFindField = [[NSTextField alloc]
			initWithFrame:NSMakeRect(8, 6, 150, 22)];
		gFindField.delegate = self;

		[bar addSubview:gFindField];

		gFindCount				= [NSTextField labelWithString:@""];
		gFindCount.frame		= NSMakeRect(164, 8, 70, 18);
		gFindCount.font			= [NSFont systemFontOfSize:11];
		gFindCount.textColor	= FG_DIM;

		[bar addSubview:gFindCount];

		struct
		{
			NSString*	label;
			SEL			action;
		} btns[] =
		{
			{@"‹", @selector(findPrev:)},
			{@"›", @selector(findNext:)},
			{@"✕", @selector(closeFind:)}};

		for (int i = 0; i < 3; ++i)
		{
			NSButton* b = [NSButton buttonWithTitle:btns[i].label
											 target:self
											 action:btns[i].action];

			b.bezelStyle	= NSBezelStyleTexturedRounded;
			b.frame			= NSMakeRect(238 + i * 27, 5, 25, 24);

			[bar addSubview:b];
		}
	}
	else
	{
		NSTextField* lbl = [NSTextField labelWithString:@"Go to line:"];

		lbl.frame		= NSMakeRect(8, 8, 80, 18);
		lbl.font		= [NSFont systemFontOfSize:11];
		lbl.textColor	= FG_DIM;

		[bar addSubview:lbl];

		gGotoField			= [[NSTextField alloc] initWithFrame:NSMakeRect(92, 6, 66, 22)];
		gGotoField.delegate	= self;

		[bar addSubview:gGotoField];

		bar.frame			= NSMakeRect(0, 0, 170, 34);
	}

	bar.hidden = YES;

	return bar;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
	for (const auto& c : core::ANN_COLORS)
		S.visColors.insert(c.name);

	const char* home = getenv("HOME");

	S.settingsPath = fs::path(home ? home : ".") / ".code_annotator_cpp.json";

	NSRect frame = NSMakeRect(120, 120, 1400, 900);

	[self loadSettingsInto:&frame];

	MakeFonts();

	NSApp.mainMenu = [self buildMenu];

	gWindow = [[NSWindow alloc]
		initWithContentRect:frame
				  styleMask:NSWindowStyleMaskTitled |
								NSWindowStyleMaskClosable |
								NSWindowStyleMaskMiniaturizable |
								NSWindowStyleMaskResizable
					backing:NSBackingStoreBuffered
					  defer:NO];

	gWindow.title				= @"Code Annotator";
	gWindow.releasedWhenClosed	= NO;

	if (@available(macOS 10.14, *))
		gWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

	NSView*			content		= gWindow.contentView;
	NSRect			cb			= content.bounds;
	const CGFloat	toolbarH	= 38;
	const CGFloat	statusH		= 24;

	// --- toolbar ---
	NSView* toolbar = [[NSView alloc]
		initWithFrame:NSMakeRect(0, cb.size.height - toolbarH, cb.size.width, toolbarH)];

	toolbar.autoresizingMask		= NSViewWidthSizable | NSViewMinYMargin;
	toolbar.wantsLayer				= YES;
	toolbar.layer.backgroundColor	= BG_PANEL.CGColor;

	NSButton* open = [NSButton buttonWithTitle:@"Open Folder…" target:self
										action:@selector(pickFolder:)];

	open.frame = NSMakeRect(8, 5, 120, 28);

	[toolbar addSubview:open];

	gFolderLabel					= [NSTextField labelWithString:@"No folder open"];

	gFolderLabel.frame				= NSMakeRect(140, 9, cb.size.width - 640, 20);
	gFolderLabel.autoresizingMask	= NSViewWidthSizable;
	gFolderLabel.font				= [NSFont systemFontOfSize:12];
	gFolderLabel.textColor			= FG_DIM;
	gFolderLabel.lineBreakMode		= NSLineBreakByTruncatingMiddle;

	[toolbar addSubview:gFolderLabel];

	CGFloat		x		= cb.size.width - 12;
	NSSlider*	slider	= [NSSlider sliderWithValue:S.opacity minValue:10
										   maxValue:100 target:self
											 action:@selector(opacityChanged:)];

	slider.frame			= NSMakeRect(x - 110, 8, 110, 22);
	slider.autoresizingMask	= NSViewMinXMargin;

	[toolbar addSubview:slider];

	x -= 118;

	NSTextField* opLbl = [NSTextField labelWithString:@"Opacity"];

	opLbl.frame				= NSMakeRect(x - 50, 9, 50, 20);
	opLbl.autoresizingMask	= NSViewMinXMargin;
	opLbl.font				= [NSFont systemFontOfSize:12];
	opLbl.textColor			= FG_DIM;

	[toolbar addSubview:opLbl];

	x -= 58;

	for (int i = core::ANN_COLOR_COUNT - 1; i >= 0; --i)
	{
		x -= 20;

		NSButton* sw = [NSButton buttonWithImage:SwatchImage(i, true)
										  target:self
										  action:@selector(toggleSwatch:)];

		sw.bordered			= NO;
		sw.tag				= i;
		sw.frame			= NSMakeRect(x, 9, 18, 18);
		sw.autoresizingMask	= NSViewMinXMargin;

		gSwatches[i]		= sw;

		[toolbar addSubview:sw];
	}

	x -= 10;

	gAnnCheckbox = [NSButton checkboxWithTitle:@"Annotations" target:self
										action:@selector(toggleAnns:)];

	gAnnCheckbox.state				=	S.showAnns ?
											NSControlStateValueOn :
											NSControlStateValueOff;
	gAnnCheckbox.frame				= NSMakeRect(x - 110, 8, 110, 22);
	gAnnCheckbox.autoresizingMask	= NSViewMinXMargin;

	[toolbar addSubview:gAnnCheckbox];
	[content addSubview:toolbar];

	// --- status bar ---
	NSView* status = [[NSView alloc]
		initWithFrame:NSMakeRect(0, 0, cb.size.width, statusH)];

	status.autoresizingMask			= NSViewWidthSizable | NSViewMaxYMargin;
	status.wantsLayer				= YES;
	status.layer.backgroundColor	= STATUS_BG.CGColor;

	gStatusFile						= [NSTextField labelWithString:@"Ready"];
	gStatusFile.frame				= NSMakeRect(10, 3, cb.size.width / 2, 18);
	gStatusFile.font				= [NSFont systemFontOfSize:11];
	gStatusFile.textColor			= NSColor.whiteColor;

	[status addSubview:gStatusFile];

	gStatusHint						= [NSTextField labelWithString:@""];

	gStatusHint.frame				= NSMakeRect(
		cb.size.width / 2 - 10,
		3,
		cb.size.width / 2,
		18);
	gStatusHint.autoresizingMask	= NSViewMinXMargin;
	gStatusHint.alignment			= NSTextAlignmentRight;
	gStatusHint.font				= [NSFont systemFontOfSize:11];
	gStatusHint.textColor			= Col(0xcd, 0xe6, 0xf7);
	gStatusHint.lineBreakMode		= NSLineBreakByTruncatingTail;

	[status addSubview:gStatusHint];
	[content addSubview:status];

	// --- split: tree+outline | code ---
	NSSplitView* split = [[NSSplitView alloc]
		initWithFrame:NSMakeRect(
			0,
			statusH,
			cb.size.width,
			cb.size.height - toolbarH - statusH)];

	split.vertical			= YES;
	split.dividerStyle		= NSSplitViewDividerStyleThin;
	split.autoresizingMask	= NSViewWidthSizable | NSViewHeightSizable;

	NSView*			left = [[NSView alloc]
		initWithFrame:NSMakeRect(0, 0, 300, split.frame.size.height)];
	NSScrollView*	annScroll = [[NSScrollView alloc]
		initWithFrame:NSMakeRect(0, 0, 300, 150)];

	annScroll.autoresizingMask		= NSViewWidthSizable | NSViewMaxYMargin;
	annScroll.hasVerticalScroller	= YES;

	gAnnTable						= [[NSTableView alloc] initWithFrame:annScroll.bounds];

	NSTableColumn* annCol = [[NSTableColumn alloc] initWithIdentifier:@"ann"];

	annCol.width					= 280;

	[gAnnTable addTableColumn:annCol];

	gAnnTable.headerView			= nil;
	gAnnTable.dataSource			= self;
	gAnnTable.delegate				= self;
	gAnnTable.backgroundColor		= BG_PANEL;
	gAnnTable.rowHeight				= 18;

	annScroll.documentView			= gAnnTable;

	[left addSubview:annScroll];

	NSTextField* annLbl = [NSTextField labelWithString:@"Annotations"];

	annLbl.frame					= NSMakeRect(8, 152, 200, 18);
	annLbl.autoresizingMask			= NSViewMaxYMargin;
	annLbl.font						= [NSFont boldSystemFontOfSize:11];
	annLbl.textColor				= FG_DIM;

	[left addSubview:annLbl];

	NSScrollView* treeScroll = [[NSScrollView alloc]
		initWithFrame:NSMakeRect(0, 174, 300,
								 split.frame.size.height - 174)];

	treeScroll.autoresizingMask		= NSViewWidthSizable | NSViewHeightSizable;
	treeScroll.hasVerticalScroller	= YES;

	gTree							= [[NSOutlineView alloc] initWithFrame:treeScroll.bounds];

	NSTableColumn* treeCol =
		[[NSTableColumn alloc] initWithIdentifier:@"tree"];

	treeCol.width					= 280;

	[gTree addTableColumn:treeCol];

	gTree.outlineTableColumn		= treeCol;
	gTree.headerView				= nil;
	gTree.dataSource				= self;
	gTree.delegate					= self;
	gTree.backgroundColor			= BG_PANEL;
	gTree.rowHeight					= 20;
	treeScroll.documentView			= gTree;

	[left addSubview:treeScroll];
	[split addSubview:left];

	gCodeScroll						= [[NSScrollView alloc]
		initWithFrame:NSMakeRect(0, 0, 800, split.frame.size.height)];

	gCodeScroll.hasVerticalScroller		= YES;
	gCodeScroll.hasHorizontalScroller	= YES;
	gCodeScroll.backgroundColor			= BG;

	gCodeView						= [[CodeView alloc] initWithFrame:gCodeScroll.bounds];

	gCodeScroll.documentView								= gCodeView;
	gCodeScroll.contentView.postsBoundsChangedNotifications	= YES;

	[NSNotificationCenter.defaultCenter
		addObserverForName:NSViewBoundsDidChangeNotification
					object:gCodeScroll.contentView
					 queue:nil
				usingBlock:^(NSNotification*)
				{
					[gCodeView setNeedsDisplay:YES];   // pinned gutter/notes
				}
	];

	[split addSubview:gCodeScroll];
	[content addSubview:split];
	[split setPosition:300 ofDividerAtIndex:0];

	gFindBar = [self buildBar:YES];
	gGotoBar = [self buildBar:NO];

	[content addSubview:gFindBar];
	[content addSubview:gGotoBar];

	PositionBars();

	gPollTimer = [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:YES
		block:^(NSTimer*) { PollSidecar(); }];

	DefaultHint();

	std::error_code ec;

	if (self.startFolder && fs::is_directory(fs::path(U8(self.startFolder)), ec))
		OpenFolder(fs::path(U8(self.startFolder)));

	[gWindow makeKeyAndOrderFront:nil];
	[gWindow makeFirstResponder:gCodeView];

	[NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification*)note
{
	[self saveSettings];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app
{
	return YES;
}

@end

// -------------------------------------------------------------------- main

int main(int argc, const char* argv[])
{
	fs::path	folder;
	fs::path	annRoot;
	bool		check	= false;
	bool		fix		= false;

	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i];

		if 		(arg == "--check")	check	= true;
		else if (arg == "--fix")	fix		= true;
		else if ((arg == "-a" || arg == "--annotations") && i + 1 < argc)
			annRoot = argv[++i];
		else if (!arg.empty() && arg[0] != '-')
			folder = arg;
	}

	if (check)
	{
		return core::check_annotations(folder.empty() ? fs::path(".") : folder,
									   fix,
									   annRoot.empty() ? nullptr : &annRoot);
	}

	@autoreleasepool
	{
		if (!annRoot.empty())
			S.ws.ann_root_arg = annRoot;

		NSApplication* app = [NSApplication sharedApplication];

		[app setActivationPolicy:NSApplicationActivationPolicyRegular];

		gDelegate = [AppDelegate new];

		if (!folder.empty())
			gDelegate.startFolder = NSStr(folder.string());

		app.delegate = gDelegate;

		[app run];
	}

	return 0;
}
