// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
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
// previous annotation, Cmd+E toggles the source editor and Cmd+S saves it.
// Like the Windows port, "insert note as comment" is not included.

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

@interface CodeView : NSView
@end
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

// --------------------------------------------------------- edit mode state

@class EditorView;
@class LineRuler;

struct EdBand					// an annotation's region as UTF-16 offsets [start, end)
{
	NSUInteger	start	= 0;
	NSUInteger	end		= 0;
};

struct EdState
{
	bool		active		= false;
	bool		dirty		= false;		// unsaved editor changes
	bool		loading		= false;		// programmatic text replacement
	bool		crlf		= false;
	bool		bom			= false;
	std::optional<fs::file_time_type>	mtime;		// source mtime at enter/last save
	std::vector<NSUInteger>				lineStarts;	// UTF-16 offset of each line
	std::vector<hl::ScanState>			states;		// scanner state at each line start
	std::vector<EdBand>					bands;		// parallel to S.anns
	int			hlLo		= -1;			// lines awaiting re-highlight
	int			hlHi		= -1;
	bool		hlQueued	= false;
};

static EdState			E;
static EditorView*		gEditor;
static NSTextStorage*	gEditStorage;		// the text view doesn't own it
static NSUndoManager*	gEditUndo;
static NSScrollView*	gEditScroll;
static LineRuler*		gRuler;
static NSButton*		gEditBtn;
static NSButton*		gSaveBtn;

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
static void FindRefresh(bool jump = true);
static void Zoom(int delta);
static void EdGotoLine(int ln, int col = 0);
static void EdGotoBand(int idx);
static void EdOnZoom();
static bool EdExit();
static bool EdSave();
static void EdToggle();
static std::vector<std::wstring> EdLines();

// ------------------------------------------------------------------- status

static void SetHint(const std::string& text)
{
	gStatusHint.stringValue = NSStr(text);
}

