// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
// gui.cpp — Code Annotator C++ port: Win32 GUI and entry point.
//
// Browse a project folder, view code files with syntax colorization, and
// overlay translucent annotations kept in a parallel tree (see core.hpp).
// Pure Win32 + the C++ standard library; no external dependencies.
//
// Usage:  code_annotator.exe [folder] [-a ANN_ROOT]
//		   code_annotator.exe [folder] --check [--fix]		(no GUI)
//
// Compared with the Python original, "insert note as comment" is not
// ported; everything else is, including the in-app source editor.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <imm.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwctype>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core.hpp"
#include "highlight.hpp"
#include "json.hpp"

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "imm32")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using core::Annotation;

// ---------------------------------------------------------------- constants

static const COLORREF BG		= RGB(0x1e, 0x1e, 0x1e);
static const COLORREF BG_PANEL	= RGB(0x25, 0x25, 0x26);
static const COLORREF BG_ACTIVE = RGB(0x37, 0x37, 0x3d);
static const COLORREF FG		= RGB(0xd4, 0xd4, 0xd4);
static const COLORREF FG_DIM	= RGB(0x85, 0x85, 0x85);
static const COLORREF GUTTER_FG = RGB(0x6e, 0x76, 0x81);
static const COLORREF ACCENT	= RGB(0x0e, 0x63, 0x9c);
static const COLORREF BORDER	= RGB(0x3c, 0x3c, 0x3c);
static const COLORREF STATUS_BG = RGB(0x00, 0x7a, 0xcc);

static COLORREF tok_color(hl::Tok t)
{
	switch (t)
	{
		case hl::Tok::Com:	return RGB(0x6a, 0x99, 0x55);
		case hl::Tok::Str:	return RGB(0xce, 0x91, 0x78);
		case hl::Tok::Num:	return RGB(0xb5, 0xce, 0xa8);
		case hl::Tok::Kw:	return RGB(0xc5, 0x86, 0xc0);
		case hl::Tok::Lit:	return RGB(0x56, 0x9c, 0xd6);
		case hl::Tok::Typ:	return RGB(0x4e, 0xc9, 0xb0);
		case hl::Tok::Fn:	return RGB(0xdc, 0xdc, 0xaa);
		case hl::Tok::Tag:	return RGB(0x56, 0x9c, 0xd6);
		case hl::Tok::Attr: return RGB(0x9c, 0xdc, 0xfe);
		default:			return FG;
	}
}

static const wchar_t* SKIP_DIRS[] = {
	L".git",
	L".hg",
	L".svn",
	L"node_modules",
	L"__pycache__",
	L".vs",
	L".idea",
	L"bin",
	L"obj",
	L"dist",
	L"build",
	L".next",
	L"venv",
	L".venv",
	L"target",
	L".mypy_cache",
	L".pytest_cache"};

static const int	SIDECAR_POLL_MS	= 1500;
static const UINT	TIMER_POLL		= 1;
static const UINT	TIMER_FLASH		= 2;
static const UINT	TIMER_ANNFLASH	= 3;

// ---------------------------------------------------------------- app state

struct TreeData
{
	std::wstring	path;
	int				kind;				// 0 dir, 1 file, 2 dummy, 3 error
	std::wstring	base;				// file name without the ● badge
};

struct Match { int ln, col, len; };

struct App {
	HINSTANCE	hinst{};
	HWND		main{};
	HWND		tree{};
	HWND		outline{};
	HWND		code{};
	HWND		findBar{};
	HWND		gotoBar{};
	HWND		findEdit{};
	HWND		gotoEdit{};
	int			dpi				= 96;

	HFONT		fCode{};
	HFONT		fNote{};
	HFONT		fUi{};
	HFONT		fUiSm{};
	HFONT		fUiSmB{};
	HFONT		fTitle{};
	HFONT		fBody{};
	HBRUSH		brBG{};
	HBRUSH		brPanel{};
	int			fontPt			= 11;
	int			charW			=  8;
	int			lineH			= 18;
	int			pad				=  8;
	int			gutterW			= 56;

	core::Workspace	ws;
	bool			wsOpen		= false;
	std::wstring	folderText	= L"No folder open";

	bool			hasFile		= false;
	fs::path		filePath;
	std::vector<std::wstring>			lines;
	std::vector<std::vector<hl::Token>>	toks;
	std::vector<int>					lineCols;
	int				maxCols		= 0;
	int				contentW	= 0;
	int				contentH	= 0;
	std::string		langId		= "plain";
	bool			truncated	= false;
	bool			annError	= false;
	std::vector<Annotation>				anns;
	std::optional<fs::file_time_type>	sidecarTime;
	std::wstring	viewMsg;			// centered message instead of a file
	bool			welcome		= true;

	int				scrollX		= 0;
	int				scrollY		= 0;
	std::optional<std::pair<int, int>>	sel;
	int				selAnchor	= -1;
	bool			dragged		= false;
	int				gutterDrag	= -1;
	std::optional<std::pair<int, int>>	pending;
	int				annIdx		= -1;
	int				flashIdx	= -1;
	std::optional<std::pair<fs::path, Annotation>>	deletedAnn;
	std::vector<std::pair<RECT, int>>				noteRects;		// screen rects, ann index

	std::vector<Match>	matches;
	int				matchIdx	= -1;
	std::wstring	lastQuery;
	std::wstring	findCount;

	bool			showAnns	= true;
	double			opacity		= 55.0;
	std::set<std::string>	visColors;
	bool			sliderDrag	= false;
	bool			splitDrag	= false;
	RECT			rOpen{};
	RECT			rEdit{};
	RECT			rSave{};			// empty unless editing
	RECT			rSlider{};
	RECT			rCheck{};
	RECT			rChips[core::ANN_COLOR_COUNT]{};
	int				treeW		= 300;

	std::wstring	statusFile	= L"Ready";
	std::wstring	statusHint;
	std::wstring	flashMsg;

	std::vector<std::unique_ptr<TreeData>>	treeData;
	std::map<std::wstring, HTREEITEM>		itemByPath;
	fs::path		settingsPath;
};

static App g;

// --------------------------------------------------------- edit mode state

struct EdPos
{
	int ln	= 0;		// 0-based line
	int col	= 0;		// UTF-16 column in the raw (unexpanded) line
};

struct EdOp				// one replacement, enough to undo or redo it
{
	EdPos			at;
	std::wstring	removed;
	std::wstring	inserted;
};

enum class EdKind { Other, Type, Back, Del };

struct EdGroup			// one undo step
{
	std::vector<EdOp>	ops;
	EdPos				caretBefore;
	EdPos				anchorBefore;
	EdPos				caretAfter;
	EdPos				anchorAfter;
	EdKind				kind	= EdKind::Other;
	int					serial	= 0;
};

struct EdBand			// an annotation's region, tracked as [start, end)
{
	EdPos	start;
	EdPos	end;
};

struct Editor
{
	HWND		wnd{};
	bool		active		= false;

	std::vector<std::wstring>				lines;		// raw: tabs kept
	std::vector<std::vector<hl::Token>>		toks;
	std::vector<hl::ScanState>				states;		// scanner state at each line start
	std::vector<int>						visW;		// line widths, tabs expanded
	bool		crlf		= false;
	bool		bom			= false;
	std::optional<fs::file_time_type>		mtime;		// source mtime at enter/last save

	EdPos		caret;
	EdPos		anchor;					// selection is anchor..caret
	int			goalVis		= -1;		// screen column kept by Up/Down
	int			scrollX		= 0;
	int			scrollY		= 0;
	int			gutterW		= 56;
	int			contentW	= 0;
	int			contentH	= 0;

	std::vector<EdBand>		bands;		// parallel to g.anns
	std::vector<EdGroup>	undo;
	std::vector<EdGroup>	redo;
	int			nextSerial	= 1;
	int			savedSerial	= 0;		// undo state id at the last save
	bool		mergeOk		= false;	// next typing may join the last step

	int			mouseMode	= 0;		// 0 none, 1 chars, 2 words, 3 lines
	EdPos		mouseLo;
	EdPos		mouseHi;
};

static Editor ed;

// ------------------------------------------------------------------ helpers

static int S(int v) { return MulDiv(v, g.dpi, 96); }

static COLORREF Blend(COLORREF bg, int r, int gr, int b, double a)
{
	auto mix =
		[a](int base, int c)
		{
			return (int)(base + (c - base) * a + 0.5);
		};

	return	RGB(
				mix(GetRValue(bg), r),
				mix(GetGValue(bg), gr),
				mix(GetBValue(bg), b)
			);
}

static std::wstring W(const std::string& s)		{ return core::utf8_to_wide(s); }
static std::string U8(const std::wstring& s)	{ return core::wide_to_utf8(s); }

static std::wstring collapse_ws(const std::wstring& s)
{
	std::wstring	out;
	bool			word	= false;

	for (wchar_t c : s)
	{
		if (iswspace(c))
		{
			word = false;
			continue;
		}

		if (!word && !out.empty()) out += L' ';

		out		+= c;
		word	= true;
	}

	return out;
}

static std::wstring strip(const std::wstring& s)
{
	size_t a = 0;
	size_t b = s.size();

	while (a < b && iswspace(s[a])) ++a;

	while (b > a && iswspace(s[b - 1])) --b;

	return s.substr(a, b - a);
}

static void FillRectColor(HDC dc, const RECT& r, COLORREF c)
{
	SetBkColor(dc, c);

	ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, L"", 0, nullptr);
}

static void DrawTextColor(
	HDC					dc,
	const std::wstring&	text,
	RECT				r,
	HFONT				font,
	COLORREF			color,
	UINT				fmt)
{
	HGDIOBJ old = SelectObject(dc, font);

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);

	DrawTextW(dc, text.c_str(), -1, &r, fmt | DT_NOPREFIX);

	SelectObject(dc, old);
}

static const core::AnnColor* AnnRGB(const Annotation& a)
{
	const core::AnnColor* c = core::ann_color(a.color);

	return c ? c : &core::ANN_COLORS[0];
}

static int LineCount() { return (int)g.lines.size(); }

// forward declarations
static void DefaultHint();
static void FlashStatus(const std::wstring& text, int ms = 4000);
static void RefreshOutline();
static void RefreshBadge(const fs::path& p);
static void Persist();
static void OpenFile(const fs::path& p);
static void EditAnnotation(
	Annotation	ann,
	bool		isNew,
	int			existingIdx,
	POINT		screenPt
);
static void UpdateScrollbars();
static void LayoutMain();
static void EdGotoLine(int ln, int col = 0);
static void EdOnZoom(double frac);
static bool EdExit();
static bool EdSave();
static void EdToggle();

static void InvalidateCode()
{
	if (g.code) InvalidateRect(g.code, nullptr, FALSE);

	if (ed.wnd) InvalidateRect(ed.wnd, nullptr, FALSE);
}
static void InvalidateBars()
{
	if (g.findBar && IsWindowVisible(g.findBar))
		InvalidateRect(g.findBar, nullptr, FALSE);
}
static void InvalidateChrome() { if (g.main) InvalidateRect(g.main, nullptr, FALSE); }

static void CodeSize(int& w, int& h)
{
	RECT r;

	GetClientRect(g.code, &r);

	w = r.right;
	h = r.bottom;
}
static int CodeViewWidth()
{
	int w, h;

	CodeSize(w, h);

	return std::max(1, w - g.gutterW);
}

// ---------------------------------------------------------- fonts / metrics

static void CreateFonts()
{
	for (HFONT* f :
		{
			&g.fCode,
			&g.fNote,
			&g.fUi,
			&g.fUiSm,
			&g.fUiSmB,
			&g.fTitle,
			&g.fBody
		}
	)
	{
		if (*f) { DeleteObject(*f); *f = nullptr; }
	}

	auto make =
		[](int pt, const wchar_t* face, bool bold, bool italic)
		{
			return CreateFontW(
				-MulDiv(pt, g.dpi, 72),
				0,
				0,
				0,
				bold ? FW_BOLD : FW_NORMAL,
				italic,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH,
				face);
		};

	g.fCode		= make(g.fontPt,					L"Consolas",	false,	false);
	g.fNote		= make(std::max(7, g.fontPt - 2),	L"Segoe UI",	false,	true);
	g.fUi		= make(10,							L"Segoe UI",	false,	false);
	g.fUiSm		= make(9,							L"Segoe UI",	false,	false);
	g.fUiSmB	= make(9,							L"Segoe UI",	true,	false);
	g.fTitle	= make(20,							L"Segoe UI",	false,	false);
	g.fBody		= make(11,							L"Segoe UI",	false,	false);

	HDC			dc	= GetDC(nullptr);
	HGDIOBJ		old	= SelectObject(dc, g.fCode);
	TEXTMETRICW	tm;

	GetTextMetricsW(dc, &tm);

	SIZE sz;

	GetTextExtentPoint32W(dc, L"0", 1, &sz);

	g.charW = sz.cx;
	g.lineH = tm.tmHeight + 4;

	SelectObject(dc, old);

	ReleaseDC(nullptr, dc);
}

static void ComputeContentMetrics()
{
	int n = LineCount();

	g.maxCols = 0;

	g.lineCols.assign((size_t)n, 0);

	for (int i = 0; i < n; ++i)
	{
		g.lineCols[(size_t)i]	= (int)g.lines[(size_t)i].size();
		g.maxCols				= std::max(g.maxCols, g.lineCols[(size_t)i]);
	}

	g.contentW = g.pad * 2 + g.maxCols * g.charW + 300;
	g.contentH = g.pad * 2 + n * g.lineH;

	int digits = (int)std::to_wstring(std::max(1, n)).size();

	g.gutterW = g.pad * 2 + std::max(2, digits) * g.charW;
}

static void ClampScroll()
{
	int w, h;

	CodeSize(w, h);

	g.scrollY = std::max(0, std::min(g.scrollY, g.contentH - h));
	g.scrollX = std::max(0, std::min(g.scrollX, g.contentW - CodeViewWidth()));
}

static void UpdateScrollbars()
{
	int w, h;

	CodeSize(w, h);

	SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};

	si.nMin		= 0;
	si.nMax		= std::max(0, g.hasFile ? g.contentH - 1 : 0);
	si.nPage	= (UINT)h;
	si.nPos		= g.scrollY;

	SetScrollInfo(g.code, SB_VERT, &si, TRUE);

	si.nMax		= std::max(0, g.hasFile ? g.contentW - 1 : 0);
	si.nPage	= (UINT)CodeViewWidth();
	si.nPos		= g.scrollX;

	SetScrollInfo(g.code, SB_HORZ, &si, TRUE);
}

// ------------------------------------------------------------------- status

static void InvalidateStatus() { InvalidateChrome(); }

static void SetHint(const std::wstring& text)
{
	g.statusHint = text;

	InvalidateStatus();
}

static void DefaultHint()
{
	if (!g.flashMsg.empty()) return;

	if (ed.active)
	{
		SetHint(L"Editing • Ctrl+Z undo • Ctrl+Y redo • Ctrl+S save • "
				L"Done returns to the annotate view");

		return;
	}

	std::wstring hint = L"Click or drag on line numbers to annotate • "
						L"click a highlight to edit • Ctrl+F to search";

	size_t count = g.anns.size();

	if (count)
	{
		hint =	std::to_wstring(count) +
					(count == 1 ? L" annotation" : L" annotations") +
					L"	 •  " +
					hint;
	}

	SetHint(hint);
}

static void FlashStatus(const std::wstring& text, int ms) {
	g.flashMsg		= text;
	g.statusHint	= text;

	InvalidateStatus();

	KillTimer(g.main, TIMER_FLASH);

	SetTimer(g.main, TIMER_FLASH, (UINT)ms, nullptr);
}

static void ClearFlash()
{
	KillTimer(g.main, TIMER_FLASH);

	g.flashMsg.clear();

	DefaultHint();
}

// --------------------------------------------------------------- navigation

static void ScrollToLine(int ln)
{
	int w, h;

	CodeSize(w, h);

	int y = g.pad + (ln - 1) * g.lineH;

	if (!(g.scrollY + g.lineH <= y && y <= g.scrollY + h - 2 * g.lineH))
	{
		g.scrollY = std::max(0, y - h / 3);

		ClampScroll();
	}

	UpdateScrollbars();

	InvalidateCode();
}

static bool LineVisible(int ln)
{
	int w, h;

	CodeSize(w, h);

	int y = g.pad + (ln - 1) * g.lineH;

	return g.scrollY <= y && y <= g.scrollY + h - g.lineH;
}

static void ViewportLines(int& top, int& bottom)
{
	int w, h;

	CodeSize(w, h);

	top		= (g.scrollY - g.pad) / g.lineH + 1;
	bottom	= top + h / g.lineH;
}

static void ShowAnnotation(int idx)
{
	if (!g.hasFile || g.anns.empty())
		return;

	idx			= std::max(0, std::min(idx, (int)g.anns.size() - 1));
	g.annIdx	= idx;

	if (ed.active)						// jump the editor caret to its band
	{
		if ((size_t)idx < ed.bands.size())
			EdGotoLine(ed.bands[(size_t)idx].start.ln + 1);

		return;
	}

	const Annotation& a = g.anns[(size_t)idx];

	ScrollToLine(a.start_line);

	g.flashIdx = idx;

	SetTimer(g.main, TIMER_ANNFLASH, 700, nullptr);

	std::wstring note = W(a.text);

	for (auto& c : note)
		if (c == L'\n') c = L' ';

	if (note.size() > 60) note = note.substr(0, 57) + L"...";

	std::wstring text =	L"Annotation " +
						std::to_wstring(idx + 1) +
						L" of " +
						std::to_wstring(g.anns.size());

	if (!note.empty()) text += L"  •	" + note;

	SetHint(text);

	InvalidateCode();
}

