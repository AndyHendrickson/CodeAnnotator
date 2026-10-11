// highlight.hpp — line-oriented syntax tokenizer of the Code Annotator
// C++ port. Mirrors the scanner and language tables in code_annotator.py.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace hl {

enum class Tok { None, Com, Str, Num, Kw, Lit, Typ, Fn, Tag, Attr };

struct Token {
	int				col;			// 0-based column (UTF-16 units)
	std::wstring	text;
	Tok				cls;
};

struct LangCfg {
	std::unordered_map<std::wstring, Tok>	words;
	std::wstring	strings	= L"'\"";
	std::wstring	line;					// line-comment prefix; empty = none
	std::wstring	block_start;
	std::wstring	block_end;
	bool			triple		= false;	// Python-style ''' / """ strings
	bool			tmpl		= false;	// JS-style ` template strings
	bool			casefold	= false;	// case-insensitive keywords (SQL)
	bool			html		= false;	// use the HTML/XML scanner
};

// language id for a file name ("py", "js", ... or "plain")
std::string lang_for(const std::wstring& filename);

// nullptr for "plain" and unknown ids
const LangCfg* lang_cfg(const std::string& lang_id);

struct ScanState {
	enum Mode { NoneM, Block, Str, HtmlComment, HtmlTag };

	Mode			mode	= NoneM;
	std::wstring	delim;
};

std::vector<Token> scan_line(const std::wstring&	line,
							 const LangCfg&			cfg,
							 ScanState&				st);
std::vector<Token> scan_html_line(const std::wstring& line, ScanState& st);

// per-line token lists for the whole file
std::vector<std::vector<Token>> highlight_lines(
	const std::vector<std::wstring>&	lines,
	const std::string&					lang_id);

}  // namespace hl