static void DefaultHint()
{
	if (gFlashActive) return;

	if (E.active)
	{
		SetHint("Editing • Cmd+Z undo • Shift+Cmd+Z redo • Cmd+S save • "
				"Done returns to the annotate view");

		return;
	}

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

	if (E.active)						// jump the editor caret to its band
	{
		EdGotoBand(idx);

		return;
	}

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
	if (E.active || !S.hasFile || S.anns.empty()) return;

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

	if (E.active)
	{
		gFindCount.stringValue = [NSString stringWithFormat:@"%d of %d", S.matchIdx + 1, n];

		EdGotoLine(m.ln, m.col);

		return;
	}

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

// jump=false (refresh after an edit) recounts without moving the caret
static void FindRefresh(bool jump)
{
	std::string query = U8(gFindField.stringValue);

	S.lastQuery = query;

	S.matches.clear();

	S.matchIdx = -1;

	std::wstring wq = core::utf8_to_wide(query);

	std::transform(wq.begin(), wq.end(), wq.begin(), towlower);

	if (!wq.empty() && S.hasFile)
	{
		std::vector<std::wstring>			edLines;

		if (E.active) edLines = EdLines();

		const std::vector<std::wstring>&	src	= E.active ? edLines : S.lines;

		for (int row = 0; row < (int)src.size() && S.matches.size() < 2000; ++row)
		{
			std::wstring low = src[(size_t)row];

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

	if (!S.matches.empty() && jump)
	{
		GotoMatch(0);
	}
	else if (!S.matches.empty())
	{
		gFindCount.stringValue = [NSString stringWithFormat:@"%d match%@",
			(int)S.matches.size(), S.matches.size() == 1 ? @"" : @"es"];
	}
	else
	{
		gFindCount.stringValue = wq.empty() ? @"" : @"no results";

		[gCodeView setNeedsDisplay:YES];
	}

	if (E.active) [(NSView*)gEditor setNeedsDisplay:YES];
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
	if (E.active)
	{
		gFindBar.hidden = YES;
		gGotoBar.hidden = YES;

		S.matches.clear();

		S.matchIdx = -1;

		S.lastQuery.clear();

		[(NSView*)gEditor setNeedsDisplay:YES];

		[gWindow makeFirstResponder:(NSView*)gEditor];

		return;
	}

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

	if (E.active)
	{
		gGotoBar.hidden = YES;

		EdGotoLine(std::min(ln, (int)E.lineStarts.size()));

		[gWindow makeFirstResponder:(NSView*)gEditor];

		return;
	}

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

	if (E.active) EdOnZoom();

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
	if (E.active || !S.deletedAnn) return;

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
	if (!S.hasFile || S.annError || E.active) return;

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
	if (!EdExit()) return;				// user kept editing

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
		@"Click an existing highlight to edit it, or use ✎ Edit\n"
		@"to change the source itself.\n\n"
		@"Cmd+F finds text, Cmd+L jumps to a line, Cmd+C copies\n"
		@"selected lines, Cmd+= / Cmd+- zoom the code,\n"
		@"Option+Down/Option+Up jump between annotations.\n\n"
		@"Notes are saved in a parallel tree that mirrors the\n"
		@"source layout (default: <project>/.annotations),\n"
		@"so annotating never touches the source files.";

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

// ---------------------------------------------------------------- edit mode
//
// "✎ Edit" swaps a real NSTextView in for the code view: native editing,
// undo/redo and clipboard, syntax colors as layout-manager temporary
// attributes (re-scanned incrementally, never part of the text or undo),
// and annotation bands that ride along with the text. Mirrors EditorPane in
// code_annotator.py.

@interface EditorView : NSTextView <NSTextStorageDelegate, NSTextViewDelegate>
@end

@interface LineRuler : NSRulerView
@end

static NSDictionary* EdBaseAttrs()
{
	NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new];

	ps.tabStops				= @[];
	ps.defaultTabInterval	= gCharW * core::TAB_WIDTH;
	ps.minimumLineHeight	= gLineH;
	ps.maximumLineHeight	= gLineH;

	return @{
		NSFontAttributeName:			gCodeFont,
		NSForegroundColorAttributeName:	FG,
		NSParagraphStyleAttributeName:	ps,
		NSBaselineOffsetAttributeName:	@2,
	};
}

static std::string U8Lossy(NSString* s)
{
	NSData* d = [s dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];

	return d ? std::string((const char*)d.bytes, d.length) : std::string();
}

static CGFloat EdGutterW()
{
	int digits = (int)std::to_string(std::max<size_t>(1, E.lineStarts.size())).size();

	return kPad * 2 + std::max(2, digits) * gCharW;
}

// 0-based line containing UTF-16 offset off
static int EdLineOf(NSUInteger off)
{
	auto it = std::upper_bound(E.lineStarts.begin(), E.lineStarts.end(), off);

	return std::max(0, (int)(it - E.lineStarts.begin()) - 1);
}

static NSUInteger EdLineEnd(int ln, NSUInteger length)		// excludes the '\n'
{
	return ln + 1 < (int)E.lineStarts.size() ? E.lineStarts[(size_t)ln + 1] - 1 : length;
}

static void EdIndexLines(NSString* s)
{
	NSUInteger				n	= s.length;
	std::vector<unichar>	buf(n);

	[s getCharacters:buf.data() range:NSMakeRange(0, n)];

	E.lineStarts.assign(1, 0);

	for (NSUInteger i = 0; i < n; ++i)
		if (buf[i] == '\n') E.lineStarts.push_back(i + 1);
}

// one line as wchar_t (UTF-32 here), plus the UTF-16 offset of each
// character so token columns map back onto NSString ranges
static std::wstring EdLineText(NSString* s, int ln, std::vector<NSUInteger>& offs)
{
	NSUInteger				a	= E.lineStarts[(size_t)ln];
	NSUInteger				b	= EdLineEnd(ln, s.length);
	std::vector<unichar>	u(b - a);
	std::wstring			w;

	[s getCharacters:u.data() range:NSMakeRange(a, b - a)];

	offs.clear();

	for (size_t i = 0; i < u.size(); ++i)
	{
		offs.push_back(i);

		uint32_t c = u[i];

		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u.size() &&
			u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
		{
			c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
			++i;
		}

		w += (wchar_t)c;
	}

	offs.push_back(u.size());

	return w;
}

// UTF-16 offset of (0-based line, character column)
static NSUInteger EdOffsetOf(int ln, int col)
{
	ln = std::max(0, std::min(ln, (int)E.lineStarts.size() - 1));

	std::vector<NSUInteger> offs;

	EdLineText(gEditor.string, ln, offs);

	return E.lineStarts[(size_t)ln] + offs[(size_t)std::max(0, std::min(col, (int)offs.size() - 1))];
}

// the editor's text as lines, newlines normalized
static std::vector<std::wstring> EdLines()
{
	return core::split_source_lines(core::utf8_to_wide(U8Lossy(gEditor.string)));
}

static bool SameScanState(const hl::ScanState& a, const hl::ScanState& b)
{
	return a.mode == b.mode && a.delim == b.delim;
}

static void EdHighlightLine(NSString* s, int ln, const hl::LangCfg* cfg, hl::ScanState& st)
{
	std::vector<NSUInteger>	offs;
	std::wstring			w		= EdLineText(s, ln, offs);
	NSUInteger				base	= E.lineStarts[(size_t)ln];
	NSLayoutManager*		lm		= gEditor.layoutManager;

	[lm removeTemporaryAttribute:NSForegroundColorAttributeName
			   forCharacterRange:NSMakeRange(base, offs.back())];

	if (!cfg) return;

	auto toks = cfg->html ? hl::scan_html_line(w, st) : hl::scan_line(w, *cfg, st);

	for (const auto& t : toks)
	{
		if (t.cls == hl::Tok::None) continue;

		size_t c0 = std::min((size_t)t.col, offs.size() - 1);
		size_t c1 = std::min((size_t)t.col + t.text.size(), offs.size() - 1);

		[lm addTemporaryAttribute:NSForegroundColorAttributeName
							value:TokColor(t.cls)
				forCharacterRange:NSMakeRange(base + offs[c0], offs[c1] - offs[c0])];
	}
}

// re-scan lines from..to, then keep going while the scanner state carried
// into the following lines differs from what they were scanned with
static void EdRehighlight(int from, int to)
{
	NSString*			s	= gEditor.string;
	const hl::LangCfg*	cfg	= hl::lang_cfg(S.langId);
	int					n	= (int)E.lineStarts.size();

	from = std::max(0, std::min(from, n - 1));

	hl::ScanState st = E.states[(size_t)from];

	for (int i = from; i < n; ++i)
	{
		if (i > to && (!cfg || SameScanState(st, E.states[(size_t)i]))) break;

		E.states[(size_t)i] = st;

		EdHighlightLine(s, i, cfg, st);
	}
}

// Bands follow the text like Tk tags: text typed at a band's edges stays
// outside it, text typed inside it joins it, and a band whose whole region
// is deleted collapses to a point that then behaves like a left-gravity
// mark (the orphan position used on save). [a, b) was replaced; the new
// text ends at e.
static void EdShiftBands(NSUInteger a, NSUInteger b, NSUInteger e)
{
	auto past = [&](NSUInteger p) { return p - b + e; };

	for (auto& bd : E.bands)
	{
		if (bd.start == bd.end)
		{
			NSUInteger p = bd.start;

			if (p > b)			p = past(p);
			else if (p >= a)	p = a;

			bd.start = bd.end = p;

			continue;
		}

		if (a < b && bd.start >= a && bd.end <= b)
		{
			bd.start = bd.end = a;				// whole region deleted

			continue;
		}

		if (bd.start >= a)	bd.start	= bd.start >= b ? past(bd.start) : e;
		if (bd.end > a)		bd.end		= bd.end > b ? past(bd.end) : a;

		if (bd.end < bd.start) bd.end = bd.start;
	}
}

static void EdUpdateTitle()
{
	if (!E.active) return;

	std::string status = S.filePath.filename().string() + "  •  editing";

	if (E.dirty) status += "  ●";

	gStatusFile.stringValue = NSStr(status);
}

static void EdAfterChange()
{
	gRuler.ruleThickness = EdGutterW();

	EdUpdateTitle();

	if (!gFindBar.hidden) FindRefresh(false);

	[gRuler setNeedsDisplay:YES];
	[gEditor setNeedsDisplay:YES];
}

// text storage edit: [loc, loc + oldLen) became [loc, loc + newLen)
static void EdOnEdit(NSString* s, NSUInteger loc, NSUInteger oldLen, NSUInteger newLen)
{
	NSUInteger	oldEnd	= loc + oldLen;
	NSInteger	delta	= (NSInteger)newLen - (NSInteger)oldLen;

	// line starts inside (loc, oldEnd] came from removed newlines
	auto	lo		= std::upper_bound(E.lineStarts.begin(), E.lineStarts.end(), loc);
	auto	hi		= std::upper_bound(E.lineStarts.begin(), E.lineStarts.end(), oldEnd);
	size_t	idx		= (size_t)(lo - E.lineStarts.begin());
	size_t	removed	= (size_t)(hi - lo);
	int		first	= (int)idx - 1;

	for (auto it = hi; it != E.lineStarts.end(); ++it)
		*it = (NSUInteger)((NSInteger)*it + delta);

	std::vector<NSUInteger>	added;
	std::vector<unichar>	buf(newLen);

	[s getCharacters:buf.data() range:NSMakeRange(loc, newLen)];

	for (NSUInteger i = 0; i < newLen; ++i)
		if (buf[i] == '\n') added.push_back(loc + i + 1);

	E.lineStarts.erase(E.lineStarts.begin() + (ptrdiff_t)idx,
					   E.lineStarts.begin() + (ptrdiff_t)(idx + removed));
	E.lineStarts.insert(E.lineStarts.begin() + (ptrdiff_t)idx, added.begin(), added.end());

	E.states.erase(E.states.begin() + (ptrdiff_t)idx,
				   E.states.begin() + (ptrdiff_t)(idx + removed));
	E.states.insert(E.states.begin() + (ptrdiff_t)idx, added.size(), hl::ScanState{});

	EdShiftBands(loc, oldEnd, loc + newLen);

	E.dirty = true;

	// re-highlight once the layout manager has caught up with the edit
	int hiLine = first + (int)added.size();

	if (E.hlLo >= 0)
	{
		if (E.hlHi > first) E.hlHi += (int)added.size() - (int)removed;

		E.hlLo = std::min(E.hlLo, first);
		E.hlHi = std::max(E.hlHi, hiLine);
	}
	else
	{
		E.hlLo = first;
		E.hlHi = hiLine;
	}

	if (!E.hlQueued)
	{
		E.hlQueued = true;

		dispatch_async(dispatch_get_main_queue(), ^{
			E.hlQueued = false;

			if (!E.active || E.hlLo < 0) return;

			int a = E.hlLo, b = E.hlHi;

			E.hlLo = E.hlHi = -1;

			EdRehighlight(a, b);
			EdAfterChange();
		});
	}
}

// a line's fragment rect in editor coordinates
static NSRect EdLineRect(int ln)
{
	NSLayoutManager*	lm	= gEditor.layoutManager;
	NSUInteger			len	= gEditor.string.length;
	NSUInteger			ci	= E.lineStarts[(size_t)std::max(0, std::min(ln, (int)E.lineStarts.size() - 1))];
	NSRect				r;

	if (ci >= len)			// the empty line after a trailing newline
	{
		r = lm.extraLineFragmentRect;

		if (NSIsEmptyRect(r) && len > 0)
		{
			NSRect last = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:len - 1]
											   effectiveRange:nil];

			r = NSOffsetRect(last, 0, last.size.height);
		}
	}
	else
	{
		r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:ci]
								 effectiveRange:nil];
	}

	NSPoint o = gEditor.textContainerOrigin;

	return NSOffsetRect(r, o.x, o.y);
}