static void GotoAnnotation(int step)
{
	if (ed.active || !g.hasFile || g.anns.empty())
		return;

	int	n	= (int)g.anns.size();
	int	idx	= g.annIdx;

	if (idx >= 0 && idx < n && LineVisible(g.anns[(size_t)idx].start_line))
	{
		idx = ((idx + step) % n + n) % n;
	}
	else
	{
		// lost track (scrolled away, file changed): pick relative to the view
		int top, bottom;

		ViewportLines(top, bottom);

		if (step > 0)
		{
			idx = 0;

			for (int i = 0; i < n; ++i)
			{
				if (g.anns[(size_t)i].start_line > top)
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
				if (g.anns[(size_t)i].start_line < bottom)
				{
					idx = i;

					break;
				}
			}
		}
	}

	ShowAnnotation(idx);
}

// --------------------------------------------------------------- find / goto

static std::wstring GetWindowString(HWND w)
{
	int n = GetWindowTextLengthW(w);

	std::wstring s((size_t)n, L'\0');

	if (n) GetWindowTextW(w, s.data(), n + 1);

	return s;
}

static void PositionBars()
{
	RECT rc;

	GetWindowRect(g.code, &rc);

	MapWindowPoints(nullptr, g.main, (POINT*)&rc, 2);

	int fw = S(316);
	int fh = S(34);

	SetWindowPos(
		g.findBar,
		HWND_TOP,
		rc.right - fw - S(24),
		rc.top + S(8),
		fw,
		fh,
		SWP_NOACTIVATE);

	int gw = S(160);

	SetWindowPos(
		g.gotoBar,
		HWND_TOP,
		rc.right - gw - S(24),
		rc.top + S(8),
		gw,
		fh,
		SWP_NOACTIVATE);
}

static void GotoMatch(int idx)
{
	if (g.matches.empty()) return;

	int n = (int)g.matches.size();

	g.matchIdx = ((idx % n) + n) % n;

	const Match& m = g.matches[(size_t)g.matchIdx];

	g.findCount =	std::to_wstring(g.matchIdx + 1) +
					L" of " +
					std::to_wstring(n);

	if (ed.active)
	{
		EdGotoLine(m.ln, m.col);
		InvalidateBars();

		return;
	}

	ScrollToLine(m.ln);

	int	x0		= g.pad + m.col * g.charW;
	int	viewW	= CodeViewWidth();

	if (!(g.scrollX <= x0 && x0 <= g.scrollX + viewW - 80))
	{
		g.scrollX = std::max(0, x0 - viewW / 3);

		ClampScroll();
	}

	UpdateScrollbars();

	InvalidateCode();

	InvalidateBars();
}

// jump=false (refresh after an edit) recounts without moving the caret
static void FindRefresh(bool jump = true)
{
	std::wstring query = GetWindowString(g.findEdit);

	g.lastQuery = query;

	g.matches.clear();

	g.matchIdx = -1;

	if (!query.empty() && g.hasFile)
	{
		std::wstring q = query;

		std::transform(q.begin(), q.end(), q.begin(), towlower);

		const auto& src = ed.active ? ed.lines : g.lines;

		for (int row = 0; row < (int)src.size() && g.matches.size() < 2000; ++row)
		{
			std::wstring low = src[(size_t)row];

			std::transform(low.begin(), low.end(), low.begin(), towlower);

			size_t start = 0;

			while (g.matches.size() < 2000)
			{
				size_t col = low.find(q, start);

				if (col == std::wstring::npos) break;

				g.matches.push_back({row + 1, (int)col, (int)query.size()});

				start = col + q.size();
			}
		}
	}

	if (!g.matches.empty() && jump)
	{
		GotoMatch(0);
	}
	else if (!g.matches.empty())
	{
		g.findCount =	std::to_wstring(g.matches.size()) +
						(g.matches.size() == 1 ? L" match" : L" matches");

		InvalidateBars();

		InvalidateCode();
	}
	else
	{
		g.findCount = query.empty() ? L"" : L"no results";

		InvalidateBars();

		InvalidateCode();
	}
}

static void FindNext() { if (!g.matches.empty()) GotoMatch(g.matchIdx + 1); }
static void FindPrev() { if (!g.matches.empty()) GotoMatch(g.matchIdx - 1); }

static void OpenFindBar()
{
	if (!g.hasFile) return;

	ShowWindow(g.gotoBar, SW_HIDE);

	PositionBars();

	ShowWindow(g.findBar, SW_SHOW);

	SetFocus(g.findEdit);

	SendMessageW(g.findEdit, EM_SETSEL, 0, -1);

	FindRefresh();
}

static void OpenGotoBar()
{
	if (!g.hasFile) return;

	ShowWindow(g.findBar, SW_HIDE);

	PositionBars();

	ShowWindow(g.gotoBar, SW_SHOW);

	SetWindowTextW(g.gotoEdit, L"");

	SetFocus(g.gotoEdit);
}

static bool BarsVisible()
{
	return IsWindowVisible(g.findBar) || IsWindowVisible(g.gotoBar);
}

// Escape: dismiss find/goto bars, selection, and matches
static void EscapeAction()
{
	if (ed.active)
	{
		ShowWindow(g.findBar, SW_HIDE);
		ShowWindow(g.gotoBar, SW_HIDE);

		g.matches.clear();

		g.matchIdx = -1;

		g.lastQuery.clear();

		SetFocus(ed.wnd);

		InvalidateCode();

		return;
	}

	if (!(BarsVisible() || g.sel || !g.matches.empty())) return;

	ShowWindow(g.findBar, SW_HIDE);

	ShowWindow(g.gotoBar, SW_HIDE);

	g.matches.clear();

	g.matchIdx = -1;

	g.lastQuery.clear();
	g.sel.reset();

	DefaultHint();

	InvalidateCode();

	SetFocus(g.code);
}

static void GotoLineAction()
{
	std::wstring	text	= strip(GetWindowString(g.gotoEdit));
	int				ln		= 0;

	try
	{
		ln = std::stoi(U8(text));
	}
	catch (...)
	{
		return;
	}

	if (!g.hasFile) return;

	if (ed.active)
	{
		ShowWindow(g.gotoBar, SW_HIDE);

		EdGotoLine(std::max(1, std::min((int)ed.lines.size(), ln)));

		SetFocus(ed.wnd);

		return;
	}

	ln = std::max(1, std::min(LineCount(), ln));

	ShowWindow(g.gotoBar, SW_HIDE);

	ScrollToLine(ln);

	g.sel = {ln, ln};

	SetHint(L"1 line selected  •	Ctrl+C to copy");

	SetFocus(g.code);

	InvalidateCode();
}

// ------------------------------------------------------------------- zoom

static void Zoom(int delta)
{
	int	cur	= g.fontPt;
	int	pt	= delta == 0 ? 11 : std::max(6, std::min(28, cur + delta));

	if (pt == cur) return;

	double frac = g.contentH > 0 ? (double)g.scrollY / g.contentH : 0.0;

	double edFrac = ed.contentH > 0 ? (double)ed.scrollY / ed.contentH : 0.0;

	g.fontPt = pt;

	CreateFonts();

	if (ed.active) EdOnZoom(edFrac);

	if (g.hasFile)
	{
		ComputeContentMetrics();

		g.scrollY = (int)(frac * g.contentH);

		ClampScroll();

		UpdateScrollbars();
	}

	InvalidateCode();
}

// ---------------------------------------------------------------- clipboard

static bool FocusInEdit()
{
	HWND f = GetFocus();

	if (!f) return false;

	wchar_t cls[16];

	return GetClassNameW(f, cls, 16) && _wcsicmp(cls, L"Edit") == 0;
}

static void CopySelection()
{
	if (FocusInEdit() || !g.hasFile || !g.sel)
		return;

	auto			[lo, hi]	= *g.sel;
	std::wstring	text;

	for (int i = lo; i <= hi && i <= LineCount(); ++i)
	{
		if (i > lo) text += L"\r\n";

		text += g.lines[(size_t)i - 1];
	}

	if (!OpenClipboard(g.main)) return;

	EmptyClipboard();

	size_t	bytes	= (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL	mem		= GlobalAlloc(GMEM_MOVEABLE, bytes);

	if (mem)
	{
		memcpy(GlobalLock(mem), text.c_str(), bytes);

		GlobalUnlock(mem);

		SetClipboardData(CF_UNICODETEXT, mem);
	}

	CloseClipboard();

	int n = hi - lo + 1;

	FlashStatus(
		L"Copied " + std::to_wstring(n) +
		(n == 1 ? L" line" : L" lines") +
		L" to clipboard");
}

// -------------------------------------------------------------- persistence

static void RefreshOutline()
{
	SendMessageW(g.outline, LB_RESETCONTENT, 0, 0);

	for (const auto& a : g.anns)
	{
		std::wstring rng = std::to_wstring(a.start_line);

		if (a.end_line != a.start_line)
			rng += L"–" + std::to_wstring(a.end_line);

		std::wstring note = collapse_ws(W(a.text));

		if (note.empty()) note = L"(no text)";

		if (note.size() > 60) note = note.substr(0, 60);

		std::wstring item = L" ";

		if (a.orphaned) item += L"⚠ ";

		item += rng + L"  " + note;

		SendMessageW(g.outline, LB_ADDSTRING, 0, (LPARAM)item.c_str());
	}
}

static void Persist()
{
	if (g.annError)
	{
		std::wstring msg =
			L"This file's annotation file exists but could not be parsed:\n" +
			g.ws.sidecar_for(g.filePath).wstring() +
			L"\n\nSaving is disabled so its contents are not overwritten.\n"
			L"Fix or delete it, then reopen the file.";

		MessageBoxW(g.main, msg.c_str(), L"Annotations locked", MB_ICONERROR);

		return;
	}

	if (!g.ws.save(g.filePath, g.anns))
	{
		MessageBoxW(
			g.main,
			L"Could not write the annotation file.",
			L"Save failed",
			MB_ICONERROR);

		return;
	}

	RefreshBadge(g.filePath);

	RefreshOutline();

	std::error_code ec;

	auto t = fs::last_write_time(g.ws.sidecar_for(g.filePath), ec);

	g.sidecarTime = ec ? std::nullopt : std::optional(t);
}

static void UndoDelete()
{
	if (ed.active || FocusInEdit() || !g.deletedAnn) return;

	auto& [path, ann] = *g.deletedAnn;

	if (path != g.filePath || g.annError) return;

	g.anns.push_back(ann);

	core::sort_annotations(g.anns);

	g.deletedAnn.reset();

	Persist();

	InvalidateCode();

	FlashStatus(L"Annotation restored");
}

// live-reload annotations when the sidecar changes outside the app
static void PollSidecar()
{
	if (!g.hasFile || g.annError || ed.active) return;

	fs::path							sidecar	= g.ws.sidecar_for(g.filePath);
	std::error_code						ec;
	auto								t		= fs::last_write_time(sidecar, ec);
	std::optional<fs::file_time_type>	mtime	=
		ec ? std::nullopt : std::optional(t);

	if (mtime == g.sidecarTime) return;

	g.sidecarTime = mtime;

	core::AnnLoad la = core::load_annotations(sidecar);

	if (!la.ok)
	{
		g.annError = true;
		g.anns.clear();

		RefreshOutline();

		InvalidateCode();

		FlashStatus(
			L"Annotation file became unreadable — annotating disabled",
			8000);

		return;
	}

	g.anns = std::move(la.anns);

	// heal in memory only: don't write back while another tool is writing
	core::reconcile_annotations(g.anns, g.lines);

	g.annIdx = -1;

	RefreshOutline();

	RefreshBadge(g.filePath);

	InvalidateCode();

	FlashStatus(L"Annotations reloaded (changed outside the app)");
}

// -------------------------------------------------------------- folder tree

static TreeData* DataOf(HTREEITEM item)
{
	TVITEMW tvi{};

	tvi.mask	= TVIF_PARAM;
	tvi.hItem	= item;

	if (!TreeView_GetItem(g.tree, &tvi)) return nullptr;

	return (TreeData*)tvi.lParam;
}

static HTREEITEM InsertNode(
	HTREEITEM parent,
	const std::wstring& text,
	std::wstring path,
	int kind,
	std::wstring base = L"")
{
	auto data = std::make_unique<TreeData>();

	data->path			= std::move(path);
	data->kind			= kind;
	data->base			= std::move(base);

	TVINSERTSTRUCTW ins{};

	ins.hParent			= parent;
	ins.hInsertAfter	= TVI_LAST;
	ins.item.mask		= TVIF_TEXT | TVIF_PARAM;
	ins.item.pszText	= const_cast<wchar_t*>(text.c_str());
	ins.item.lParam		= (LPARAM)data.get();

	g.treeData.push_back(std::move(data));

	return TreeView_InsertItem(g.tree, &ins);
}

static void PopulateDir(HTREEITEM item, const fs::path& dir)
{
	std::error_code			ec;
	std::vector<fs::path>	dirs;
	std::vector<fs::path>	files;
	fs::directory_iterator	it(dir, ec), end;

	if (ec)
	{
		InsertNode(item, L" ⚠ " + W(ec.message()), L"", 3);

		return;
	}

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
			return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
		};

	std::sort(dirs.begin(), dirs.end(), by_name);
	std::sort(files.begin(), files.end(), by_name);

	fs::path ann_abs = fs::absolute(g.ws.ann_root, ec).lexically_normal();

	for (const auto& d : dirs)
	{
		std::wstring	name	= d.filename().wstring();
		bool			skip	= false;

		for (const wchar_t* sd : SKIP_DIRS)
		{
			if (name == sd)
			{
				skip = true;

				break;
			}
		}

		if (skip) continue;

		if (fs::absolute(d, ec).lexically_normal() == ann_abs)
			continue;				// keep the annotation tree out of the browser

		HTREEITEM child = InsertNode(item, name, d.wstring(), 0);

		InsertNode(child, L"…", L"", 2);	 // dummy: filled lazily
	}

	for (const auto& f : files)
	{
		std::wstring	name	= f.filename().wstring();
		bool			badge	= fs::is_regular_file(g.ws.sidecar_for(f), ec);
		std::wstring	abs		= fs::absolute(f, ec).lexically_normal().wstring();
		HTREEITEM		child	= InsertNode(
			item,
			badge ? name + L" ●" : name,
			abs,
			1,
			name
		);

		g.itemByPath[abs] = child;
	}
}

static void ShowFolder()
{
	TreeView_DeleteAllItems(g.tree);

	g.treeData.clear();
	g.itemByPath.clear();

	std::wstring name = g.ws.folder.filename().wstring();

	if (name.empty()) name = g.ws.folder.wstring();

	HTREEITEM root = InsertNode(TVI_ROOT, name, g.ws.folder.wstring(), 0);

	PopulateDir(root, g.ws.folder);

	TreeView_Expand(g.tree, root, TVE_EXPAND);
}

static void RefreshBadge(const fs::path& p)
{
	std::error_code	ec;
	std::wstring	abs	= fs::absolute(p, ec).lexically_normal().wstring();
	auto			it	= g.itemByPath.find(abs);

	if (it == g.itemByPath.end()) return;

	TreeData* data = DataOf(it->second);

	if (!data) return;

	bool			badge = fs::is_regular_file(g.ws.sidecar_for(p), ec);
	std::wstring	text = badge ? data->base + L" ●" : data->base;
	TVITEMW			tvi{};

	tvi.mask	= TVIF_TEXT;
	tvi.hItem	= it->second;
	tvi.pszText	= const_cast<wchar_t*>(text.c_str());

	TreeView_SetItem(g.tree, &tvi);
}

static void OpenFolder(const fs::path& folder)
{
	if (!EdExit()) return;				// user kept editing

	if (g.ws.ann_root_arg.empty() && g.wsOpen)
	{ /* keep -a override */ }

	g.ws.open(folder);

	g.wsOpen		= true;
	g.folderText	=	g.ws.folder.wstring() +
						L"	•	  annotations: " +
						g.ws.ann_root.wstring();

	ShowFolder();

	InvalidateChrome();
}

static void PickFolder()
{
	IFileOpenDialog* dlg = nullptr;

	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
								nullptr,
								CLSCTX_ALL,
								IID_PPV_ARGS(&dlg))))
	{
		return;
	}

	DWORD opts = 0;

	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dlg->SetTitle(L"Open project folder");

	if (SUCCEEDED(dlg->Show(g.main)))
	{
		IShellItem* item = nullptr;

		if (SUCCEEDED(dlg->GetResult(&item)))
		{
			wchar_t* path = nullptr;

			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				OpenFolder(path);

				CoTaskMemFree(path);
			}

			item->Release();
		}
	}

	dlg->Release();
}

// -------------------------------------------------------------- file view

static void ShowMessageView(const std::wstring& msg)
{
	g.hasFile = false;

	g.filePath.clear();
	g.lines.clear();
	g.toks.clear();
	g.anns.clear();
	g.matches.clear();

	g.matchIdx	= -1;
	g.annIdx	= -1;
	g.flashIdx	= -1;

	g.sel.reset();
	g.pending.reset();

	g.annError = false;

	g.sidecarTime.reset();

	g.viewMsg		= msg;
	g.welcome		= false;
	g.statusFile	= L"Ready";

	RefreshOutline();

	UpdateScrollbars();

	InvalidateCode();

	InvalidateStatus();
}

