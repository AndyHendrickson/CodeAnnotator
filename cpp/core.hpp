// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
// core.hpp — non-GUI logic of the Code Annotator C++ port: annotation
// sidecar files, whitespace-insensitive anchors, the parallel annotation
// tree, and the --check/--fix CLI. Mirrors code_annotator.py.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core {

namespace fs = std::filesystem;

inline constexpr const char* SIDECAR_SUFFIX = ".ann.json";
inline constexpr const char* ANN_DIR_NAME = ".annotations";
inline constexpr size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
inline constexpr int MAX_LINES = 8000;
inline constexpr int TAB_WIDTH = 4;

// annotation highlight palette, in toolbar/dialog display order
struct AnnColor
{
	const char*	name	= nullptr;
	int			r		= 0;
	int			g		= 0;
	int			b		= 0;
};

inline constexpr		AnnColor ANN_COLORS[]	=
{
	{ "yellow",	230,	190,	 50 },
	{ "green",	 80,	200,	120 },
	{ "blue",	 70,	150,	255 },
	{ "pink",	240,	100,	170 },
	{ "purple",	170,	110,	255 },
	{ "orange",	250,	140,	 60 },
};

inline constexpr int	ANN_COLOR_COUNT			= 6;

const AnnColor* ann_color(const std::string& name);	  // nullptr when unknown

struct Annotation {
	std::string	id;					// 8 hex chars
	int			start_line = 1;		// 1-based, inclusive
	int			end_line = 1;
	std::string	color = "yellow";
	std::string	text;				// UTF-8, '\n' newlines
	bool		orphaned = false;
	std::string	anchor;				// UTF-8, whitespace-collapsed source line
};

// ------------------------------------------------------------- text helpers

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// whitespace-insensitive fingerprint of a source line (UTF-8)
std::string anchor_text(const std::wstring& line);

// file bytes -> display lines (UTF-8 decoded, tabs expanded); [""] when empty
std::vector<std::wstring> decode_source(const std::string& bytes);
std::optional<std::vector<std::wstring>> read_source_lines(const fs::path& p);

std::optional<std::string> read_file_bytes(const fs::path& p);
bool write_file_atomic(const fs::path& p, const std::string& bytes);

std::string new_id();									// 8 random hex chars

// ------------------------------------------------------------- annotations

std::optional<fs::path> find_git_root(const fs::path& start);

struct AnnLoad
{
	bool					ok		= true;
	std::string				error;		// set when a sidecar exists but is corrupt
	std::vector<Annotation>	anns;
};

// []/ok when the sidecar doesn't exist; ok=false when it exists but cannot
// be parsed — callers must not mistake a corrupt file for an empty one
AnnLoad load_annotations(const fs::path& ann_path);

// write (or remove, when empty) one sidecar; prunes empty dirs up to
// (but not including) prune_root. Returns false on I/O failure.
bool save_annotations(
	const fs::path&					ann_path,
	const std::string&				rel_name,
	const std::vector<Annotation>&	anns,
	const fs::path*					prune_root
);

// re-locate annotations by their anchors after the source changed outside
// the app; returns (repaired, lost)
std::pair<int, int> reconcile_annotations(
	std::vector<Annotation>&			anns,
	const std::vector<std::wstring>&	lines
);

void sort_annotations(std::vector<Annotation>& anns);

// --------------------------------------------------------------- edit mode

bool is_valid_utf8(const std::string& bytes);

// the source as the in-app editor sees it: strict UTF-8, newlines
// normalized to '\n' and split into raw lines (tabs kept). Joining the
// lines with '\n' reproduces the text, so a trailing newline shows up as a
// final empty line.
struct EditSource
{
	enum Status { Ok, Unreadable, NotUtf8 };

	Status						status	= Ok;
	std::vector<std::wstring>	lines;
	bool						crlf	= false;	// file used "\r\n" newlines
	bool						bom		= false;	// file began with a UTF-8 BOM
};

EditSource load_source_for_edit(const fs::path& p);

// split text on "\r\n", "\r" or "\n" into raw lines (always at least one)
std::vector<std::wstring> split_source_lines(const std::wstring& text);

// inverse of load_source_for_edit: re-applies the newline style and BOM
std::string join_source(const std::vector<std::wstring>& lines, bool crlf, bool bom);

// re-anchor an annotation from its region as tracked through in-app edits.
// The region now spans the half-open range from (start_line0, 0) to
// (end_line0, end_col), 0-based; empty means the whole region was deleted,
// which collapses the annotation to start_line0 and orphans it. Returns
// true if anything changed.
bool rebase_annotation(
	Annotation&							a,
	int									start_line0,
	int									end_line0,
	int									end_col,
	bool								empty,
	const std::vector<std::wstring>&	lines
);

// --------------------------------------------------------------- workspace

struct Workspace
{
	fs::path	folder;
	fs::path	source_base;			// git repo root, or the folder itself
	fs::path	ann_root;
	fs::path	ann_root_arg;			// -a override; empty = default

	void		open(const fs::path& f);
	std::string	rel_name(const fs::path& file) const;		// forward slashes
	fs::path	sidecar_for(const fs::path& file) const;
	bool		save(const fs::path& file, const std::vector<Annotation>& anns) const;
};

// ------------------------------------------------------------------- check

// validate every sidecar against its source, print a report to stdout;
// with fix, repairable files are rewritten. Returns a process exit code.
int check_annotations(const fs::path& folder, bool fix, const fs::path* ann_root);

}  // namespace core