// --- views --------------------------------------------------------------------

@implementation EditorView

- (void)textStorage:(NSTextStorage*)ts
	didProcessEditing:(NSTextStorageEditActions)mask
				range:(NSRange)r
	   changeInLength:(NSInteger)delta
{
	if (!(mask & NSTextStorageEditedCharacters) || E.loading || !E.active)
		return;

	EdOnEdit(ts.string, r.location, (NSUInteger)((NSInteger)r.length - delta), r.length);
}

- (NSUndoManager*)undoManagerForTextView:(NSTextView*)tv	{ return gEditUndo; }

- (void)textViewDidChangeSelection:(NSNotification*)note	{ [gRuler setNeedsDisplay:YES]; }

- (void)undo:(id)sender		{ [gEditUndo undo]; }
- (void)redo:(id)sender		{ [gEditUndo redo]; }

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
	if (item.action == @selector(undo:)) return gEditUndo.canUndo;
	if (item.action == @selector(redo:)) return gEditUndo.canRedo;

	return [super validateMenuItem:item];
}

// pasted text may carry "\r\n"; the editor keeps '\n' only
- (void)paste:(id)sender
{
	NSString* s = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];

	if (!s.length) return;

	s = [[s stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]
			stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];

	[self insertText:s replacementRange:self.selectedRange];
}