static void OpenFile(const fs::path& path)
{
	std::error_code	ec;
	uintmax_t		size	= fs::file_size(path, ec);
	std::wstring	name	= path.filename().wstring();

	if (!ec && size > core::MAX_FILE_BYTES)
	{
		wchar_t buf[64];

		swprintf(
			buf,
			64,
			L" is too large to display (%.1f MB).",
			size / 1048576.0
		);

		ShowMessageView(name + buf);

		return;
	}

	auto bytes = core::read_file_bytes(path);

	if (!bytes)
	{
		ShowMessageView(L"Could not open file: " + name);

		return;
	}

	size_t head = std::min<size_t>(bytes->size(), 8192);

	if (bytes->find('\0') < head)
	{
		ShowMessageView(name + L" looks like a binary file.");

		return;
	}

	std::vector<std::wstring>	lines		= core::decode_source(*bytes);
	bool						truncated	= (int)lines.size() > core::MAX_LINES;

	if (truncated) lines.resize(core::MAX_LINES);

	g.truncated	= truncated;
	g.hasFile	= true;
	g.welcome	= false;

	g.viewMsg.clear();

	g.filePath	= fs::absolute(path, ec).lexically_normal();
	g.lines		= std::move(lines);

	fs::path		sidecar		= g.ws.sidecar_for(g.filePath);
	int				repaired	= 0;
	int				lost		= 0;

	core::AnnLoad	la			= core::load_annotations(sidecar);

	if (la.ok)
	{
		g.anns		= std::move(la.anns);
		g.annError	= false;

		if (!truncated)
		{
			auto [r, l] = core::reconcile_annotations(g.anns, g.lines);

			repaired	= r;
			lost		= l;

			if (repaired || lost)
				Persist();			// keep the healed positions
		}
	}
	else
	{
		g.anns.clear();

		g.annError = true;
	}

	auto t = fs::last_write_time(sidecar, ec);

	g.sidecarTime	= ec ? std::nullopt : std::optional(t);
	g.annIdx		= -1;
	g.flashIdx		= -1;

	g.sel.reset();
	g.pending.reset();

	g.selAnchor		= -1;
	g.gutterDrag	= -1;

	g.deletedAnn.reset();

	g.langId		= hl::lang_for(g.filePath.filename().wstring());
	g.toks			= hl::highlight_lines(g.lines, g.langId);

	ComputeContentMetrics();

	g.scrollX = g.scrollY = 0;

	UpdateScrollbars();

	if (IsWindowVisible(g.findBar))
		FindRefresh();
	else
	{
		g.matches.clear();

		g.matchIdx = -1;
	}

	RefreshOutline();

	g.statusFile = name;

	if (g.langId != "plain")
	{
		std::wstring lang = W(g.langId);

		std::transform(lang.begin(), lang.end(), lang.begin(), towupper);

		g.statusFile += L"	•	 " + lang;
	}

	if (truncated)
		g.statusFile +=	L"	•	 showing first " +
						std::to_wstring(core::MAX_LINES) +
						L" lines";

	if (g.annError)
	{
		FlashStatus(L"Annotation file is unreadable — annotating is "
					L"disabled for this file until it is fixed or deleted",
					8000);
	}
	else if (repaired || lost)
	{
		std::wstring parts;

		if (repaired)
			parts = std::to_wstring(repaired) + L" annotation(s) re-anchored to moved code";

		if (lost)
		{
			if (!parts.empty()) parts += L" • ";

			parts	+=	std::to_wstring(lost) +
						L" annotation(s) orphaned — their anchored code is gone";
		}

		FlashStatus(parts, 6000);
	}
	else
	{
		DefaultHint();
	}

	InvalidateCode();

	InvalidateStatus();
}

// --------------------------------------------------------- annotation dialog

struct DlgState
{
	Annotation*		ann;
	bool			isNew;
	std::wstring	title;
	std::string		color;
	HWND			wnd{};
	HWND			edit{};
	bool			done	= false;
	int				result	= 0;				// 0 cancel, 1 save, 2 delete
	RECT			rSw[core::ANN_COLOR_COUNT]{};
	RECT			rSave{};
	RECT			rDel{};
	RECT			rCancel{};
};

static void DlgReadEdit(DlgState& st)
{
	std::wstring text = GetWindowString(st.edit);
	std::wstring norm;

	for (size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] == L'\r')
		{
			if (i + 1 < text.size() && text[i + 1] == L'\n')
				++i;

			norm += L'\n';
		}
		else
		{
			norm += text[i];
		}
	}

	st.ann->text	= U8(strip(norm));
	st.ann->color	= st.color;
}

static LRESULT CALLBACK DlgProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	DlgState* st = (DlgState*)GetWindowLongPtrW(w, GWLP_USERDATA);

	switch (msg) {
		case WM_NCCREATE:
			SetWindowLongPtrW(
				w,
				GWLP_USERDATA,
				(LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);

			return DefWindowProcW(w, msg, wp, lp);

		case WM_CREATE:
		{
			st = (DlgState*)GetWindowLongPtrW(w, GWLP_USERDATA);

			RECT rc;

			GetClientRect(w, &rc);

			std::wstring text = W(st->ann->text);
			std::wstring crlf;

			for (wchar_t c : text)
			{
				if (c == L'\n')
					crlf += L"\r\n";
				else
					crlf += c;
			}

			st->edit = CreateWindowExW(
				0,
				L"EDIT",
				crlf.c_str(),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
				S(10),
				S(30),
				rc.right - S(20),
				S(78),
				w,
				nullptr,
				g.hinst,
				nullptr);

			SendMessageW(st->edit, WM_SETFONT, (WPARAM)g.fUi, TRUE);

			// layout rects
			int y = S(116);

			for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
			{
				st->rSw[i] =
					{
						S(10) + i * S(26),
						y,
						S(10) + i * S(26) + S(22),
						y + S(22)
					};
			}

			int	by		= S(146), bh = S(26);
			int	inner	= rc.right - S(20);
			int	nbtn	= st->isNew ? 2 : 3;
			int	bw		= (inner - S(8) * (nbtn - 1)) / nbtn;
			int	x		= S(10);

			st->rSave = { x, by, x + bw, by + bh };

			x += bw + S(8);

			if (!st->isNew)
			{
				st->rDel	= { x, by, x + bw, by + bh };
				x			+= bw + S(8);
			}

			st->rCancel = {x, by, rc.right - S(10), by + bh};

			return 0;
		}

		case WM_CTLCOLOREDIT:
		{
			HDC dc = (HDC)wp;

			SetTextColor(dc, FG);
			SetBkColor(dc, BG);

			return (LRESULT)g.brBG;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT	ps;
			HDC			dc	= BeginPaint(w, &ps);
			RECT		rc;

			GetClientRect(w, &rc);
			FillRectColor(dc, rc, BG_PANEL);

			HBRUSH border = CreateSolidBrush(BORDER);

			FrameRect(dc, &rc, border);
			DeleteObject(border);

			RECT rt = {S(10), S(8), rc.right - S(10), S(28)};

			DrawTextColor(dc, st->title, rt, g.fUiSmB, FG_DIM, DT_LEFT);

			// edit border
			RECT re = {S(9), S(29), rc.right - S(9), S(30) + S(78) + 1};

			FrameRect(dc, &re, (HBRUSH)GetStockObject(GRAY_BRUSH));

			// swatches
			for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
			{
				const auto&	c	= core::ANN_COLORS[i];
				HBRUSH		b	= CreateSolidBrush(RGB(c.r, c.g, c.b));
				HGDIOBJ		ob	= SelectObject(dc, b);
				HGDIOBJ		op	= SelectObject(dc, GetStockObject(NULL_PEN));
				RECT		r	= st->rSw[i];

				Ellipse(dc, r.left + S(3), r.top + S(3), r.right - S(3), r.bottom - S(3));

				if (st->color == c.name)
				{
					HPEN ring = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));

					SelectObject(dc, ring);
					SelectObject(dc, GetStockObject(NULL_BRUSH));

					Ellipse(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);

					SelectObject(dc, op);
					DeleteObject(ring);
				}

				SelectObject(dc, ob);
				SelectObject(dc, op);

				DeleteObject(b);
			}

			// buttons
			auto button =
				[&](const RECT& r, const wchar_t* label, COLORREF bg, COLORREF fg)
				{
					if (r.right == r.left) return;

					FillRectColor(dc, r, bg);

					DrawTextColor(
						dc,
						label,
						r,
						g.fUiSm,
						fg,
						DT_CENTER | DT_VCENTER | DT_SINGLELINE
					);
				};

			button(st->rSave, L"Save", ACCENT, RGB(255, 255, 255));

			if (!st->isNew)
				button(st->rDel, L"Delete", RGB(0x6e, 0x20, 0x20), RGB(0xf0, 0xb0, 0xb0));
			button(st->rCancel, L"Cancel", BG_ACTIVE, FG);

			EndPaint(w, &ps);

			return 0;
		}

		case WM_LBUTTONDOWN:
		{
			POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

			for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
			{
				if (PtInRect(&st->rSw[i], pt))
				{
					st->color = core::ANN_COLORS[i].name;

					InvalidateRect(w, nullptr, FALSE);

					return 0;
				}
			}

			if (PtInRect(&st->rSave, pt))
			{
				DlgReadEdit(*st);

				st->result	= 1;
				st->done	= true;
			}
			else if (!st->isNew && PtInRect(&st->rDel, pt))
			{
				st->result	= 2;
				st->done	= true;
			}
			else if (PtInRect(&st->rCancel, pt))
			{
				st->result	= 0;
				st->done	= true;
			}

			return 0;
		}

		case WM_CLOSE:
			if (st)
			{
				st->result	= 0;
				st->done	= true;
			}

			return 0;
	}

	return DefWindowProcW(w, msg, wp, lp);
}

// returns 0 cancel, 1 save, 2 delete; on save the annotation text/color are
// already updated
static int RunAnnotationDialog(Annotation& ann, bool isNew, POINT pt)
{
	DlgState st;

	st.ann		= &ann;
	st.isNew	= isNew;
	st.color	= ann.color;

	std::wstring rng =
		ann.start_line == ann.end_line ?
			L"line " + std::to_wstring(ann.start_line) :
			L"lines " + std::to_wstring(ann.start_line) + L"–" + std::to_wstring(ann.end_line);

	st.title = (isNew ? L"New annotation — " : L"Edit annotation — ") + rng;

	int			w	= S(380), h = S(184);
	MONITORINFO	mi{sizeof mi};

	GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);

	int x = std::max((int)mi.rcWork.left,
					 std::min((int)pt.x, (int)mi.rcWork.right - w - 10));
	int y = std::max((int)mi.rcWork.top,
					 std::min((int)pt.y + 14, (int)mi.rcWork.bottom - h - 10));

	st.wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"CADlg", L"", WS_POPUP, x, y,
							 w, h, g.main, nullptr, g.hinst, &st);

	EnableWindow(g.main, FALSE);

	ShowWindow(st.wnd, SW_SHOW);

	SetFocus(st.edit);

	MSG m;

	while (!st.done && GetMessageW(&m, nullptr, 0, 0))
	{
		if (m.message == WM_KEYDOWN &&
			(m.hwnd == st.wnd || IsChild(st.wnd, m.hwnd)))
		{
			if (m.wParam == VK_ESCAPE)
			{
				st.result	= 0;
				st.done		= true;

				continue;
			}
			if (m.wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0)
			{
				DlgReadEdit(st);

				st.result	= 1;
				st.done		= true;

				continue;
			}
		}

		TranslateMessage(&m);
		DispatchMessageW(&m);

		if (m.message == WM_QUIT)
		{
			PostQuitMessage((int)m.wParam);

			break;
		}
	}

	EnableWindow(g.main, TRUE);
	DestroyWindow(st.wnd);

	SetActiveWindow(g.main);

	SetFocus(g.code);

	return st.result;
}

// dialog + post-processing for both new and existing annotations.
// existingIdx is the index into g.anns for edits, or -1 for a new one.
static void EditAnnotation(
	Annotation	ann,
	bool		isNew,
	int			existingIdx,
	POINT		screenPt)
{
	int result = RunAnnotationDialog(ann, isNew, screenPt);

	g.pending.reset();

	if (result == 1)
	{
		ann.orphaned = false;		// user re-confirmed this location

		if (ann.start_line >= 1 && ann.start_line <= LineCount())
		{
			ann.anchor = core::anchor_text(g.lines[(size_t)ann.start_line - 1]);
		}

		if (isNew)
		{
			g.anns.push_back(std::move(ann));
		}
		else if (existingIdx >= 0 && existingIdx < (int)g.anns.size())
		{
			g.anns[(size_t)existingIdx] = std::move(ann);
		}

		core::sort_annotations(g.anns);

		g.annIdx = -1;

		Persist();

		DefaultHint();
	}
	else if (result == 2 && !isNew)
	{
		g.anns.erase(
			std::remove_if(
				g.anns.begin(),
				g.anns.end(),
				[&](const Annotation& a)
				{
					return a.id == ann.id;
				}
			),
			g.anns.end()
		);

		g.annIdx		= -1;
		g.deletedAnn	= {g.filePath, ann};

		Persist();

		FlashStatus(L"Annotation deleted — Ctrl+Z restores", 6000);
	}
	else
	{
		DefaultHint();
	}

	InvalidateCode();
}

// ------------------------------------------------------------ code painting

static void PaintWelcome(HDC dc, int cw, int ch)
{
	int		cx	= std::max(S(360), cw / 2);
	RECT	rt	= {cx - S(400), S(90), cx + S(400), S(150)};

	DrawTextColor(dc, L"Code Annotator", rt, g.fTitle, FG,
				  DT_CENTER | DT_SINGLELINE);

	std::wstring	body	=
		L"Open a project folder, pick a code file, and see its\n"
		L"annotations overlaid transparently on the source.\n\n"
		L"Click or drag on the line numbers to add an annotation.\n"
		L"Click an existing highlight to edit it, or use ✎ Edit\n"
		L"to change the source itself.\n\n"
		L"Ctrl+F finds text, Ctrl+G jumps to a line, Ctrl+C copies\n"
		L"selected lines, Ctrl+wheel or Ctrl+=/- zooms the code,\n"
		L"Alt+Down/Alt+Up jump to the next/previous annotation.\n\n"
		L"Notes are saved in a parallel tree that mirrors the\n"
		L"source layout (default: <project>/.annotations),\n"
		L"so annotating never touches the source files.";
	RECT			rb		= {cx - S(400), S(160), cx + S(400), ch};

	DrawTextColor(dc, body, rb, g.fBody, FG_DIM, DT_CENTER);
}

static void PaintCode(HDC dc, int cw, int ch)
{
	RECT all = {0, 0, cw, ch};

	FillRectColor(dc, all, BG);

	SetBkMode(dc, TRANSPARENT);

	g.noteRects.clear();

	if (!g.hasFile)
	{
		if (g.welcome)
		{
			PaintWelcome(dc, cw, ch);
		}
		else if (!g.viewMsg.empty())
		{
			RECT r = {std::max(S(360), cw / 2) - S(400), S(100),
					  std::max(S(360), cw / 2) + S(400), S(160)};

			DrawTextColor(
				dc,
				g.viewMsg,
				r,
				g.fBody,
				FG_DIM,
				DT_CENTER | DT_SINGLELINE);
		}

		return;
	}

	int		n		= LineCount();
	int		lh		= g.lineH;
	int		r0		= std::max(0, (g.scrollY - g.pad) / lh);
	int		r1		= std::min(n - 1, (g.scrollY + ch - g.pad) / lh + 1);

	// gutter line numbers
	HGDIOBJ	oldFont	= SelectObject(dc, g.fCode);

	SetTextColor(dc, GUTTER_FG);
	SetTextAlign(dc, TA_RIGHT | TA_TOP);

	for (int row = r0; row <= r1; ++row)
	{
		std::wstring num = std::to_wstring(row + 1);

		ExtTextOutW(dc,
					g.gutterW - g.pad,
					g.pad + row * lh - g.scrollY + 2,
					0,
					nullptr,
					num.c_str(),
					(UINT)num.size(),
					nullptr);
	}

	SetTextAlign(dc, TA_LEFT | TA_TOP);

	SaveDC(dc);

	IntersectClipRect(dc, g.gutterW, 0, cw, ch);

	int		bandL	= g.gutterW - g.scrollX;
	int		bandR	= bandL + g.contentW;
	int		xOff	= g.gutterW + g.pad - g.scrollX;
	double	alpha	= g.opacity / 100.0;

	auto	lineTop	=
		[&](int ln)
		{
			return g.pad + (ln - 1) * lh - g.scrollY;
		};

	// selection band (under everything else, like the canvas ordering)
	if (g.sel)
	{
		RECT r = {bandL, lineTop(g.sel->first), bandR, lineTop(g.sel->second + 1)};

		FillRectColor(dc, r, Blend(BG, 130, 160, 190, 0.16));
	}

	// pending gutter-drag band
	if (g.pending)
	{
		RECT r = {bandL, lineTop(g.pending->first), bandR, lineTop(g.pending->second + 1)};

		FillRectColor(dc, r, Blend(BG, 14, 99, 156, 0.35));
	}

	// annotation bands
	if (g.showAnns)
	{
		for (const auto& a : g.anns)
		{
			if (!g.visColors.count(a.color)) continue;

			const auto*	c		= AnnRGB(a);
			int			y0		= lineTop(a.start_line);
			int			y1		= lineTop(std::min(a.end_line, n) + 1);

			if (y1 < -lh || y0 > ch + lh) continue;

			double		f		= a.orphaned ? 0.18 : 0.45;
			RECT		r		= {bandL, y0, bandR, y1};

			FillRectColor(dc, r, Blend(BG, c->r, c->g, c->b, alpha * f));

			COLORREF	edge	=
				Blend(BG, c->r, c->g, c->b, std::min(1.0, alpha * 1.5));

			if (a.orphaned)		// dashed edge marks a best-guess spot
			{
				for (int y = y0 + 1; y < y1 - 1; y += 6)
				{
					RECT rd = {bandL + 1, y, bandL + 4, std::min(y + 3, y1 - 1)};

					FillRectColor(dc, rd, edge);
				}
			}
			else
			{
				RECT re = {bandL, y0, bandL + 3, y1};

				FillRectColor(dc, re, edge);
			}
		}
	}

	// find-match highlights: over the bands, under the code text
	for (int i = 0; i < (int)g.matches.size(); ++i)
	{
		const Match&	m	= g.matches[(size_t)i];
		int				y0	= lineTop(m.ln);

		if (y0 < -lh || y0 > ch) continue;

		int				x0	= xOff + m.col * g.charW;
		bool			cur	= i == g.matchIdx;
		RECT			r	=
			{
				x0 - 1,
				y0 + 1,
				x0 + m.len * g.charW + 1,
				y0 + lh - 1
			};

		FillRectColor(dc, r, Blend(BG, 210, 170, 60, cur ? 0.5 : 0.25));

		if (cur)
		{
			HBRUSH b = CreateSolidBrush(RGB(0xd7, 0xba, 0x7d));

			FrameRect(dc, &r, b);

			DeleteObject(b);
		}
	}

	// code text
	SelectObject(dc, g.fCode);

	for (int row = r0; row <= r1; ++row)
	{
		int y = g.pad + row * lh - g.scrollY + 2;
		for (const auto& t : g.toks[(size_t)row])
		{
			if (t.cls == hl::Tok::None &&
				t.text.find_first_not_of(L" \t") == std::wstring::npos)
			{
				continue;
			}

			SetTextColor(dc, tok_color(t.cls));

			ExtTextOutW(dc,
						xOff + t.col * g.charW,
						y,
						0,
						nullptr,
						t.text.c_str(),
						(UINT)t.text.size(),
						nullptr);
		}
	}

	// notes: word-wrapped to stay clear of the code, hugging the right edge
	if (g.showAnns)
	{
		SelectObject(dc, g.fNote);

		COLORREF	noteFill	=
			Blend(BG, 255, 255, 255, std::min(1.0, 0.35 + alpha * 0.65));

		int			codeW		= cw - g.gutterW;
		int			nxContent	= g.scrollX + codeW - 12;	   // content coords

		for (int ai = 0; ai < (int)g.anns.size(); ++ai)
		{
			const Annotation& a = g.anns[(size_t)ai];

			if (!g.visColors.count(a.color) || a.text.empty()) continue;

			int				lo = a.start_line;
			auto			avail =
				[&](int hi)
				{
					hi = std::max(lo, std::min(hi, (int)g.lineCols.size()));

					int cols = 0;

					for (int i = lo - 1; i < hi; ++i)
						cols = std::max(cols, g.lineCols[(size_t)i]);

					return nxContent - 24 - (g.pad + cols * g.charW);
				};
			int				hiCover	= std::min(a.end_line, n);
			int				width	= std::max(80, avail(hiCover));
			std::wstring	text	=
				(a.orphaned ? L"⚠ orphaned — " : L"⚑ ") + W(a.text);
			int				height	= 0;

			for (int iter = 0; iter < 6; ++iter)
			{
				RECT calc = {0, 0, width, 0};

				height = DrawTextW(dc, text.c_str(), -1, &calc,
								   DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

				int	bottomContent	= g.pad + (lo - 1) * lh + 2 + height;
				int	covered			= (bottomContent - g.pad) / lh + 1;

				if (covered <= hiCover || hiCover >= n)
					break;

				hiCover = std::min(covered, n);

				int newW = std::max(80, avail(hiCover));

				if (newW >= width) break;

				width = newW;
			}

			int	right	= g.gutterW + codeW - 12;	   // == nxContent on screen
			int	top		= g.pad + (lo - 1) * lh - g.scrollY + 2;

			if (top + height < -lh || top > ch + lh)
				continue;

			RECT r = {right - width, top, right, top + height};

			SetTextColor(dc, noteFill);

			DrawTextW(
				dc,
				text.c_str(),
				-1,
				&r,
				DT_WORDBREAK | DT_RIGHT | DT_NOPREFIX);

			g.noteRects.push_back({r, ai});
		}
	}

	// jump-target flash outline
	if (g.flashIdx >= 0 && g.flashIdx < (int)g.anns.size())
	{
		const Annotation&	a	= g.anns[(size_t)g.flashIdx];
		const auto*			c	= AnnRGB(a);
		RECT				r	=
			{
				bandL + 1,
				lineTop(a.start_line),
				bandR - 1,
				lineTop(std::min(a.end_line, n) + 1)
			};
		HPEN				pen	= CreatePen(PS_SOLID, 2, RGB(c->r, c->g, c->b));
		HGDIOBJ				op	= SelectObject(dc, pen);
		HGDIOBJ				ob	= SelectObject(dc, GetStockObject(NULL_BRUSH));

		Rectangle(dc, r.left, r.top, r.right, r.bottom);

		SelectObject(dc, op);
		SelectObject(dc, ob);

		DeleteObject(pen);
	}

	RestoreDC(dc, -1);

	SelectObject(dc, oldFont);
}

// ---------------------------------------------------------- code view input

static int LineAt(int y)
{
	int ln = (y + g.scrollY - g.pad) / g.lineH + 1;

	return std::max(1, std::min(LineCount(), ln));
}

static void ScrollBy(int dy)
{
	g.scrollY += dy;

	ClampScroll();
	UpdateScrollbars();

	InvalidateCode();
}

static void CodeRelease(POINT pt)
{
	if (g.gutterDrag >= 0)
	{
		int lo = g.pending ? g.pending->first : g.gutterDrag;
		int hi = g.pending ? g.pending->second : g.gutterDrag;

		g.gutterDrag = -1;

		Annotation ann;

		ann.id			= core::new_id();
		ann.start_line	= lo;
		ann.end_line	= hi;
		ann.anchor		= core::anchor_text(g.lines[(size_t)lo - 1]);

		POINT sp = pt;

		ClientToScreen(g.code, &sp);

		EditAnnotation(std::move(ann), true, -1, sp);

		return;
	}

	if (g.selAnchor < 0) return;

	g.selAnchor = -1;

	if (g.dragged) return;

	int line = LineAt(pt.y);

	// click on an annotation's note or band opens its edit dialog
	if (g.showAnns)
	{
		int hit = -1;

		for (auto it = g.noteRects.rbegin(); it != g.noteRects.rend(); ++it)
		{
			if (PtInRect(&it->first, pt))
			{
				hit = it->second;
				break;
			}
		}

		if (hit < 0)
		{
			for (int i = (int)g.anns.size() - 1; i >= 0; --i)
			{
				const Annotation& a = g.anns[(size_t)i];

				if (g.visColors.count(a.color) &&
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
			POINT sp = pt;

			ClientToScreen(g.code, &sp);

			EditAnnotation(g.anns[(size_t)hit], false, hit, sp);

			return;
		}
	}

	bool shift = GetKeyState(VK_SHIFT) < 0;

	if (shift && g.sel)
	{
		g.sel = {std::min(g.sel->first, line), std::max(g.sel->second, line)};
	}
	else if (g.sel && *g.sel == std::make_pair(line, line))
	{
		g.sel.reset();				// click again to clear
	}
	else
	{
		g.sel = {line, line};
	}

	if (g.sel)
	{
		int cnt = g.sel->second - g.sel->first + 1;

		SetHint(std::to_wstring(cnt) +
				(cnt == 1 ? L" line" : L" lines") +
				L" selected  •	Ctrl+C to copy");
	}
	else
	{
		DefaultHint();
	}

	InvalidateCode();
}

static LRESULT CALLBACK CodeProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT	ps;
			HDC			dc	= BeginPaint(w, &ps);
			RECT		rc;

			GetClientRect(w, &rc);

			HDC		mem	= CreateCompatibleDC(dc);
			HBITMAP	bmp	= CreateCompatibleBitmap(dc, rc.right, rc.bottom);
			HGDIOBJ	ob	= SelectObject(mem, bmp);

			PaintCode(mem, rc.right, rc.bottom);
			BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

			SelectObject(mem, ob);
			DeleteObject(bmp);
			DeleteDC(mem);

			EndPaint(w, &ps);

			return 0;
		}

		case WM_SIZE:
			ClampScroll();
			UpdateScrollbars();

			return 0;

		case WM_VSCROLL:
		{
			int wH, hH;

			CodeSize(wH, hH);

			switch (LOWORD(wp))
			{
				case SB_LINEUP:			g.scrollY -= g.lineH;	break;
				case SB_LINEDOWN:		g.scrollY += g.lineH;	break;
				case SB_PAGEUP:			g.scrollY -= hH;		break;
				case SB_PAGEDOWN:		g.scrollY += hH;		break;
				case SB_TOP:			g.scrollY = 0;			break;
				case SB_BOTTOM:			g.scrollY = g.contentH;	break;
				case SB_THUMBTRACK:
				case SB_THUMBPOSITION:
				{
					SCROLLINFO si{sizeof si, SIF_TRACKPOS};

					GetScrollInfo(w, SB_VERT, &si);

					g.scrollY = si.nTrackPos;

					break;
				}
			}

			ClampScroll();
			UpdateScrollbars();

			InvalidateCode();

			return 0;
		}

		case WM_HSCROLL:
		{
			int viewW = CodeViewWidth();

			switch (LOWORD(wp)) {
				case SB_LINELEFT:		g.scrollX -= g.charW * 4;	break;
				case SB_LINERIGHT:		g.scrollX += g.charW * 4;	break;
				case SB_PAGELEFT:		g.scrollX -= viewW;			break;
				case SB_PAGERIGHT:		g.scrollX += viewW;			break;
				case SB_THUMBTRACK:
				case SB_THUMBPOSITION:
				{
					SCROLLINFO si{sizeof si, SIF_TRACKPOS};

					GetScrollInfo(w, SB_HORZ, &si);

					g.scrollX = si.nTrackPos;

					break;
				}
			}

			ClampScroll();
			UpdateScrollbars();

			InvalidateCode();

			return 0;
		}

		case WM_MOUSEWHEEL:
		{
			int delta = GET_WHEEL_DELTA_WPARAM(wp);
			int step = delta > 0 ? 1 : -1;

			if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
			{
				Zoom(step);
			}
			else
			{
				ScrollBy(-step * 3 * g.lineH);
			}

			return 0;
		}

		case WM_SETCURSOR:
			if (LOWORD(lp) == HTCLIENT)
			{
				POINT pt;

				GetCursorPos(&pt);

				ScreenToClient(w, &pt);
				SetCursor(LoadCursorW(nullptr, pt.x < g.gutterW ? IDC_HAND : IDC_ARROW));

				return TRUE;
			}
			break;

		case WM_LBUTTONDOWN:
		{
			SetFocus(w);

			if (!g.hasFile) return 0;

			POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

			SetCapture(w);

			if (pt.x < g.gutterW)
			{
				if (g.annError)
				{
					FlashStatus(L"Annotating is disabled — this file's "
								L"annotation file could not be parsed");

					ReleaseCapture();

					return 0;
				}

				g.gutterDrag	= LineAt(pt.y);
				g.pending		= {g.gutterDrag, g.gutterDrag};

				InvalidateCode();
			} else {
				g.dragged		= false;
				g.selAnchor		= LineAt(pt.y);
			}

			return 0;
		}

		case WM_MOUSEMOVE:
		{
			if (GetCapture() != w || !g.hasFile) return 0;

			POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

			if (g.gutterDrag >= 0)
			{
				int line = LineAt(pt.y);

				g.pending =
					{
						std::min(g.gutterDrag, line),
						std::max(g.gutterDrag, line)
					};

				InvalidateCode();
			}
			else if (g.selAnchor >= 0)
			{
				int line = LineAt(pt.y);

				if (line != g.selAnchor) g.dragged = true;

				if (g.dragged)
				{
					g.sel = {std::min(g.selAnchor, line),
							 std::max(g.selAnchor, line)};

					int cnt = g.sel->second - g.sel->first + 1;

					SetHint(std::to_wstring(cnt) +
							(cnt == 1 ? L" line" : L" lines") +
							L" selected  •	Ctrl+C to copy");

					InvalidateCode();
				}
			}

			return 0;
		}

		case WM_LBUTTONUP:
		{
			if (GetCapture() == w) ReleaseCapture();

			if (!g.hasFile) return 0;

			CodeRelease({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});

			return 0;
		}

		case WM_KEYDOWN:
		{
			int wH, hH;

			CodeSize(wH, hH);

			switch (wp) {
				case VK_UP:		ScrollBy(-g.lineH);	return 0;
				case VK_DOWN:	ScrollBy(g.lineH);	return 0;
				case VK_PRIOR:	ScrollBy(-hH);		return 0;
				case VK_NEXT:	ScrollBy(hH);		return 0;
				case VK_HOME:
					g.scrollY = 0;

					ClampScroll();
					UpdateScrollbars();

					InvalidateCode();

					return 0;

				case VK_END:
					g.scrollY = g.contentH;

					ClampScroll();
					UpdateScrollbars();

					InvalidateCode();

					return 0;
			}
			break;
		}
	}

	return DefWindowProcW(w, msg, wp, lp);
}

// ---------------------------------------------------------------- edit mode
//
// "✎ Edit" swaps a real editor in for the code view: caret and selection,
// undo/redo, clipboard, syntax colors re-scanned incrementally as you type,
// and annotation bands that ride along with the text. Mirrors EditorPane in
// code_annotator.py.

static bool operator==(EdPos a, EdPos b) { return a.ln == b.ln && a.col == b.col; }
static bool operator!=(EdPos a, EdPos b) { return !(a == b); }
static bool operator<(EdPos a, EdPos b)  { return a.ln != b.ln ? a.ln < b.ln : a.col < b.col; }

static bool IsHighSurrogate(wchar_t c)	{ return c >= 0xD800 && c <= 0xDBFF; }
static bool IsLowSurrogate(wchar_t c)	{ return c >= 0xDC00 && c <= 0xDFFF; }

static int		EdLineCount()	{ return (int)ed.lines.size(); }
static EdPos	EdEnd()			{ return {EdLineCount() - 1, (int)ed.lines.back().size()}; }
static bool		EdHasSel()		{ return ed.caret != ed.anchor; }
static int		EdStateId()		{ return ed.undo.empty() ? 0 : ed.undo.back().serial; }
static bool		EdDirty()		{ return EdStateId() != ed.savedSerial; }

static EdPos EdClampPos(EdPos p)
{
	p.ln	= std::max(0, std::min(p.ln, EdLineCount() - 1));
	p.col	= std::max(0, std::min(p.col, (int)ed.lines[(size_t)p.ln].size()));

	return p;
}

static void EdSelRange(EdPos& a, EdPos& b)
{
	a = std::min(ed.caret, ed.anchor);
	b = std::max(ed.caret, ed.anchor);
}

// screen column of a raw column (tabs expanded, surrogate pairs one cell)
static int VisCol(const std::wstring& s, int col)
{
	int v = 0;

	for (int i = 0; i < col && i < (int)s.size(); ++i)
	{
		if (s[(size_t)i] == L'\t')
			v = (v / core::TAB_WIDTH + 1) * core::TAB_WIDTH;
		else if (!IsLowSurrogate(s[(size_t)i]))
			++v;
	}

	return v;
}

// raw column of the character boundary nearest to screen column fx
static int ColFromVis(const std::wstring& s, double fx)
{
	int v = 0;

	for (int i = 0; i < (int)s.size(); ++i)
	{
		wchar_t c = s[(size_t)i];

		if (IsLowSurrogate(c)) continue;

		int next = c == L'\t' ? (v / core::TAB_WIDTH + 1) * core::TAB_WIDTH : v + 1;

		if (fx < next)
			return (fx - v < next - fx) ? i : i + 1 + (IsHighSurrogate(c) ? 1 : 0);

		v = next;
	}

	return (int)s.size();
}

static int NextCol(const std::wstring& s, int col)
{
	if (col >= (int)s.size()) return col;

	++col;

	if (col < (int)s.size() && IsLowSurrogate(s[(size_t)col])) ++col;

	return col;
}

static int PrevCol(const std::wstring& s, int col)
{
	if (col <= 0) return 0;

	--col;

	if (col > 0 && IsLowSurrogate(s[(size_t)col])) --col;

	return col;
}

static bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

// 0 whitespace, 1 word, 2 punctuation
static int CharClass(wchar_t c) { return iswspace(c) ? 0 : IsWordChar(c) ? 1 : 2; }