- (void)cancelOperation:(id)sender	{ EscapeAction(); }
- (void)complete:(id)sender			{ EscapeAction(); }		// Esc in a text view

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

// full-width annotation bands and find matches, under the text
- (void)drawViewBackgroundInRect:(NSRect)rect
{
	[super drawViewBackgroundInRect:rect];

	if (!E.active) return;

	double	alpha	= S.opacity / 100.0;
	CGFloat	width	= std::max(self.bounds.size.width, NSMaxX(rect));

	if (S.showAnns)
	{
		for (size_t i = 0; i < E.bands.size() && i < S.anns.size(); ++i)
		{
			const Annotation&	a	= S.anns[i];
			const EdBand&		bd	= E.bands[i];

			if (!S.visColors.count(a.color) || bd.start == bd.end) continue;

			int lo = EdLineOf(bd.start);
			int el = EdLineOf(bd.end);
			int hi = std::max(lo, bd.end == E.lineStarts[(size_t)el] ? el - 1 : el);

			NSRect r0	= EdLineRect(lo);
			NSRect r1	= EdLineRect(hi);
			NSRect band	= NSMakeRect(0, NSMinY(r0), width, NSMaxY(r1) - NSMinY(r0));

			if (!NSIntersectsRect(band, rect)) continue;

			const core::AnnColor* c = core::ann_color(a.color);

			if (!c) c = &core::ANN_COLORS[0];

			[BlendBG(BG, c->r, c->g, c->b, alpha * (a.orphaned ? 0.18 : 0.45)) setFill];

			NSRectFill(band);

			[BlendBG(BG, c->r, c->g, c->b, std::min(1.0, alpha * 1.5)) setFill];

			NSRectFill(NSMakeRect(0, band.origin.y, 3, band.size.height));
		}
	}

	NSLayoutManager* lm = self.layoutManager;

	for (int i = 0; i < (int)S.matches.size(); ++i)
	{
		const Match&	m	= S.matches[(size_t)i];
		NSUInteger		a	= EdOffsetOf(m.ln - 1, m.col);
		NSUInteger		b	= EdOffsetOf(m.ln - 1, m.col + m.len);
		NSRange			gr	= [lm glyphRangeForCharacterRange:NSMakeRange(a, b - a)
											 actualCharacterRange:nil];
		NSRect			r	= [lm boundingRectForGlyphRange:gr inTextContainer:self.textContainer];
		NSPoint			o	= self.textContainerOrigin;

		r = NSOffsetRect(r, o.x, o.y);

		if (!NSIntersectsRect(r, rect)) continue;

		[BlendBG(BG, 210, 170, 60, i == S.matchIdx ? 0.55 : 0.25) setFill];

		NSRectFill(r);
	}
}