static EdPos EdCharLeft(EdPos p)
{
	if (p.col > 0) return {p.ln, PrevCol(ed.lines[(size_t)p.ln], p.col)};

	if (p.ln > 0) return {p.ln - 1, (int)ed.lines[(size_t)p.ln - 1].size()};

	return p;
}

static EdPos EdCharRight(EdPos p)
{
	const std::wstring& s = ed.lines[(size_t)p.ln];

	if (p.col < (int)s.size()) return {p.ln, NextCol(s, p.col)};

	if (p.ln + 1 < EdLineCount()) return {p.ln + 1, 0};

	return p;
}

static EdPos EdWordLeft(EdPos p)
{
	if (p.col == 0) return EdCharLeft(p);

	const std::wstring&	s	= ed.lines[(size_t)p.ln];
	int					i	= p.col;

	while (i > 0 && iswspace(s[(size_t)i - 1])) --i;

	if (i > 0)
	{
		int cls = CharClass(s[(size_t)i - 1]);

		while (i > 0 && CharClass(s[(size_t)i - 1]) == cls) --i;
	}

	return {p.ln, i};
}

static EdPos EdWordRight(EdPos p)
{
	const std::wstring&	s	= ed.lines[(size_t)p.ln];
	int					n	= (int)s.size();

	if (p.col >= n) return EdCharRight(p);

	int i	= p.col;
	int cls	= CharClass(s[(size_t)i]);

	if (cls != 0)
		while (i < n && CharClass(s[(size_t)i]) == cls) ++i;

	while (i < n && iswspace(s[(size_t)i])) ++i;

	return {p.ln, i};
}

// the word (or run of punctuation / whitespace) around a position
static void EdWordAt(EdPos p, EdPos& a, EdPos& b)
{
	const std::wstring&	s	= ed.lines[(size_t)p.ln];
	int					n	= (int)s.size();
	int					i	= std::min(p.col, n);

	if (n == 0)
	{
		a = b = {p.ln, 0};

		return;
	}

	if (i == n) --i;

	int cls = CharClass(s[(size_t)i]);
	int lo	= i;
	int hi	= i;

	while (lo > 0 && CharClass(s[(size_t)lo - 1]) == cls) --lo;

	while (hi < n && CharClass(s[(size_t)hi]) == cls) ++hi;

	a = {p.ln, lo};
	b = {p.ln, hi};
}

// position just past line ln, including its newline when there is one
static EdPos EdAfterLine(int ln)
{
	if (ln + 1 < EdLineCount()) return {ln + 1, 0};

	return {ln, (int)ed.lines[(size_t)ln].size()};
}

static std::wstring EdText(EdPos a, EdPos b)
{
	if (b < a) std::swap(a, b);

	if (a.ln == b.ln)
		return ed.lines[(size_t)a.ln].substr((size_t)a.col, (size_t)(b.col - a.col));

	std::wstring out = ed.lines[(size_t)a.ln].substr((size_t)a.col);

	for (int l = a.ln + 1; l < b.ln; ++l)
	{
		out += L'\n';
		out += ed.lines[(size_t)l];
	}

	out += L'\n';
	out += ed.lines[(size_t)b.ln].substr(0, (size_t)b.col);

	return out;
}

// where text inserted at `at` ends
static EdPos EdEndOf(EdPos at, const std::wstring& text)
{
	size_t nl = text.rfind(L'\n');

	if (nl == std::wstring::npos) return {at.ln, at.col + (int)text.size()};

	return {at.ln + (int)std::count(text.begin(), text.end(), L'\n'),
			(int)(text.size() - nl - 1)};
}

// --- annotation bands -------------------------------------------------------

// a position at or after an edit's replaced range [a, b), which now ends at e
static EdPos EdShiftPast(EdPos p, EdPos b, EdPos e)
{
	if (p.ln == b.ln) return {e.ln, e.col + p.col - b.col};

	return {p.ln + e.ln - b.ln, p.col};
}

// Bands follow the text like Tk tags: text typed at a band's edges stays
// outside it, text typed inside it joins it, and a band whose whole region
// is deleted collapses to a point that then behaves like a left-gravity
// mark (the orphan position used on save).
static void EdShiftBands(EdPos a, EdPos b, EdPos e)
{
	for (auto& bd : ed.bands)
	{
		if (bd.start == bd.end)
		{
			EdPos p = bd.start;

			if (b < p)			p = EdShiftPast(p, b, e);
			else if (!(p < a))	p = a;

			bd.start = bd.end = p;

			continue;
		}

		if (a < b && !(bd.start < a) && !(b < bd.end))
		{
			bd.start = bd.end = a;				// whole region deleted

			continue;
		}

		if (!(bd.start < a))
			bd.start = !(bd.start < b) ? EdShiftPast(bd.start, b, e) : e;

		if (a < bd.end)
			bd.end = b < bd.end ? EdShiftPast(bd.end, b, e) : a;

		if (bd.end < bd.start) bd.end = bd.start;
	}
}

static void EdBandLines(const EdBand& bd, int& lo, int& hi)		// 0-based, inclusive
{
	lo = bd.start.ln;
	hi = bd.end.col == 0 ? bd.end.ln - 1 : bd.end.ln;
	hi = std::max(lo, hi);
}

// --- highlighting -----------------------------------------------------------

static bool SameScanState(const hl::ScanState& a, const hl::ScanState& b)
{
	return a.mode == b.mode && a.delim == b.delim;
}

// re-scan lines from..to (0-based), then keep going while the scanner state
// carried into the following lines differs from what they were scanned with
static void EdRehighlight(int from, int to)
{
	const hl::LangCfg*	cfg	= hl::lang_cfg(g.langId);
	int					n	= EdLineCount();

	if (!cfg)
	{
		for (int i = from; i <= to && i < n; ++i)
		{
			ed.toks[(size_t)i].clear();

			if (!ed.lines[(size_t)i].empty())
				ed.toks[(size_t)i].push_back({0, ed.lines[(size_t)i], hl::Tok::None});
		}

		return;
	}

	hl::ScanState st = ed.states[(size_t)from];

	for (int i = from; i < n; ++i)
	{
		if (i > to && SameScanState(st, ed.states[(size_t)i])) break;

		ed.states[(size_t)i]	= st;
		ed.toks[(size_t)i]		= cfg->html
			? hl::scan_html_line(ed.lines[(size_t)i], st)
			: hl::scan_line(ed.lines[(size_t)i], *cfg, st);
	}
}

// --- metrics / scrolling ----------------------------------------------------

static void EdClientSize(int& w, int& h)
{
	RECT r;

	GetClientRect(ed.wnd, &r);

	w = r.right;
	h = r.bottom;
}

static void EdMetrics()
{
	int n		= EdLineCount();
	int maxVis	= 0;

	for (int v : ed.visW) maxVis = std::max(maxVis, v);

	int digits	= (int)std::to_wstring(std::max(1, n)).size();

	ed.gutterW	= g.pad * 2 + std::max(2, digits) * g.charW;
	ed.contentW	= g.pad * 2 + (maxVis + 1) * g.charW + S(100);
	ed.contentH	= g.pad * 2 + n * g.lineH;
}

static void EdClampScroll()
{
	int w, h;

	EdClientSize(w, h);

	ed.scrollY = std::max(0, std::min(ed.scrollY, ed.contentH - h));
	ed.scrollX = std::max(0, std::min(ed.scrollX, ed.contentW - std::max(1, w - ed.gutterW)));
}

static void EdUpdateScrollbars()
{
	int w, h;

	EdClientSize(w, h);

	SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};

	si.nMax		= std::max(0, ed.contentH - 1);
	si.nPage	= (UINT)h;
	si.nPos		= ed.scrollY;

	SetScrollInfo(ed.wnd, SB_VERT, &si, TRUE);

	si.nMax		= std::max(0, ed.contentW - 1);
	si.nPage	= (UINT)std::max(1, w - ed.gutterW);
	si.nPos		= ed.scrollX;

	SetScrollInfo(ed.wnd, SB_HORZ, &si, TRUE);
}

static void EdInvalidate() { if (ed.wnd) InvalidateRect(ed.wnd, nullptr, FALSE); }

static void EdUpdateCaret()
{
	if (!ed.active || GetFocus() != ed.wnd) return;

	const std::wstring& s = ed.lines[(size_t)ed.caret.ln];

	int x = ed.gutterW + g.pad - ed.scrollX + VisCol(s, ed.caret.col) * g.charW;
	int y = g.pad + ed.caret.ln * g.lineH - ed.scrollY + 1;

	if (x < ed.gutterW) x = -S(100);		// scrolled under the gutter: hide

	SetCaretPos(x, y);

	// keep IME composition windows next to the caret
	if (HIMC imc = ImmGetContext(ed.wnd))
	{
		COMPOSITIONFORM cf{};

		cf.dwStyle			= CFS_POINT;
		cf.ptCurrentPos		= {x, y};

		ImmSetCompositionWindow(imc, &cf);
		ImmReleaseContext(ed.wnd, imc);
	}
}

static void EdScrollTo(int x, int y)
{
	ed.scrollX = x;
	ed.scrollY = y;

	EdClampScroll();
	EdUpdateScrollbars();
	EdUpdateCaret();
	EdInvalidate();
}

static void EdEnsureVisible()
{
	int w, h;

	EdClientSize(w, h);

	int y		= g.pad + ed.caret.ln * g.lineH;
	int sy		= ed.scrollY;

	if (y < sy)							sy = y - g.pad;
	else if (y + g.lineH > sy + h)		sy = y + g.lineH + g.pad - h;

	int viewW	= std::max(1, w - ed.gutterW);
	int x		= g.pad + VisCol(ed.lines[(size_t)ed.caret.ln], ed.caret.col) * g.charW;
	int sx		= ed.scrollX;

	if (x < sx + g.charW)					sx = x - viewW / 4;
	else if (x > sx + viewW - 2 * g.charW)	sx = x - viewW * 3 / 4;

	EdScrollTo(sx, sy);
}

static void EdUpdateTitle()
{
	if (!ed.active) return;

	g.statusFile = g.filePath.filename().wstring() + L"  •  editing";

	if (EdDirty()) g.statusFile += L"  ●";

	InvalidateStatus();
}

// after any text change: metrics, scrollbars, title, stale find matches
static void EdChanged()
{
	EdMetrics();
	EdEnsureVisible();
	EdUpdateTitle();

	if (IsWindowVisible(g.findBar)) FindRefresh(false);

	EdInvalidate();
}

// --- editing primitives -----------------------------------------------------

// replace [a, b) with text (which uses '\n' newlines); returns its end
static EdPos EdReplace(EdPos a, EdPos b, const std::wstring& text)
{
	std::vector<std::wstring>	parts(1);

	for (wchar_t c : text)
	{
		if (c == L'\n')	parts.emplace_back();
		else			parts.back() += c;
	}

	std::wstring tail	= ed.lines[(size_t)b.ln].substr((size_t)b.col);

	parts.front().insert(0, ed.lines[(size_t)a.ln], 0, (size_t)a.col);

	EdPos e				= {a.ln + (int)parts.size() - 1, (int)parts.back().size()};

	parts.back() += tail;

	auto first	= (ptrdiff_t)a.ln;
	auto last	= (ptrdiff_t)b.ln + 1;
	auto count	= (ptrdiff_t)parts.size();

	hl::ScanState st0 = ed.states[(size_t)a.ln];

	ed.lines.erase(ed.lines.begin() + first, ed.lines.begin() + last);
	ed.lines.insert(ed.lines.begin() + first, parts.begin(), parts.end());

	ed.toks.erase(ed.toks.begin() + first, ed.toks.begin() + last);
	ed.toks.insert(ed.toks.begin() + first, (size_t)count, {});

	ed.states.erase(ed.states.begin() + first, ed.states.begin() + last);
	ed.states.insert(ed.states.begin() + first, (size_t)count, hl::ScanState{});
	ed.states[(size_t)a.ln] = st0;

	ed.visW.erase(ed.visW.begin() + first, ed.visW.begin() + last);
	ed.visW.insert(ed.visW.begin() + first, (size_t)count, 0);

	for (int i = a.ln; i < a.ln + (int)count; ++i)
	{
		const std::wstring& s = ed.lines[(size_t)i];

		ed.visW[(size_t)i] = VisCol(s, (int)s.size());
	}

	EdShiftBands(a, b, e);
	EdRehighlight(a.ln, a.ln + (int)count - 1);

	return e;
}

// one user edit: replace [a, b) with text, recorded for undo. Consecutive
// typing / backspacing / deleting merge into one undo step.
static void EdEdit(EdPos a, EdPos b, const std::wstring& text, EdKind kind)
{
	if (b < a) std::swap(a, b);

	if (a == b && text.empty()) return;

	EdOp	op			= {a, EdText(a, b), text};
	EdPos	caretBefore	= ed.caret;
	EdPos	anchorBefore= ed.anchor;
	EdPos	e			= EdReplace(a, b, text);

	ed.caret = ed.anchor = e;

	bool merge = ed.mergeOk &&
				 kind != EdKind::Other &&
				 !ed.undo.empty() &&
				 ed.undo.back().kind == kind;

	if (merge)
	{
		EdGroup& grp = ed.undo.back();

		grp.ops.push_back(std::move(op));

		grp.caretAfter	= ed.caret;
		grp.anchorAfter	= ed.anchor;
	}
	else
	{
		EdGroup grp;

		grp.ops.push_back(std::move(op));

		grp.caretBefore		= caretBefore;
		grp.anchorBefore	= anchorBefore;
		grp.caretAfter		= ed.caret;
		grp.anchorAfter		= ed.anchor;
		grp.kind			= kind;
		grp.serial			= ed.nextSerial++;

		ed.undo.push_back(std::move(grp));
	}

	ed.redo.clear();

	ed.mergeOk	= kind != EdKind::Other;
	ed.goalVis	= -1;

	EdChanged();
}

static void EdReplaceSel(const std::wstring& text, EdKind kind)
{
	EdPos a, b;

	EdSelRange(a, b);

	if (a != b) kind = EdKind::Other;

	EdEdit(a, b, text, kind);
}

static void EdUndo()
{
	if (ed.undo.empty()) return;

	EdGroup grp = std::move(ed.undo.back());

	ed.undo.pop_back();

	for (auto it = grp.ops.rbegin(); it != grp.ops.rend(); ++it)
		EdReplace(it->at, EdEndOf(it->at, it->inserted), it->removed);

	ed.caret	= grp.caretBefore;
	ed.anchor	= grp.anchorBefore;

	ed.redo.push_back(std::move(grp));

	ed.mergeOk	= false;
	ed.goalVis	= -1;

	EdChanged();
}

static void EdRedo()
{
	if (ed.redo.empty()) return;

	EdGroup grp = std::move(ed.redo.back());

	ed.redo.pop_back();

	for (const auto& op : grp.ops)
		EdReplace(op.at, EdEndOf(op.at, op.removed), op.inserted);

	ed.caret	= grp.caretAfter;
	ed.anchor	= grp.anchorAfter;

	ed.undo.push_back(std::move(grp));

	ed.mergeOk	= false;
	ed.goalVis	= -1;

	EdChanged();
}

// --- caret movement ---------------------------------------------------------

static void EdMoveTo(EdPos p, bool extend)
{
	ed.caret = EdClampPos(p);

	if (!extend) ed.anchor = ed.caret;

	ed.mergeOk = false;

	EdEnsureVisible();
	EdInvalidate();
}

static void EdMoveVert(int lines, bool extend)
{
	if (ed.goalVis < 0)
		ed.goalVis = VisCol(ed.lines[(size_t)ed.caret.ln], ed.caret.col);

	int		goal	= ed.goalVis;
	int		ln		= ed.caret.ln + lines;
	EdPos	p;

	if (ln < 0)							p = {0, 0};
	else if (ln >= EdLineCount())		p = EdEnd();
	else								p = {ln, ColFromVis(ed.lines[(size_t)ln], goal)};

	EdMoveTo(p, extend);

	ed.goalVis = goal;
}

static void EdSelectAll()
{
	ed.anchor	= {0, 0};
	ed.caret	= EdEnd();
	ed.mergeOk	= false;

	EdEnsureVisible();
	EdInvalidate();
}

// --- clipboard --------------------------------------------------------------