@end

@implementation LineRuler

- (BOOL)isFlipped					{ return YES; }
- (CGFloat)requiredThickness		{ return EdGutterW(); }

- (void)drawRect:(NSRect)dirty
{
	[BG setFill];

	NSRectFill(dirty);

	if (!E.active || E.lineStarts.empty()) return;

	NSLayoutManager*	lm		= gEditor.layoutManager;
	NSRect				vis		= gEditor.visibleRect;
	NSRange				gr		= [lm glyphRangeForBoundingRect:vis
											  inTextContainer:gEditor.textContainer];
	NSRange				cr		= [lm characterRangeForGlyphRange:gr actualGlyphRange:nil];
	int					caretLn	= EdLineOf(gEditor.selectedRange.location);
	CGFloat				w		= self.ruleThickness;
	int					n		= (int)E.lineStarts.size();

	for (int ln = EdLineOf(cr.location);
		 ln < n && E.lineStarts[(size_t)ln] <= NSMaxRange(cr);
		 ++ln)
	{
		NSRect		lr		= EdLineRect(ln);
		CGFloat		y		= [self convertPoint:NSMakePoint(0, lr.origin.y) fromView:gEditor].y;
		NSString*	num		= [NSString stringWithFormat:@"%d", ln + 1];
		NSDictionary* attrs	= @{
			NSFontAttributeName:			gCodeFont,
			NSForegroundColorAttributeName:	ln == caretLn ? FG : GUTTER_FG,
		};
		NSSize		sz		= [num sizeWithAttributes:attrs];

		[num drawAtPoint:NSMakePoint(w - kPad - sz.width, y + 2) withAttributes:attrs];
	}
}

@end

// --- entering / leaving / saving ---------------------------------------------

static void EdHideBars()
{
	gFindBar.hidden = YES;
	gGotoBar.hidden = YES;

	S.matches.clear();

	S.matchIdx = -1;

	S.lastQuery.clear();
}

static void EdShowRange(NSUInteger off)
{
	[gEditor setSelectedRange:NSMakeRange(off, 0)];
	[gEditor scrollRangeToVisible:NSMakeRange(off, 0)];
	[gRuler setNeedsDisplay:YES];
	[gEditor setNeedsDisplay:YES];
}

static void EdGotoLine(int ln, int col)
{
	if (!E.active) return;

	EdShowRange(EdOffsetOf(ln - 1, col));
}

static void EdGotoBand(int idx)
{
	if (!E.active || idx < 0 || idx >= (int)E.bands.size()) return;

	EdShowRange(E.bands[(size_t)idx].start);
}

static void EdEnter()
{
	if (E.active) return;

	if (!S.hasFile)
	{
		FlashStatus("Open a file first");

		return;
	}

	if (S.truncated)
	{
		FlashStatus("File is truncated in this view — too large to edit here");

		return;
	}

	core::EditSource src = core::load_source_for_edit(S.filePath);

	if (src.status == core::EditSource::Unreadable)
	{
		FlashStatus("Could not read file: " + S.filePath.filename().string());

		return;
	}

	if (src.status == core::EditSource::NotUtf8)
	{
		FlashStatus("File is not valid UTF-8 — editing disabled");

		return;
	}

	EdHideBars();

	S.sel.reset();
	S.pending.reset();

	std::error_code ec;
	auto			t	= fs::last_write_time(S.filePath, ec);

	E.mtime	= ec ? std::nullopt : std::optional(t);
	E.crlf	= src.crlf;
	E.bom	= src.bom;

	std::wstring joined;

	for (size_t i = 0; i < src.lines.size(); ++i)
	{
		if (i) joined += L'\n';

		joined += src.lines[i];
	}

	NSString*	text	= NSStrW(joined);
	CGFloat		topY	= gCodeView.visibleRect.origin.y;

	E.loading = true;

	[gEditor.textStorage setAttributedString:
		[[NSAttributedString alloc] initWithString:text attributes:EdBaseAttrs()]];

	E.loading = false;

	gEditor.typingAttributes = EdBaseAttrs();

	[gEditUndo removeAllActions];

	EdIndexLines(text);

	int n = (int)E.lineStarts.size();

	E.states.assign((size_t)n, hl::ScanState{});
	E.bands.clear();

	for (const auto& a : S.anns)
	{
		int		lo	= std::min(a.start_line, n) - 1;
		int		hi	= std::min(a.end_line, n);			// next line, 0-based
		EdBand	bd;

		bd.start	= E.lineStarts[(size_t)std::max(0, lo)];
		bd.end		= hi < n ? E.lineStarts[(size_t)hi] : text.length;

		E.bands.push_back(bd);
	}

	E.active	= true;
	E.dirty		= false;
	E.hlLo		= -1;
	E.hlHi		= -1;

	EdRehighlight(0, n - 1);

	gCodeScroll.hidden		= YES;
	gEditScroll.hidden		= NO;
	gEditor.minSize			= gEditScroll.contentView.bounds.size;
	gRuler.ruleThickness	= EdGutterW();
	gEditBtn.title			= @"✓ Done";
	gSaveBtn.hidden			= NO;

	// open at the spot the annotate view was showing
	int top = std::max(0, std::min(n - 1, (int)ceil(topY / gLineH)));

	[gEditor setSelectedRange:NSMakeRange(E.lineStarts[(size_t)top], 0)];
	[gEditor scrollPoint:NSMakePoint(0, topY)];
	[gWindow makeFirstResponder:gEditor];

	EdUpdateTitle();
	DefaultHint();

	[gRuler setNeedsDisplay:YES];
}

// re-anchor annotations from their bands, then persist any changes
static void EdSyncAnnotations()
{
	std::vector<std::wstring>	lines	= EdLines();
	bool						changed	= false;

	for (size_t i = 0; i < S.anns.size() && i < E.bands.size(); ++i)
	{
		const EdBand&	bd	= E.bands[i];
		int				sl	= EdLineOf(bd.start);
		int				el	= EdLineOf(bd.end);

		changed |= core::rebase_annotation(
			S.anns[i],
			sl,
			el,
			(int)(bd.end - E.lineStarts[(size_t)el]),
			bd.start == bd.end,
			lines);
	}

	if (!changed) return;

	// keep the bands parallel to the (re-sorted) annotations
	std::vector<std::pair<Annotation, EdBand>> pairs;

	for (size_t i = 0; i < S.anns.size(); ++i)
		pairs.push_back({S.anns[i], i < E.bands.size() ? E.bands[i] : EdBand{}});

	std::stable_sort(
		pairs.begin(),
		pairs.end(),
		[](const auto& x, const auto& y) { return x.first.start_line < y.first.start_line; });

	for (size_t i = 0; i < pairs.size(); ++i)
	{
		S.anns[i] = pairs[i].first;

		if (i < E.bands.size()) E.bands[i] = pairs[i].second;
	}

	Persist();
}

static bool EdSave()
{
	if (!E.active) return false;

	NSString*		name	= NSStr(S.filePath.filename().string());
	std::error_code	ec;
	auto			onDisk	= fs::last_write_time(S.filePath, ec);

	if (!ec && E.mtime && onDisk != *E.mtime)
	{
		NSAlert* a = [NSAlert new];

		a.messageText		= @"File changed on disk";
		a.informativeText	= [NSString stringWithFormat:
			@"%@ was modified outside the editor.\n\nOverwrite it with your version?", name];

		[a addButtonWithTitle:@"Overwrite"];
		[a addButtonWithTitle:@"Cancel"];

		if ([a runModal] != NSAlertFirstButtonReturn) return false;
	}

	// atomic save (temp file + rename), so a crash mid-save can't destroy
	// the source file
	if (!core::write_file_atomic(S.filePath, core::join_source(EdLines(), E.crlf, E.bom)))
	{
		NSAlert* a = [NSAlert new];

		a.messageText		= @"Save failed";
		a.informativeText	= [NSString stringWithFormat:
			@"Could not write the source file:\n%@", NSStr(S.filePath.string())];

		[a runModal];

		return false;
	}

	auto t = fs::last_write_time(S.filePath, ec);

	E.mtime = ec ? std::nullopt : std::optional(t);

	EdSyncAnnotations();

	E.dirty = false;

	EdUpdateTitle();
	FlashStatus("Saved " + S.filePath.filename().string());

	[gEditor setNeedsDisplay:YES];

	return true;
}