static bool EdCopy()
{
	if (!EdHasSel()) return false;

	std::wstring text;

	for (wchar_t c : EdText(ed.caret, ed.anchor))
	{
		if (c == L'\n') text += L'\r';

		text += c;
	}

	if (!OpenClipboard(g.main)) return false;

	EmptyClipboard();

	size_t	bytes	= (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL	mem		= GlobalAlloc(GMEM_MOVEABLE, bytes);

	if (mem)
	{
		memcpy(GlobalLock(mem), text.c_str(), bytes);

		GlobalUnlock(mem);

		SetClipboardData(CF_UNICODETEXT, mem);
	}

	CloseClipboard();

	return true;
}

static void EdPaste()
{
	if (!OpenClipboard(g.main)) return;

	std::wstring	text;
	HANDLE			h		= GetClipboardData(CF_UNICODETEXT);

	if (h)
	{
		if (const wchar_t* p = (const wchar_t*)GlobalLock(h))
		{
			for (; *p; ++p)
			{
				if (*p == L'\r')
				{
					if (p[1] == L'\n') ++p;

					text += L'\n';
				}
				else
				{
					text += *p;
				}
			}

			GlobalUnlock(h);
		}
	}

	CloseClipboard();

	if (!text.empty()) EdReplaceSel(text, EdKind::Other);
}

// --- painting ---------------------------------------------------------------

static void EdPaint(HDC dc, int cw, int ch)
{
	RECT all = {0, 0, cw, ch};

	FillRectColor(dc, all, BG);

	SetBkMode(dc, TRANSPARENT);

	int	n	= EdLineCount();
	int	lh	= g.lineH;
	int	r0	= std::max(0, (ed.scrollY - g.pad) / lh);
	int	r1	= std::min(n - 1, (ed.scrollY + ch - g.pad) / lh + 1);

	auto lineTop = [&](int ln0) { return g.pad + ln0 * lh - ed.scrollY; };

	// gutter line numbers; the caret's line is brighter
	HGDIOBJ oldFont = SelectObject(dc, g.fCode);

	SetTextAlign(dc, TA_RIGHT | TA_TOP);

	for (int row = r0; row <= r1; ++row)
	{
		std::wstring num = std::to_wstring(row + 1);

		SetTextColor(dc, row == ed.caret.ln ? FG : GUTTER_FG);

		ExtTextOutW(dc, ed.gutterW - g.pad, lineTop(row) + 2, 0, nullptr,
					num.c_str(), (UINT)num.size(), nullptr);
	}

	SetTextAlign(dc, TA_LEFT | TA_TOP);

	SaveDC(dc);

	IntersectClipRect(dc, ed.gutterW, 0, cw, ch);

	int		bandL	= ed.gutterW - ed.scrollX;
	int		bandR	= std::max(cw, bandL + ed.contentW);
	int		xOff	= ed.gutterW + g.pad - ed.scrollX;
	double	alpha	= g.opacity / 100.0;

	auto xAt = [&](int ln0, int col)
	{
		return xOff + VisCol(ed.lines[(size_t)ln0], col) * g.charW;
	};

	// annotation bands
	if (g.showAnns)
	{
		for (size_t i = 0; i < ed.bands.size() && i < g.anns.size(); ++i)
		{
			const Annotation&	a	= g.anns[i];
			const EdBand&		bd	= ed.bands[i];

			if (!g.visColors.count(a.color) || bd.start == bd.end) continue;

			int lo, hi;

			EdBandLines(bd, lo, hi);

			int y0 = lineTop(lo);
			int y1 = lineTop(hi + 1);

			if (y1 < 0 || y0 > ch) continue;

			const auto*	c	= AnnRGB(a);
			double		f	= a.orphaned ? 0.18 : 0.45;
			RECT		r	= {bandL, y0, bandR, y1};

			FillRectColor(dc, r, Blend(BG, c->r, c->g, c->b, alpha * f));

			RECT re = {bandL, y0, bandL + 3, y1};

			FillRectColor(dc, re, Blend(BG, c->r, c->g, c->b, std::min(1.0, alpha * 1.5)));
		}
	}

	// find matches
	for (int i = 0; i < (int)g.matches.size(); ++i)
	{
		const Match& m = g.matches[(size_t)i];

		if (m.ln - 1 < r0 || m.ln - 1 > r1 || m.ln > n) continue;

		int		y0	= lineTop(m.ln - 1);
		bool	cur	= i == g.matchIdx;
		RECT	r	= {xAt(m.ln - 1, m.col), y0 + 1, xAt(m.ln - 1, m.col + m.len), y0 + lh - 1};

		FillRectColor(dc, r, Blend(BG, 210, 170, 60, cur ? 0.55 : 0.25));
	}

	// selection
	if (EdHasSel())
	{
		EdPos a, b;

		EdSelRange(a, b);

		COLORREF selC = RGB(0x26, 0x4f, 0x78);

		for (int row = std::max(a.ln, r0); row <= std::min(b.ln, r1); ++row)
		{
			int x0 = row == a.ln ? xAt(row, a.col) : xOff;
			int x1 = row == b.ln ? xAt(row, b.col)
								 : xAt(row, (int)ed.lines[(size_t)row].size()) + g.charW / 2;

			RECT r = {x0, lineTop(row), x1, lineTop(row) + lh};

			FillRectColor(dc, r, selC);
		}
	}

	// code text: tokens, with tabs expanded to the next tab stop
	SelectObject(dc, g.fCode);

	for (int row = r0; row <= r1; ++row)
	{
		int y = lineTop(row) + 2;

		for (const auto& t : ed.toks[(size_t)row])
		{
			SetTextColor(dc, tok_color(t.cls));

			size_t i = 0;

			while (i < t.text.size())
			{
				size_t j = t.text.find(L'\t', i);

				if (j == std::wstring::npos) j = t.text.size();

				if (j > i)
				{
					ExtTextOutW(dc, xAt(row, t.col + (int)i), y, 0, nullptr,
								t.text.c_str() + i, (UINT)(j - i), nullptr);
				}

				i = j + 1;
			}
		}
	}

	RestoreDC(dc, -1);

	SelectObject(dc, oldFont);
}

// --- mouse ------------------------------------------------------------------

static EdPos EdPosFromPoint(POINT pt)
{
	int ln = (int)std::floor((double)(pt.y + ed.scrollY - g.pad) / g.lineH);

	ln = std::max(0, std::min(ln, EdLineCount() - 1));

	double fx = (double)(pt.x - (ed.gutterW + g.pad - ed.scrollX)) / g.charW;

	return {ln, ColFromVis(ed.lines[(size_t)ln], fx)};
}

static void EdDragTo(POINT pt)
{
	EdPos p = EdPosFromPoint(pt);

	if (ed.mouseMode == 1)
	{
		ed.caret = p;
	}
	else if (ed.mouseMode == 2)				// by words
	{
		EdPos a, b;

		EdWordAt(p, a, b);

		if (p < ed.mouseLo)	{ ed.anchor = ed.mouseHi; ed.caret = a; }
		else				{ ed.anchor = ed.mouseLo; ed.caret = std::max(b, ed.mouseHi); }
	}
	else if (ed.mouseMode == 3)				// by lines (gutter drag)
	{
		if (p.ln >= ed.mouseLo.ln)	{ ed.anchor = {ed.mouseLo.ln, 0}; ed.caret = EdAfterLine(p.ln); }
		else						{ ed.anchor = EdAfterLine(ed.mouseLo.ln); ed.caret = {p.ln, 0}; }
	}

	EdEnsureVisible();
	EdInvalidate();
}

// --- keyboard ---------------------------------------------------------------

static void EdDeleteSelOr(EdPos target, EdKind kind)
{
	if (EdHasSel())
		EdReplaceSel(L"", EdKind::Other);
	else
		EdEdit(ed.caret, target, L"", kind);
}

static bool EdKeyDown(WPARAM vk)
{
	bool	ctrl	= GetKeyState(VK_CONTROL) < 0;
	bool	shift	= GetKeyState(VK_SHIFT) < 0;
	int		w, h;

	EdClientSize(w, h);

	int		page	= std::max(1, h / g.lineH - 1);
	EdPos	a, b;

	EdSelRange(a, b);

	switch (vk)
	{
		case VK_LEFT:
			ed.goalVis = -1;

			if (EdHasSel() && !shift && !ctrl)	EdMoveTo(a, false);
			else								EdMoveTo(ctrl ? EdWordLeft(ed.caret) : EdCharLeft(ed.caret), shift);

			return true;

		case VK_RIGHT:
			ed.goalVis = -1;

			if (EdHasSel() && !shift && !ctrl)	EdMoveTo(b, false);
			else								EdMoveTo(ctrl ? EdWordRight(ed.caret) : EdCharRight(ed.caret), shift);

			return true;

		case VK_UP:
		case VK_DOWN:
		{
			int dir = vk == VK_UP ? -1 : 1;

			if (ctrl)	EdScrollTo(ed.scrollX, ed.scrollY + dir * g.lineH);
			else		EdMoveVert(dir, shift);

			return true;
		}

		case VK_PRIOR:
		case VK_NEXT:
		{
			int dir = vk == VK_PRIOR ? -1 : 1;

			ed.scrollY += dir * page * g.lineH;

			EdClampScroll();
			EdMoveVert(dir * page, shift);

			return true;
		}

		case VK_HOME:
		{
			ed.goalVis = -1;

			if (ctrl)
			{
				EdMoveTo({0, 0}, shift);

				return true;
			}

			// smart home: first non-blank, then column 0
			const std::wstring&	s		= ed.lines[(size_t)ed.caret.ln];
			int					indent	= 0;

			while (indent < (int)s.size() && (s[(size_t)indent] == L' ' || s[(size_t)indent] == L'\t'))
				++indent;

			EdMoveTo({ed.caret.ln, ed.caret.col == indent ? 0 : indent}, shift);

			return true;
		}

		case VK_END:
			ed.goalVis = -1;

			EdMoveTo(ctrl ? EdEnd() : EdPos{ed.caret.ln, (int)ed.lines[(size_t)ed.caret.ln].size()}, shift);

			return true;

		case VK_BACK:
			EdDeleteSelOr(ctrl ? EdWordLeft(ed.caret) : EdCharLeft(ed.caret),
						  ctrl ? EdKind::Other : EdKind::Back);

			return true;

		case VK_DELETE:
			if (shift && !ctrl)
			{
				if (EdCopy()) EdReplaceSel(L"", EdKind::Other);		// Shift+Del cuts

				return true;
			}

			EdDeleteSelOr(ctrl ? EdWordRight(ed.caret) : EdCharRight(ed.caret),
						  ctrl ? EdKind::Other : EdKind::Del);

			return true;

		case VK_INSERT:
			if (ctrl)		EdCopy();
			else if (shift)	EdPaste();

			return true;

		case VK_RETURN:
			EdReplaceSel(L"\n", EdKind::Other);

			return true;

		case VK_TAB:
			if (!ctrl) EdReplaceSel(L"\t", EdKind::Type);

			return true;
	}

	if (!ctrl) return false;

	switch (vk)
	{
		case 'A':	EdSelectAll();							return true;
		case 'C':	EdCopy();								return true;
		case 'X':	if (EdCopy()) EdReplaceSel(L"", EdKind::Other); return true;
		case 'V':	EdPaste();								return true;
		case 'Y':	EdRedo();								return true;
		case 'Z':	shift ? EdRedo() : EdUndo();			return true;
	}

	return false;
}

static void EdTypeChar(wchar_t c)
{
	// a space after a word closes the current typing undo step, so undo
	// works roughly a word at a time
	if (iswspace(c) && ed.caret.col > 0 &&
		!iswspace(ed.lines[(size_t)ed.caret.ln][(size_t)ed.caret.col - 1]))
	{
		ed.mergeOk = false;
	}

	EdReplaceSel(std::wstring(1, c), EdKind::Type);
}

static LRESULT CALLBACK EditorProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT	ps;
			HDC			dc	= BeginPaint(w, &ps);
			RECT		rc;

			GetClientRect(w, &rc);

			HDC		mem	= CreateCompatibleDC(dc);
			HBITMAP	bmp	= CreateCompatibleBitmap(dc, rc.right, rc.bottom);
			HGDIOBJ	ob	= SelectObject(mem, bmp);

			if (ed.active)	EdPaint(mem, rc.right, rc.bottom);
			else			FillRectColor(mem, rc, BG);

			BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

			SelectObject(mem, ob);
			DeleteObject(bmp);
			DeleteDC(mem);

			EndPaint(w, &ps);

			return 0;
		}

		case WM_SIZE:
			if (ed.active)
			{
				EdClampScroll();
				EdUpdateScrollbars();
				EdUpdateCaret();
			}

			return 0;

		case WM_SETFOCUS:
			CreateCaret(w, nullptr, std::max(2, S(2)), g.lineH - 2);

			EdUpdateCaret();

			ShowCaret(w);

			return 0;

		case WM_KILLFOCUS:
			DestroyCaret();

			return 0;

		case WM_VSCROLL:
		case WM_HSCROLL:
		{
			if (!ed.active) return 0;

			bool	vert	= msg == WM_VSCROLL;
			int		cw, chh;

			EdClientSize(cw, chh);

			int		pageV	= vert ? chh : std::max(1, cw - ed.gutterW);
			int		stepV	= vert ? g.lineH : g.charW * 4;
			int&	pos		= vert ? ed.scrollY : ed.scrollX;

			switch (LOWORD(wp))
			{
				case SB_LINEUP:		pos -= stepV;	break;
				case SB_LINEDOWN:	pos += stepV;	break;
				case SB_PAGEUP:		pos -= pageV;	break;
				case SB_PAGEDOWN:	pos += pageV;	break;
				case SB_TOP:		pos = 0;		break;
				case SB_BOTTOM:		pos = INT_MAX / 2; break;
				case SB_THUMBTRACK:
				case SB_THUMBPOSITION:
				{
					SCROLLINFO si{sizeof si, SIF_TRACKPOS};

					GetScrollInfo(w, vert ? SB_VERT : SB_HORZ, &si);

					pos = si.nTrackPos;

					break;
				}
			}

			EdScrollTo(ed.scrollX, ed.scrollY);

			return 0;
		}

		case WM_MOUSEWHEEL:
		{
			int step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;

			if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)	Zoom(step);
			else if (ed.active)							EdScrollTo(ed.scrollX, ed.scrollY - step * 3 * g.lineH);

			return 0;
		}

		case WM_SETCURSOR:
			if (LOWORD(lp) == HTCLIENT)
			{
				POINT pt;

				GetCursorPos(&pt);
				ScreenToClient(w, &pt);
				SetCursor(LoadCursorW(nullptr, pt.x < ed.gutterW ? IDC_ARROW : IDC_IBEAM));

				return TRUE;
			}

			break;

		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
		{
			SetFocus(w);

			if (!ed.active) return 0;

			POINT	pt		= {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			EdPos	p		= EdPosFromPoint(pt);
			bool	shift	= (wp & MK_SHIFT) != 0;

			SetCapture(w);
			SetTimer(w, 1, 50, nullptr);			// drag autoscroll

			ed.mergeOk	= false;
			ed.goalVis	= -1;

			if (pt.x < ed.gutterW)
			{
				ed.mouseMode	= 3;
				ed.mouseLo		= {p.ln, 0};
				ed.anchor		= {p.ln, 0};
				ed.caret		= EdAfterLine(p.ln);
			}
			else if (msg == WM_LBUTTONDBLCLK)
			{
				ed.mouseMode = 2;

				EdWordAt(p, ed.mouseLo, ed.mouseHi);

				ed.anchor	= ed.mouseLo;
				ed.caret	= ed.mouseHi;
			}
			else
			{
				ed.mouseMode	= 1;
				ed.caret		= p;

				if (!shift) ed.anchor = p;
			}

			EdEnsureVisible();
			EdInvalidate();

			return 0;
		}

		case WM_MOUSEMOVE:
			if (GetCapture() == w && ed.active && ed.mouseMode)
				EdDragTo({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});

			return 0;

		case WM_TIMER:
		{
			if (GetCapture() != w || !ed.active || !ed.mouseMode) return 0;

			POINT	pt;
			RECT	rc;

			GetCursorPos(&pt);
			ScreenToClient(w, &pt);
			GetClientRect(w, &rc);

			if (!PtInRect(&rc, pt)) EdDragTo(pt);

			return 0;
		}

		case WM_LBUTTONUP:
		case WM_CAPTURECHANGED:
			if (msg == WM_LBUTTONUP && GetCapture() == w) ReleaseCapture();

			KillTimer(w, 1);

			ed.mouseMode = 0;

			return 0;

		case WM_KEYDOWN:
			if (ed.active && EdKeyDown(wp)) return 0;

			break;

		case WM_CHAR:
			// control characters are handled as keys in WM_KEYDOWN
			if (ed.active && wp >= 0x20 && wp != 0x7F)
				EdTypeChar((wchar_t)wp);

			return 0;
	}

	return DefWindowProcW(w, msg, wp, lp);
}

// --- entering / leaving / saving ---------------------------------------------

static void EdHideBars()
{
	ShowWindow(g.findBar, SW_HIDE);
	ShowWindow(g.gotoBar, SW_HIDE);

	g.matches.clear();

	g.matchIdx = -1;

	g.lastQuery.clear();
}

static void EdEnter()
{
	if (ed.active) return;

	if (!g.hasFile)
	{
		FlashStatus(L"Open a file first");

		return;
	}

	if (g.truncated)
	{
		FlashStatus(L"File is truncated in this view — too large to edit here");

		return;
	}

	core::EditSource src = core::load_source_for_edit(g.filePath);

	if (src.status == core::EditSource::Unreadable)
	{
		FlashStatus(L"Could not read file: " + g.filePath.filename().wstring());

		return;
	}

	if (src.status == core::EditSource::NotUtf8)
	{
		FlashStatus(L"File is not valid UTF-8 — editing disabled");

		return;
	}

	EdHideBars();

	g.sel.reset();
	g.pending.reset();

	std::error_code ec;
	auto			t	= fs::last_write_time(g.filePath, ec);

	ed.mtime	= ec ? std::nullopt : std::optional(t);
	ed.lines	= std::move(src.lines);
	ed.crlf		= src.crlf;
	ed.bom		= src.bom;

	int n = EdLineCount();

	ed.toks.assign((size_t)n, {});
	ed.states.assign((size_t)n, hl::ScanState{});
	ed.visW.assign((size_t)n, 0);

	for (int i = 0; i < n; ++i)
		ed.visW[(size_t)i] = VisCol(ed.lines[(size_t)i], (int)ed.lines[(size_t)i].size());

	EdRehighlight(0, n - 1);

	// each annotation's lines become a tracked [start, end) region
	ed.bands.clear();

	for (const auto& a : g.anns)
	{
		int		lo	= std::min(a.start_line, n) - 1;
		int		hi	= std::min(a.end_line, n);			// next line, 0-based
		EdBand	bd;

		bd.start	= {lo, 0};
		bd.end		= hi < n ? EdPos{hi, 0} : EdEnd();

		ed.bands.push_back(bd);
	}

	ed.undo.clear();
	ed.redo.clear();

	ed.nextSerial	= 1;
	ed.savedSerial	= 0;
	ed.mergeOk		= false;
	ed.goalVis		= -1;
	ed.mouseMode	= 0;
	ed.active		= true;

	// open at the same spot the annotate view was showing
	ed.scrollX		= 0;
	ed.scrollY		= g.scrollY;

	int top = std::max(0, std::min(n - 1, (g.scrollY + g.lineH - 1) / g.lineH));

	ed.caret = ed.anchor = {top, 0};

	EdMetrics();

	ShowWindow(g.code, SW_HIDE);
	ShowWindow(ed.wnd, SW_SHOW);

	EdClampScroll();
	EdUpdateScrollbars();

	SetFocus(ed.wnd);

	EdUpdateTitle();
	DefaultHint();

	InvalidateChrome();
	EdInvalidate();
}

// re-anchor annotations from their bands, then persist any changes
static void EdSyncAnnotations()
{
	bool changed = false;

	for (size_t i = 0; i < g.anns.size() && i < ed.bands.size(); ++i)
	{
		const EdBand& bd = ed.bands[i];

		changed |= core::rebase_annotation(
			g.anns[i],
			bd.start.ln,
			bd.end.ln,
			bd.end.col,
			bd.start == bd.end,
			ed.lines);
	}

	if (!changed) return;

	// keep the bands parallel to the (re-sorted) annotations
	std::vector<std::pair<Annotation, EdBand>> pairs;

	for (size_t i = 0; i < g.anns.size(); ++i)
		pairs.push_back({g.anns[i], i < ed.bands.size() ? ed.bands[i] : EdBand{}});

	std::stable_sort(
		pairs.begin(),
		pairs.end(),
		[](const auto& x, const auto& y) { return x.first.start_line < y.first.start_line; });

	for (size_t i = 0; i < pairs.size(); ++i)
	{
		g.anns[i] = pairs[i].first;

		if (i < ed.bands.size()) ed.bands[i] = pairs[i].second;
	}

	Persist();
}

static bool EdSave()
{
	if (!ed.active) return false;

	std::wstring	name	= g.filePath.filename().wstring();
	std::error_code	ec;
	auto			onDisk	= fs::last_write_time(g.filePath, ec);

	if (!ec && ed.mtime && onDisk != *ed.mtime)
	{
		std::wstring msg =	name +
							L" was modified outside the editor.\n\n"
							L"Overwrite it with your version?";

		if (MessageBoxW(g.main, msg.c_str(), L"File changed on disk",
						MB_YESNO | MB_ICONWARNING) != IDYES)
		{
			return false;
		}
	}

	// atomic save (temp file + rename), so a crash mid-save can't destroy
	// the source file
	if (!core::write_file_atomic(g.filePath, core::join_source(ed.lines, ed.crlf, ed.bom)))
	{
		std::wstring msg = L"Could not write the source file:\n" + g.filePath.wstring();

		MessageBoxW(g.main, msg.c_str(), L"Save failed", MB_ICONERROR);

		return false;
	}

	auto t = fs::last_write_time(g.filePath, ec);

	ed.mtime = ec ? std::nullopt : std::optional(t);

	EdSyncAnnotations();

	ed.savedSerial	= EdStateId();
	ed.mergeOk		= false;			// later typing must not join a saved step

	EdUpdateTitle();
	FlashStatus(L"Saved " + name);

	return true;
}

// leave edit mode; false if the user cancelled (or a requested save failed)
static bool EdExit()
{
	if (!ed.active) return true;

	if (EdDirty())
	{
		std::wstring msg = L"Save changes to " + g.filePath.filename().wstring() + L"?";

		int r = MessageBoxW(g.main, msg.c_str(), L"Unsaved changes",
							MB_YESNOCANCEL | MB_ICONWARNING);

		if (r == IDCANCEL) return false;

		if (r == IDYES && !EdSave()) return false;
	}

	int keepY = ed.scrollY;

	EdHideBars();

	ed.active = false;

	ed.lines.clear();
	ed.toks.clear();
	ed.states.clear();
	ed.visW.clear();
	ed.bands.clear();
	ed.undo.clear();
	ed.redo.clear();

	ShowWindow(ed.wnd, SW_HIDE);
	ShowWindow(g.code, SW_SHOW);

	fs::path path = g.filePath;

	OpenFile(path);						// re-render the (possibly new) source

	g.scrollY = keepY;

	ClampScroll();
	UpdateScrollbars();

	SetFocus(g.code);

	InvalidateChrome();
	InvalidateCode();

	return true;
}

static void EdToggle()
{
	if (ed.active)	EdExit();
	else			EdEnter();
}

static void EdOnZoom(double frac)
{
	EdMetrics();

	ed.scrollY = (int)(frac * ed.contentH);

	EdClampScroll();
	EdUpdateScrollbars();

	if (GetFocus() == ed.wnd)
	{
		DestroyCaret();
		CreateCaret(ed.wnd, nullptr, std::max(2, S(2)), g.lineH - 2);
		EdUpdateCaret();
		ShowCaret(ed.wnd);
	}

	EdInvalidate();
}

// caret to the start of a 1-based line (find/goto/outline in edit mode)
static void EdGotoLine(int ln, int col)
{
	EdPos p = EdClampPos({ln - 1, col});

	ed.anchor	= p;
	ed.caret	= p;
	ed.mergeOk	= false;
	ed.goalVis	= -1;

	EdEnsureVisible();
	EdInvalidate();
}

// ------------------------------------------------------------ find/goto bars

static LRESULT CALLBACK BarProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	bool isFind = w == g.findBar;

	switch (msg) {
		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT	ps;
			HDC			dc = BeginPaint(w, &ps);
			RECT		rc;

			GetClientRect(w, &rc);
			FillRectColor(dc, rc, BG_PANEL);

			HBRUSH border = CreateSolidBrush(BORDER);

			FrameRect(dc, &rc, border);

			DeleteObject(border);

			if (isFind)
			{
				RECT rcount = {S(176), 0, S(244), rc.bottom};

				DrawTextColor(
					dc,
					g.findCount,
					rcount,
					g.fUiSm,
					FG_DIM,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE);

				const wchar_t* labels[] = {L"‹", L"›", L"✕"};

				for (int i = 0; i < 3; ++i)
				{
					RECT rb = {S(246) + i * S(22), S(5), S(246) + i * S(22) + S(20), rc.bottom - S(5)};

					DrawTextColor(
					dc,
					labels[i],
					rb,
					g.fUi,
					FG,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				}
			}
			else
			{
				RECT rl = {S(8), 0, S(80), rc.bottom};

				DrawTextColor(
					dc,
					L"Go to line:",
					rl,
					g.fUiSm,
					FG_DIM,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			}

			EndPaint(w, &ps);

			return 0;
		}

		case WM_LBUTTONDOWN:
		{
			if (!isFind) return 0;

			POINT	pt	= {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			RECT	rc;

			GetClientRect(w, &rc);

			for (int i = 0; i < 3; ++i)
			{
				RECT rb =
					{
						(246) + i * S(22),
						0,
						S(246) + i * S(22) + S(22),
						rc.bottom
					};

				if (PtInRect(&rb, pt))
				{
					if (i == 0) FindPrev();
					else if (i == 1) FindNext();
					else EscapeAction();
					return 0;
				}
			}
			
			return 0;
		}
		
		case WM_COMMAND:
			if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g.findEdit)
			{
				if (GetWindowString(g.findEdit) != g.lastQuery)
					FindRefresh();
			}

			return 0;

		case WM_CTLCOLOREDIT:
		{
			HDC dc = (HDC)wp;

			SetTextColor(dc, FG);
			SetBkColor(dc, BG);

			return (LRESULT) g.brBG;
		}
	}

	return DefWindowProcW(w, msg, wp, lp);
}

// -------------------------------------------------------------- main window

static void LayoutMain()
{
	RECT rc;

	GetClientRect(g.main, &rc);

	int	w		= rc.right;
	int	h		= rc.bottom;
	int	top		= S(38);
	int	bottom	= h - S(24);

	g.treeW = std::max(S(180), std::min(g.treeW, w - S(404)));

	int outlineH = S(150);
	int labelH = S(22);

	MoveWindow(
		g.tree,
		0,
		top,
		g.treeW,
		std::max(0, bottom - top - outlineH - labelH),
		TRUE);

	MoveWindow(
		g.outline,
		S(4),
		bottom - outlineH,
		g.treeW - S(8),
		outlineH - S(4),
		TRUE);

	for (HWND pane : {g.code, ed.wnd})
	{
		MoveWindow(
			pane,
			g.treeW + S(4),
			top,
			std::max(0, w - g.treeW - S(4)),
			std::max(0, bottom - top),
			TRUE);
	}

	PositionBars();

	InvalidateChrome();
}