// leave edit mode; false if the user cancelled (or a requested save failed)
static bool EdExit()
{
	if (!E.active) return true;

	if (E.dirty)
	{
		NSAlert* a = [NSAlert new];

		a.messageText		= [NSString stringWithFormat:@"Save changes to %@?",
								  NSStr(S.filePath.filename().string())];
		a.informativeText	= @"Your changes will be lost if you don't save them.";

		[a addButtonWithTitle:@"Save"];
		[a addButtonWithTitle:@"Cancel"];
		[a addButtonWithTitle:@"Don't Save"];

		NSModalResponse r = [a runModal];

		if (r == NSAlertSecondButtonReturn) return false;

		if (r == NSAlertFirstButtonReturn && !EdSave()) return false;
	}

	CGFloat topY = gEditor.visibleRect.origin.y;

	EdHideBars();

	E.active = false;

	E.lineStarts.clear();
	E.states.clear();
	E.bands.clear();

	E.loading = true;

	[gEditor.textStorage setAttributedString:[NSAttributedString new]];

	E.loading = false;

	[gEditUndo removeAllActions];

	gEditScroll.hidden	= YES;
	gCodeScroll.hidden	= NO;
	gEditBtn.title		= @"✎ Edit";
	gSaveBtn.hidden		= YES;

	fs::path path = S.filePath;

	OpenFile(path);						// re-render the (possibly new) source

	[gCodeView scrollPoint:NSMakePoint(0, topY)];
	[gWindow makeFirstResponder:gCodeView];

	return true;
}

static void EdToggle()
{
	if (E.active)	EdExit();
	else			EdEnter();
}

static void EdOnZoom()
{
	NSTextStorage* ts = gEditor.textStorage;

	// attribute-only change: not an edit, not undoable
	[ts addAttributes:EdBaseAttrs() range:NSMakeRange(0, ts.length)];

	gEditor.typingAttributes	= EdBaseAttrs();
	gRuler.ruleThickness		= EdGutterW();

	[gRuler setNeedsDisplay:YES];
	[gEditor setNeedsDisplay:YES];
}

static void EdBuildViews(NSRect frame)
{
	gEditScroll = [[NSScrollView alloc] initWithFrame:frame];

	gEditScroll.hasVerticalScroller		= YES;
	gEditScroll.hasHorizontalScroller	= YES;
	gEditScroll.backgroundColor			= BG;
	gEditScroll.autoresizingMask		= NSViewWidthSizable | NSViewHeightSizable;
	gEditScroll.hidden					= YES;

	// TextKit 1 stack, built explicitly: no wrapping, and the layout manager
	// carries the syntax colors as temporary attributes
	gEditStorage = [NSTextStorage new];

	NSLayoutManager*	lm	= [NSLayoutManager new];
	NSTextContainer*	tc	= [[NSTextContainer alloc]
		initWithContainerSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];

	tc.widthTracksTextView			= NO;
	tc.heightTracksTextView			= NO;
	lm.allowsNonContiguousLayout	= YES;

	[gEditStorage addLayoutManager:lm];
	[lm addTextContainer:tc];

	gEditor = [[EditorView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)
								  textContainer:tc];

	gEditor.minSize							= NSMakeSize(0, 0);
	gEditor.maxSize							= NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
	gEditor.verticallyResizable				= YES;
	gEditor.horizontallyResizable			= YES;
	gEditor.autoresizingMask				= NSViewWidthSizable;
	gEditor.textContainerInset				= NSMakeSize(4, kPad);
	gEditor.richText						= NO;
	gEditor.importsGraphics					= NO;
	gEditor.allowsUndo						= YES;
	gEditor.usesFindBar						= NO;
	gEditor.smartInsertDeleteEnabled		= NO;
	gEditor.automaticQuoteSubstitutionEnabled	= NO;
	gEditor.automaticDashSubstitutionEnabled	= NO;
	gEditor.automaticTextReplacementEnabled		= NO;
	gEditor.automaticSpellingCorrectionEnabled	= NO;
	gEditor.continuousSpellCheckingEnabled		= NO;
	gEditor.grammarCheckingEnabled				= NO;
	gEditor.automaticLinkDetectionEnabled		= NO;
	gEditor.enabledTextCheckingTypes			= 0;
	gEditor.backgroundColor					= BG;
	gEditor.drawsBackground					= YES;
	gEditor.insertionPointColor				= FG;
	gEditor.selectedTextAttributes			= @{
		NSBackgroundColorAttributeName: Col(0x26, 0x4f, 0x78)};
	gEditor.typingAttributes				= EdBaseAttrs();
	gEditor.delegate						= gEditor;
	gEditStorage.delegate					= gEditor;

	gEditUndo = [NSUndoManager new];

	gEditScroll.documentView = gEditor;

	gRuler = [[LineRuler alloc] initWithScrollView:gEditScroll
									   orientation:NSVerticalRuler];

	gRuler.clientView				= gEditor;
	gRuler.ruleThickness			= EdGutterW();
	gEditScroll.verticalRulerView	= gRuler;
	gEditScroll.hasVerticalRuler	= YES;
	gEditScroll.rulersVisible		= YES;

	gEditScroll.contentView.postsBoundsChangedNotifications	= YES;
	gEditScroll.contentView.postsFrameChangedNotifications	= YES;

	[NSNotificationCenter.defaultCenter
		addObserverForName:NSViewBoundsDidChangeNotification
					object:gEditScroll.contentView
					 queue:nil
				usingBlock:^(NSNotification*)
				{
					[gRuler setNeedsDisplay:YES];
				}
	];

	// at least as large as the visible area, so bands span the whole pane
	[NSNotificationCenter.defaultCenter
		addObserverForName:NSViewFrameDidChangeNotification
					object:gEditScroll.contentView
					 queue:nil
				usingBlock:^(NSNotification*)
				{
					gEditor.minSize = gEditScroll.contentView.bounds.size;
				}
	];
}

// ------------------------------------------------------------ app delegate

@interface AppDelegate
	: NSObject <NSApplicationDelegate, NSOutlineViewDataSource,
				NSOutlineViewDelegate, NSTableViewDataSource,
				NSTableViewDelegate, NSTextFieldDelegate, NSWindowDelegate>
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

// leaving the editor (and its unsaved-changes prompt) comes before
// switching files; refusing the selection keeps the editor open
- (BOOL)outlineView:(NSOutlineView*)ov shouldSelectItem:(id)item
{
	TreeNode* node = item;

	if (E.active && node && !node.isDir &&
		fs::path(U8(node.path)) != S.filePath)
	{
		return EdExit();
	}

	return YES;
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
- (void)toggleEdit:(id)sender	{ EdToggle(); }
- (void)saveSource:(id)sender	{ EdSave(); }

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
	if (item.action == @selector(toggleEdit:))
	{
		item.title = E.active ? @"Done Editing" : @"Edit Source";

		return S.hasFile;
	}

	if (item.action == @selector(saveSource:)) return E.active;

	return YES;
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
	return EdExit();
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app
{
	return EdExit() ? NSTerminateNow : NSTerminateCancel;
}

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

	[fileMenu addItem:[NSMenuItem separatorItem]];

	[[fileMenu addItemWithTitle:@"Edit Source" action:@selector(toggleEdit:)
				  keyEquivalent:@"e"] setTarget:self];

	[[fileMenu addItemWithTitle:@"Save" action:@selector(saveSource:)
				  keyEquivalent:@"s"] setTarget:self];

	fileItem.submenu = fileMenu;

	NSMenuItem*	editItem	= [bar addItemWithTitle:@"Edit" action:nil
									  keyEquivalent:@""];
	NSMenu*		editMenu	= [[NSMenu alloc] initWithTitle:@"Edit"];

	[editMenu addItemWithTitle:@"Undo" action:@selector(undo:)
				 keyEquivalent:@"z"];

	NSMenuItem* redo = [editMenu addItemWithTitle:@"Redo" action:@selector(redo:)
									keyEquivalent:@"Z"];

	redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;

	[editMenu addItem:[NSMenuItem separatorItem]];

	[editMenu addItemWithTitle:@"Cut" action:@selector(cut:)
				 keyEquivalent:@"x"];

	[editMenu addItemWithTitle:@"Copy" action:@selector(copy:)
				 keyEquivalent:@"c"];

	[editMenu addItemWithTitle:@"Paste" action:@selector(paste:)
				 keyEquivalent:@"v"];

	[editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:)
				 keyEquivalent:@"a"];

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
	gWindow.delegate			= self;

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

	gEditBtn		= [NSButton buttonWithTitle:@"✎ Edit" target:self
										 action:@selector(toggleEdit:)];
	gEditBtn.frame	= NSMakeRect(132, 5, 84, 28);

	[toolbar addSubview:gEditBtn];

	gSaveBtn		= [NSButton buttonWithTitle:@"Save" target:self
										 action:@selector(saveSource:)];
	gSaveBtn.frame	= NSMakeRect(218, 5, 70, 28);
	gSaveBtn.hidden	= YES;					// shown only while editing

	[toolbar addSubview:gSaveBtn];

	gFolderLabel					= [NSTextField labelWithString:@"No folder open"];

	gFolderLabel.frame				= NSMakeRect(296, 9, cb.size.width - 796, 20);
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

	NSView* codePane				= [[NSView alloc]
		initWithFrame:NSMakeRect(0, 0, 800, split.frame.size.height)];

	gCodeScroll						= [[NSScrollView alloc] initWithFrame:codePane.bounds];

	gCodeScroll.autoresizingMask		= NSViewWidthSizable | NSViewHeightSizable;
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

	EdBuildViews(codePane.bounds);

	[codePane addSubview:gCodeScroll];
	[codePane addSubview:gEditScroll];
	[split addSubview:codePane];
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