static void PaintChrome(HDC dc, const RECT& rc)
{
	int w = rc.right, h = rc.bottom;

	// fill everything first so gaps between child windows stay dark
	FillRectColor(dc, rc, BG_PANEL);

	// --- toolbar ---
	RECT bar = {0, 0, w, S(38)};

	FillRectColor(dc, bar, BG_PANEL);

	g.rOpen = {S(8), S(6), S(128), S(32)};

	FillRectColor(dc, g.rOpen, ACCENT);
	DrawTextColor(dc, L"Open Folder…", g.rOpen, g.fUi, RGB(255, 255, 255),
				  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	// ✎ Edit / ✓ Done toggle, plus Save while editing
	g.rEdit = {g.rOpen.right + S(8), S(6), g.rOpen.right + S(84), S(32)};

	FillRectColor(dc, g.rEdit, BG_ACTIVE);
	DrawTextColor(dc, ed.active ? L"✓ Done" : L"✎ Edit", g.rEdit, g.fUi,
				  g.hasFile ? FG : FG_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	if (ed.active)
	{
		g.rSave = {g.rEdit.right + S(4), S(6), g.rEdit.right + S(72), S(32)};

		FillRectColor(dc, g.rSave, BG_ACTIVE);
		DrawTextColor(dc, L"Save", g.rSave, g.fUi, FG,
					  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	else
	{
		g.rSave = {};
	}

	int folderL = (ed.active ? g.rSave.right : g.rEdit.right) + S(10);

	int x = w - S(12);

	g.rSlider	= {x - S(110), S(12), x, S(26)};
	x			-= S(110) + S(8);

	RECT track = {g.rSlider.left, S(17), g.rSlider.right, S(21)};

	FillRectColor(dc, track, BG_ACTIVE);

	int		tx		=	g.rSlider.left +
						(int)((g.opacity - 10) / 90.0 * (g.rSlider.right - g.rSlider.left - S(12)));
	RECT	thumb	= {tx, S(12), tx + S(12), S(26)};
	HBRUSH	tb		= CreateSolidBrush(RGB(0xc0, 0xc0, 0xc0));
	HGDIOBJ	ob		= SelectObject(dc, tb);
	HGDIOBJ	op		= SelectObject(dc, GetStockObject(NULL_PEN));

	Ellipse(dc, thumb.left, thumb.top, thumb.right, thumb.bottom);

	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(tb);

	RECT rl = {x - S(52), 0, x, S(38)};

	DrawTextColor(
		dc,
		L"Opacity",
		rl,
		g.fUi,
		FG_DIM,
		DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

	x -= S(60);

	// color filter chips
	int chipsW = core::ANN_COLOR_COUNT * S(18);

	for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
	{
		const auto&	c	= core::ANN_COLORS[i];
		RECT		r	=
			{
				x - chipsW + i * S(18),
				S(11),
				x - chipsW + i * S(18) + S(16),
				S(27)
			};

		g.rChips[i] = r;

		bool	on		= g.visColors.count(c.name) != 0;
		HBRUSH	b		= CreateSolidBrush(on ? RGB(c.r, c.g, c.b)
									   : BG_PANEL);
		HGDIOBJ	ob2		= SelectObject(dc, b);
		HGDIOBJ	op2;
		HPEN	ring	= nullptr;

		if (on)
		{
			op2 = SelectObject(dc, GetStockObject(NULL_PEN));

			Ellipse(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);
		}
		else
		{
			ring	= CreatePen(PS_SOLID,
								2,
								Blend(BG_PANEL, c.r, c.g, c.b, 0.45));
			op2		= SelectObject(dc, ring);

			SelectObject(dc, GetStockObject(NULL_BRUSH));

			Ellipse(dc, r.left + S(3), r.top + S(3), r.right - S(3), r.bottom - S(3));
		}

		SelectObject(dc, ob2);
		SelectObject(dc, op2);
		DeleteObject(b);

		if (ring) DeleteObject(ring);
	}

	x			-= chipsW + S(10);

	// annotations show/hide checkbox
	g.rCheck	= {x - S(100), S(8), x, S(30)};

	RECT box = {g.rCheck.left, S(13), g.rCheck.left + S(13), S(26)};

	FillRectColor(dc, box, BG);

	HBRUSH bb = CreateSolidBrush(FG_DIM);

	FrameRect(dc, &box, bb);

	DeleteObject(bb);

	if (g.showAnns)
	{
		DrawTextColor(
			dc,
			L"✓",
			box,
			g.fUiSm,
			FG,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}

	RECT lbl = {box.right + S(6), 0, g.rCheck.right, S(38)};

	DrawTextColor(
		dc,
		L"Annotations",
		lbl,
		g.fUi,
		FG_DIM,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	// folder label fills the gap
	RECT rf = {folderL, 0, g.rCheck.left - S(10), S(38)};

	DrawTextColor(
		dc,
		g.folderText,
		rf,
		g.fUi,
		FG_DIM,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	// --- outline label ---
	int		bottom	= h - S(24);
	RECT	ro		= {0, bottom - S(150) - S(22), g.treeW, bottom - S(150)};

	FillRectColor(dc, ro, BG_PANEL);

	RECT rot = {S(8), ro.top, ro.right, ro.bottom};

	DrawTextColor(
		dc,
		L"Annotations",
		rot,
		g.fUiSmB,
		FG_DIM,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	// --- splitter ---
	RECT rs = {g.treeW, S(38), g.treeW + S(4), bottom};

	FillRectColor(dc, rs, BORDER);

	// --- status bar ---
	RECT st = {0, h - S(24), w, h};

	FillRectColor(dc, st, STATUS_BG);

	RECT sl = {S(10), st.top, w / 2, h};

	DrawTextColor(
		dc,
		g.statusFile,
		sl,
		g.fUiSm,
		RGB(255, 255, 255),
		DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	RECT sr = {w / 2, st.top, w - S(10), h};

	DrawTextColor(
		dc,
		g.statusHint,
		sr,
		g.fUiSm,
		RGB(0xcd, 0xe6, 0xf7),
		DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void ToggleColor(int i)
{
	const char* name = core::ANN_COLORS[i].name;

	if (g.visColors.count(name))
		g.visColors.erase(name);
	else
		g.visColors.insert(name);

	InvalidateChrome();
	InvalidateCode();
}

// -------------------------------------------------------------- settings

static void LoadSettings(int& x, int& y, int& w, int& h)
{
	x = CW_USEDEFAULT;
	y = CW_USEDEFAULT;
	w = 1400;
	h = 900;

	auto bytes = core::read_file_bytes(g.settingsPath);

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

	x = (int)num("x", x); y = (int)num("y", y);
	w = (int)num("w", w); h = (int)num("h", h);

	int pt = (int)num("font_size", 11);

	if (pt >= 6 && pt <= 28)
		g.fontPt = pt;

	double op = num("opacity", 55);

	if (op >= 10 && op <= 100)
		g.opacity = op;

	const json::Value* p = v.get("show_annotations");

	if (p && p->type == json::Value::Bool)
		g.showAnns = p->boolean;

	p = v.get("visible_colors");

	if (p && p->type == json::Value::Arr)
	{
		std::set<std::string> colors;

		for (const auto& c : p->arr)
		{
			if (c.type == json::Value::Str && core::ann_color(c.str))
				colors.insert(c.str);
		}

		if (!colors.empty()) g.visColors = colors;
	}

	p = v.get("folder");

	std::error_code ec;

	if (p && p->type == json::Value::Str && !p->str.empty())
	{
		fs::path folder = W(p->str);

		if (fs::is_directory(folder, ec))
			g.ws.folder = folder;			// stashed
	}
}

static void SaveSettings()
{
	WINDOWPLACEMENT wp{sizeof wp};

	GetWindowPlacement(g.main, &wp);

	RECT		r	= wp.rcNormalPosition;
	json::Value	v	= json::Value::make_obj();

	v.set("x",					json::Value::make_int(r.left));
	v.set("y",					json::Value::make_int(r.top));
	v.set("w",					json::Value::make_int(r.right - r.left));
	v.set("h",					json::Value::make_int(r.bottom - r.top));
	v.set("font_size",			json::Value::make_int(g.fontPt));
	v.set("opacity",			json::Value::make_num(g.opacity));
	v.set("show_annotations",	json::Value::make_bool(g.showAnns));

	json::Value colors = json::Value::make_arr();

	for (const auto& c : g.visColors)
		colors.arr.push_back(json::Value::make_str(c));

	v.set("visible_colors",		std::move(colors));
	v.set("folder",				json::Value::make_str(g.wsOpen ? U8(g.ws.folder.wstring()) : ""));

	core::write_file_atomic(g.settingsPath, json::dump(v, 2));
}

// ------------------------------------------------------------ main wndproc

static LRESULT CALLBACK MainProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_CREATE: {
			g.main	= w;
			g.tree	= CreateWindowExW(
				0,
				WC_TREEVIEWW,
				L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
				0,
				0,
				0,
				0,
				w,
				nullptr,
				g.hinst,
				nullptr);

			TreeView_SetBkColor(g.tree, BG_PANEL);
			TreeView_SetTextColor(g.tree, FG);
			TreeView_SetItemHeight(g.tree, S(22));

			SendMessageW(g.tree, WM_SETFONT, (WPARAM)g.fUi, TRUE);

			g.outline = CreateWindowExW(
				0,
				L"LISTBOX",
				L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_NOTIFY |
				LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
				0,
				0,
				0,
				0,
				w,
				(HMENU)(INT_PTR)100,
				g.hinst,
				nullptr);

			SendMessageW(g.outline, LB_SETITEMHEIGHT, 0, S(18));

			g.code = CreateWindowExW(
				0,
				L"CACode",
				L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL| WS_TABSTOP,
				0,
				0,
				0,
				0,
				w,
				nullptr,
				g.hinst,
				nullptr);
			ed.wnd = CreateWindowExW(
				0,
				L"CAEditor",
				L"",
				WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
				0,
				0,
				0,
				0,
				w,
				nullptr,
				g.hinst,
				nullptr);
			g.findBar = CreateWindowExW(
				0,
				L"CABar",
				L"",
				WS_CHILD | WS_CLIPSIBLINGS,
				0,
				0,
				S(316),
				S(34),
				w,
				nullptr,
				g.hinst,
				nullptr);
			g.findEdit = CreateWindowExW(
				0,
				L"EDIT",
				L"",
				WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				S(8),
				S(7),
				S(160),
				S(20),
				g.findBar,
				nullptr,
				g.hinst,
				nullptr);

			SendMessageW(g.findEdit, WM_SETFONT, (WPARAM)g.fUi, TRUE);

			g.gotoBar = CreateWindowExW(
				0,
				L"CABar",
				L"",
				WS_CHILD | WS_CLIPSIBLINGS,
				0,
				0,
				S(160),
				S(34),
				w,
				nullptr,
				g.hinst,
				nullptr);
			g.gotoEdit = CreateWindowExW(
				0,
				L"EDIT",
				L"",
				WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				S(82),
				S(7),
				S(64),
				S(20),
				g.gotoBar,
				nullptr,
				g.hinst,
				nullptr);

			SendMessageW(g.gotoEdit, WM_SETFONT, (WPARAM)g.fUi, TRUE);

			SetTimer(w, TIMER_POLL, SIDECAR_POLL_MS, nullptr);

			DefaultHint();

			return 0;
		}

		case WM_SIZE:
			LayoutMain();

			return 0;

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT: {
			PAINTSTRUCT	ps;
			HDC			dc	= BeginPaint(w, &ps);
			RECT		rc;

			GetClientRect(w, &rc);

			PaintChrome(dc, rc);
			EndPaint(w, &ps);

			return 0;
		}
		case WM_SETCURSOR:
		{
			if (LOWORD(lp) == HTCLIENT)
			{
				POINT pt;

				GetCursorPos(&pt);
				ScreenToClient(w, &pt);

				RECT rc;

				GetClientRect(w, &rc);

				RECT rs = { g.treeW, S(38), g.treeW + S(4), rc.bottom - S(24) };

				if (PtInRect(&rs, pt))
				{
					SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));

					return TRUE;
				}

				if (PtInRect(&g.rOpen, pt) || PtInRect(&g.rCheck, pt) || PtInRect(&g.rSlider, pt) ||
					PtInRect(&g.rEdit, pt) || PtInRect(&g.rSave, pt))
				{
					SetCursor(LoadCursorW(nullptr, IDC_HAND));

					return TRUE;
				}
				for (const RECT& r : g.rChips)
				{
					if (PtInRect(&r, pt))
					{
						SetCursor(LoadCursorW(nullptr, IDC_HAND));

						return TRUE;
					}
				}
			}

			break;
		}

		case WM_LBUTTONDOWN:
		{
			POINT	pt	= {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			RECT	rc;

			GetClientRect(w, &rc);

			RECT rs = {g.treeW, S(38), g.treeW + S(4), rc.bottom - S(24)};

			if (PtInRect(&g.rOpen, pt))
			{
				PickFolder();

				return 0;
			}

			if (PtInRect(&g.rEdit, pt))
			{
				EdToggle();

				return 0;
			}

			if (PtInRect(&g.rSave, pt))
			{
				EdSave();

				return 0;
			}

			if (PtInRect(&g.rCheck, pt))
			{
				g.showAnns = !g.showAnns;

				InvalidateChrome();
				InvalidateCode();

				return 0;
			}

			for (int i = 0; i < core::ANN_COLOR_COUNT; ++i)
			{
				if (PtInRect(&g.rChips[i], pt))
				{
					ToggleColor(i);

					return 0;
				}
			}

			if (PtInRect(&g.rSlider, pt))
			{
				g.sliderDrag = true;

				SetCapture(w);
				// fall through to move handling below
			}
			else if (PtInRect(&rs, pt))
			{
				g.splitDrag = true;

				SetCapture(w);

				return 0;
			}
			// fallthrough for slider press: update immediately
			if (g.sliderDrag)
			{
				double f =
					(double)(pt.x - g.rSlider.left) /
					std::max(1, (int)(g.rSlider.right - g.rSlider.left));

				g.opacity = std::max(10.0, std::min(100.0, 10 + 90 * f));

				InvalidateChrome();
				InvalidateCode();
			}

			return 0;
		}

		case WM_MOUSEMOVE:
		{
			if (GetCapture() != w) return 0;

			POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

			if (g.sliderDrag)
			{
				double f =
					(double)(pt.x - g.rSlider.left) /
					std::max(1, (int)(g.rSlider.right - g.rSlider.left));

				g.opacity = std::max(10.0, std::min(100.0, 10 + 90 * f));

				InvalidateChrome();
				InvalidateCode();
			}
			else if (g.splitDrag)
			{
				g.treeW = pt.x;

				LayoutMain();
			}

			return 0;
		}

		case WM_LBUTTONUP:
			if (GetCapture() == w) ReleaseCapture();

			g.sliderDrag = g.splitDrag = false;

			return 0;

		case WM_NOTIFY:
		{
			NMHDR* hdr = (NMHDR*)lp;

			if (hdr->hwndFrom == g.tree)
			{
				if (hdr->code == TVN_ITEMEXPANDINGW)
				{
					NMTREEVIEWW* tv = (NMTREEVIEWW*)lp;

					if (tv->action == TVE_EXPAND)
					{
						HTREEITEM	item	= tv->itemNew.hItem;
						HTREEITEM	child	= TreeView_GetChild(g.tree, item);
						TreeData*	cd		= child ? DataOf(child) : nullptr;

						if (cd && cd->kind == 2 && !TreeView_GetNextSibling(g.tree, child))
						{
							TreeView_DeleteItem(g.tree, child);

							TreeData* d = DataOf(item);

							if (d)
								PopulateDir(item, fs::path(d->path));
						}
					}

					return FALSE;
				}

				if (hdr->code == NM_CUSTOMDRAW)
				{
					NMTVCUSTOMDRAW* cd = (NMTVCUSTOMDRAW*)lp;

					if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
						return CDRF_NOTIFYITEMDRAW;

					if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
					{
						if (cd->nmcd.uItemState & CDIS_SELECTED)
						{
							cd->clrTextBk	= BG_ACTIVE;
							cd->clrText		= RGB(255, 255, 255);
						}
						else
						{
							cd->clrTextBk	= BG_PANEL;
							cd->clrText		= FG;
						}

						return CDRF_DODEFAULT;
					}

					return CDRF_DODEFAULT;
				}

				if (hdr->code == TVN_SELCHANGINGW && ed.active)
				{
					NMTREEVIEWW*	tv	= (NMTREEVIEWW*)lp;
					TreeData*		d	=
						tv->itemNew.hItem ? DataOf(tv->itemNew.hItem) : nullptr;

					// returning TRUE keeps the selection (and the editor)
					if (d && d->kind == 1 && !EdExit())
						return TRUE;

					return FALSE;
				}

				if (hdr->code == TVN_SELCHANGEDW)
				{
					NMTREEVIEWW*	tv	= (NMTREEVIEWW*)lp;
					TreeData*		d	=
						tv->itemNew.hItem ? DataOf(tv->itemNew.hItem) : nullptr;

					if (d && d->kind == 1)
						OpenFile(fs::path(d->path));

					return 0;
				}
			}

			return 0;
		}

		case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;

			if (dis->hwndItem != g.outline) break;

			FillRectColor(
				dis->hDC,
				dis->rcItem,
				(dis->itemState & ODS_SELECTED) ? BG_ACTIVE : BG_PANEL);

			if (dis->itemID == (UINT)-1 || dis->itemID >= g.anns.size())
				return TRUE;

			wchar_t buf[256];

			SendMessageW(g.outline, LB_GETTEXT, dis->itemID, (LPARAM)buf);

			const auto*	c	= AnnRGB(g.anns[dis->itemID]);
			RECT		r	= dis->rcItem;

			r.left += S(4);

			DrawTextColor(
				dis->hDC,
				buf,
				r,
				g.fUiSm,
				RGB(c->r, c->g, c->b),
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			return TRUE;
		}

		case WM_CTLCOLORLISTBOX:
		{
			HDC dc = (HDC)wp;

			SetTextColor(dc, FG);
			SetBkColor(dc, BG_PANEL);

			return (LRESULT)g.brPanel;
		}

		case WM_COMMAND:
			if ((HWND)lp == g.outline && HIWORD(wp) == LBN_SELCHANGE)
			{
				int sel = (int)SendMessageW(g.outline, LB_GETCURSEL, 0, 0);

				if (sel >= 0) ShowAnnotation(sel);
			}

			return 0;

		case WM_TIMER:
			if (wp == TIMER_POLL) PollSidecar();
			else if (wp == TIMER_FLASH) ClearFlash();
			else if (wp == TIMER_ANNFLASH)
			{
				KillTimer(w, TIMER_ANNFLASH);

				g.flashIdx = -1;

				InvalidateCode();
			}

			return 0;

		case WM_CLOSE:
			if (!EdExit()) return 0;		// user cancelled the close

			SaveSettings();

			DestroyWindow(w);

			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);

			return 0;
	}

	return DefWindowProcW(w, msg, wp, lp);
}

// ------------------------------------------------------------ message loop

static bool PreTranslate(MSG& m)
{
	if (m.message == WM_MOUSEWHEEL)
	{
		// route the wheel to the pane under the cursor
		POINT pt = {GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
		HWND target = WindowFromPoint(pt);
		DWORD pid = 0;

		if (target && GetWindowThreadProcessId(target, &pid) == GetCurrentThreadId())
			m.hwnd = target;

		return false;
	}

	if (m.message == WM_SYSKEYDOWN &&(m.wParam == VK_UP || m.wParam == VK_DOWN))
	{
		GotoAnnotation(m.wParam == VK_UP ? -1 : 1);

		return true;
	}

	if (m.message != WM_KEYDOWN) return false;

	bool ctrl = GetKeyState(VK_CONTROL) < 0;
	bool shift = GetKeyState(VK_SHIFT) < 0;
	HWND focus = GetFocus();

	if (ctrl && m.wParam == 'S')
	{
		if (ed.active) EdSave();

		return true;
	}

	// the editor handles its own clipboard / undo / select-all keys
	if (ctrl && focus == ed.wnd && ed.active &&
		wcschr(L"ACVXYZ", (wchar_t)m.wParam))
	{
		return false;
	}

	switch (m.wParam)
	{
		case VK_ESCAPE:
			EscapeAction();

			return true;

		case VK_F3:
			shift ? FindPrev() : FindNext();

			return true;

		case VK_RETURN:
			if (focus == g.findEdit)
			{
				shift ? FindPrev() : FindNext();

				return true;
			}

			if (focus == g.gotoEdit)
			{
				GotoLineAction();

				return true;
			}

			break;
	}

	if (ctrl) {
		switch (m.wParam) {
			case 'F':			OpenFindBar();							return true;
			case 'G':			OpenGotoBar();							return true;
			case 'C':			if (!FocusInEdit()) { CopySelection();	return true; }	break;
			case 'Z':			if (!FocusInEdit()) { UndoDelete();		return true; }	break;
			case VK_OEM_PLUS:
			case VK_ADD:		Zoom(1);								return true;
			case VK_OEM_MINUS:
			case VK_SUBTRACT:	Zoom(-1);								return true;
			case '0':
			case VK_NUMPAD0:	Zoom(0);								return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- wWinMain

static void SetupConsole()
{
	BOOL	attached	= AttachConsole(ATTACH_PARENT_PROCESS);
	HANDLE	h			= GetStdHandle(STD_OUTPUT_HANDLE);

	if (attached && (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN))
	{
		FILE* f = nullptr;

		freopen_s(&f, "CONOUT$", "w", stdout);
	}
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show)
{
	g.hinst = hinst;

	int			argc	= 0;
	wchar_t**	argv	= CommandLineToArgvW(GetCommandLineW(), &argc);
	bool		check	= false;
	bool		fix 	= false;
	fs::path	folder;
	fs::path	annRoot;

	for (int i = 1; i < argc; ++i)
	{
		std::wstring arg = argv[i];

		if (arg == L"--check")		check	= true;
		else if (arg == L"--fix")	fix		= true;
		else if ((arg == L"-a" || arg == L"--annotations") && i + 1 < argc)
			annRoot = argv[++i];
		else if (!arg.empty() && arg[0] != L'-')
			folder = arg;
	}

	LocalFree(argv);

	if (check)
	{
		SetupConsole();

		fs::path	target	= folder.empty() ? fs::path(L".") : folder;
		int			code	=
			core::check_annotations(target, fix, annRoot.empty() ? nullptr : &annRoot);
		fflush(stdout);

		return code;
	}

	SetProcessDPIAware();

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES };

	InitCommonControlsEx(&icc);

	HDC screen = GetDC(nullptr);

	g.dpi		= GetDeviceCaps(screen, LOGPIXELSY);

	ReleaseDC(nullptr, screen);

	g.pad		= S(8);
	g.treeW		= S(300);
	g.brBG		= CreateSolidBrush(BG);
	g.brPanel	= CreateSolidBrush(BG_PANEL);

	for (const auto& c : core::ANN_COLORS)
		g.visColors.insert(c.name);

	const wchar_t* profile = _wgetenv(L"USERPROFILE");

	g.settingsPath = fs::path(profile ? profile : L".") / L".code_annotator_cpp.json";
	if (!annRoot.empty()) g.ws.ann_root_arg = annRoot;

	int x, y, ww, wh;

	LoadSettings(x, y, ww, wh);

	CreateFonts();

	WNDCLASSW wc{};

	wc.hInstance		= hinst;
	wc.hCursor			= LoadCursorW(nullptr, IDC_ARROW);
	wc.lpfnWndProc		= MainProc;
	wc.lpszClassName	= L"CAMain";

	RegisterClassW(&wc);

	wc.lpfnWndProc		= CodeProc;
	wc.lpszClassName	= L"CACode";

	RegisterClassW(&wc);

	wc.lpfnWndProc		= EditorProc;
	wc.lpszClassName	= L"CAEditor";
	wc.hCursor			= LoadCursorW(nullptr, IDC_IBEAM);
	wc.style			= CS_DBLCLKS;

	RegisterClassW(&wc);

	wc.hCursor			= LoadCursorW(nullptr, IDC_ARROW);
	wc.style			= 0;

	wc.lpfnWndProc		= BarProc;
	wc.lpszClassName	= L"CABar";

	RegisterClassW(&wc);

	wc.lpfnWndProc		= DlgProc;
	wc.lpszClassName	= L"CADlg";

	RegisterClassW(&wc);

	HWND main = CreateWindowExW(
		0,
		L"CAMain",
		L"Code Annotator",
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x,
		y,
		ww,
		wh,
		nullptr,
		nullptr,
		hinst,
		nullptr);

	ShowWindow(main, show);

	// folder priority: command line, then the remembered one from settings
	std::error_code ec;

	if (!folder.empty() && fs::is_directory(folder, ec))
		OpenFolder(folder);
	else if (!g.ws.folder.empty() && fs::is_directory(g.ws.folder, ec))
		OpenFolder(g.ws.folder);

	SetFocus(g.code);

	MSG m;

	while (GetMessageW(&m, nullptr, 0, 0))
	{
		if (PreTranslate(m)) continue;

		TranslateMessage(&m);
		DispatchMessageW(&m);
	}

	CoUninitialize();

	return (int)m.wParam;
}
