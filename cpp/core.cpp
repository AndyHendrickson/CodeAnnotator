// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
// core.cpp — see core.hpp. Port of the non-GUI half of code_annotator.py.
// Platform-independent: shared by the Win32 GUI (../cpp/gui.cpp) and the
// macOS GUI (../macos/main.mm).
#include "core.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <random>
#include <sstream>
#include <string_view>

namespace core {

const AnnColor* ann_color(const std::string& name)
{
	for (const auto& c : ANN_COLORS)
		if (name == c.name)
			return &c;

	return nullptr;
}

// ------------------------------------------------------------- text helpers

// hand-rolled UTF-8 <-> wchar_t conversion so the core has no platform
// dependencies; invalid input becomes U+FFFD. Handles both 16-bit
// (Windows, surrogate pairs) and 32-bit (POSIX) wchar_t.

std::wstring utf8_to_wide(const std::string& s)
{
	std::wstring	out;
	size_t			i	= 0;

	out.reserve(s.size());

	while (i < s.size())
	{
		unsigned char	c	= (unsigned char)s[i];
		uint32_t		cp	= 0xFFFD;
		int				len	= 1;

		if (      c < 0x80)				{ cp = c; }
		else if ((c & 0xE0) == 0xC0)	{ cp = c & 0x1F;	len = 2; }
		else if ((c & 0xF0) == 0xE0)	{ cp = c & 0x0F;	len = 3; }
		else if ((c & 0xF8) == 0xF0)	{ cp = c & 0x07;	len = 4; }

		if (len > 1)
		{
			if (i + len > s.size())
			{
				cp = 0xFFFD;
				len = 1;
			}
			else
			{
				for (int k = 1; k < len; ++k)
				{
					unsigned char	cc	= (unsigned char)s[i + k];

					if ((cc & 0xC0) != 0x80)
					{
						cp = 0xFFFD;
						len = 1; break;
					}

					cp = (cp << 6) | (cc & 0x3F);
				}
			}
		}

		if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
			cp = 0xFFFD;

		i += (size_t)len;

		if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
			cp	-= 0x10000;

			out	+= (wchar_t)(0xD800 + (cp >> 10));
			out	+= (wchar_t)(0xDC00 + (cp & 0x3FF));
		}
		else
		{
			out	+= (wchar_t)cp;
		}
	}

	return out;
}

std::string wide_to_utf8(const std::wstring& s)
{
	std::string out;

	out.reserve(s.size());

	for (size_t i = 0; i < s.size(); ++i)
	{
		uint32_t	cp	= (uint32_t)s[i];
		
		if (sizeof(wchar_t) == 2 &&
			cp >= 0xD800 &&
			cp <= 0xDBFF &&
			i + 1 < s.size())
		{
			uint32_t	lo	= (uint32_t)s[i + 1];

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);

				++i;
			}
		}

		if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
			cp = 0xFFFD;

		if (cp < 0x80)
		{
			out += (char)cp;
		}
		else if (cp < 0x800)
		{
			out += (char)(0xC0 | (cp >>  6));
			out += (char)(0x80 | (cp			& 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += (char)(0xE0 |  (cp >> 12));
			out += (char)(0x80 | ((cp >>  6)	& 0x3F));
			out += (char)(0x80 |  (cp 			& 0x3F));
		}
		else
		{
			out += (char)(0xF0 |  (cp >> 18));
			out += (char)(0x80 | ((cp >> 12)	& 0x3F));
			out += (char)(0x80 | ((cp >>  6)	& 0x3F));
			out += (char)(0x80 |  (cp			& 0x3F));
		}
	}

	return out;
}

std::string anchor_text(const std::wstring& line)
{
	std::wstring	collapsed;
	bool			in_word		= false;

	for (wchar_t c : line)
	{
		if (iswspace(c))
		{
			in_word		= false;
		}
		else
		{
			if (!in_word && !collapsed.empty()) collapsed += L' ';
			
			collapsed	+= c;
			in_word		= true;
		}
	}

	return wide_to_utf8(collapsed);
}

static std::wstring expand_tabs(const std::wstring& line)
{
	if (line.find(L'\t') == std::wstring::npos) return line;

	std::wstring out;

	out.reserve(line.size() + 8);

	for (wchar_t c : line)
	{
		if (c == L'\t')
		{
			size_t next = (out.size() / TAB_WIDTH + 1) * TAB_WIDTH;

			out.append(next - out.size(), L' ');
		}
		else
		{
			out += c;
		}
	}
	return out;
}

std::vector<std::wstring> decode_source(const std::string& bytes)
{
	std::wstring				text	= utf8_to_wide(bytes);		// invalid bytes become U+FFFD
	std::vector<std::wstring>	lines;
	std::wstring				cur;

	for (size_t i = 0; i < text.size(); ++i)
	{
		wchar_t c = text[i];

		if (c == L'\r')
		{
			if (i + 1 < text.size() && text[i + 1] == L'\n')
				++i;

			lines.push_back(expand_tabs(cur));
			cur.clear();
		}
		else if (c == L'\n')
		{
			lines.push_back(expand_tabs(cur));
			cur.clear();
		}
		else
		{
			cur += c;
		}
	}

	if (!cur.empty()) lines.push_back(expand_tabs(cur));

	if (lines.empty()) lines.push_back(L"");

	return lines;
}

std::optional<std::string> read_file_bytes(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);

	if (!f) return std::nullopt;

	std::ostringstream ss;

	ss << f.rdbuf();

	if (f.bad()) return std::nullopt;

	return ss.str();
}

std::optional<std::vector<std::wstring>> read_source_lines(const fs::path& p)
{
	auto bytes = read_file_bytes(p);

	if (!bytes) return std::nullopt;

	return decode_source(*bytes);
}

bool write_file_atomic(const fs::path& p, const std::string& bytes)
{
	fs::path tmp = p;

	tmp += ".tmp~";

	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);

		if (!f) return false;

		f.write(bytes.data(), (std::streamsize)bytes.size());

		if (!f) return false;
	}

	std::error_code ec;

	fs::rename(tmp, p, ec);					   // atomic replace on Windows

	if (ec)
	{
		fs::remove(tmp, ec);

		return false;
	}

	return true;
}

std::string new_id()
{
	static std::mt19937	rng{std::random_device{}()};
	static const char*	hex		= "0123456789abcdef";
	std::string			id(8, '0');

	for (auto& c : id)
		c = hex[rng() & 0xF];

	return id;
}

// ------------------------------------------------------------- annotations

std::optional<fs::path> find_git_root(const fs::path& start)
{
	std::error_code	ec;
	fs::path		cur	= fs::absolute(start, ec).lexically_normal();

	while (true)
	{
		if (fs::exists(cur / ".git", ec)) return cur;

		fs::path parent = cur.parent_path();

		if (parent == cur || parent.empty()) return std::nullopt;

		cur = parent;
	}
}

void sort_annotations(std::vector<Annotation>& anns)
{
	std::stable_sort(
		anns.begin(),
		anns.end(),
		[](const Annotation& a, const Annotation& b) {
			return a.start_line < b.start_line;
		}
	);
}

static bool json_int(const json::Value* v, int& out)
{
	if (!v) return false;

	if (v->type == json::Value::Num)
	{
		out = (int)v->number;

		return true;
	}

	if (v->type == json::Value::Str)			 // int("10") also works in py
	{
		try {
			size_t	used	= 0;
			int		n		= std::stoi(v->str, &used);

			if (used != v->str.size()) return false;

			out = n;

			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	return false;
}

AnnLoad load_annotations(const fs::path& ann_path)
{
	AnnLoad			result;
	std::error_code	ec;

	if (!fs::is_regular_file(ann_path, ec)) return result;	 // absent: []

	auto bytes = read_file_bytes(ann_path);

	if (!bytes)
	{
		result.ok		= false;
		result.error	= ann_path.string() + ": could not read file";

		return result;
	}

	json::Value data;

	if (!json::parse(*bytes, data) || data.type != json::Value::Obj)
	{
		result.ok		= false;
		result.error	= ann_path.string() + ": invalid JSON";

		return result;
	}

	const json::Value* entries = data.get("annotations");

	if (!entries || entries->type != json::Value::Arr) return result;

	for (const auto& e : entries->arr)
	{
		if (e.type != json::Value::Obj) continue;

		Annotation	a;
		int			start;
		int			end;

		if (!json_int(e.get("startLine"), start) || !json_int(e.get("endLine"), end))
			continue;

		if (start < 1) continue;

		a.start_line	= start;
		a.end_line		= std::max(start, end);

		const json::Value*	v	= e.get("text");

		if (v && v->type == json::Value::Str)
			a.text = v->str;

		// normalize newlines
		std::string t;

		t.reserve(a.text.size());

		for (size_t i = 0; i < a.text.size(); ++i)
		{
			if (a.text[i] == '\r')
			{
				if (i + 1 < a.text.size() && a.text[i + 1] == '\n')
					++i;

				t += '\n';
			}
			else
			{
				t += a.text[i];
			}
		}

		a.text		= std::move(t);
		v			= e.get("id");
		a.id		=	(v && v->type == json::Value::Str && !v->str.empty()) ?
							v->str :
							new_id();
		v			= e.get("color");
		a.color		=	(v && v->type == json::Value::Str && ann_color(v->str)) ?
							v->str :
							"yellow";
		v			= e.get("orphaned");
		a.orphaned	= v &&
						((v->type == json::Value::Bool && v->boolean) ||
							(v->type == json::Value::Num && v->number != 0));
		v			= e.get("anchor");

		if (v && v->type == json::Value::Str)
			a.anchor = v->str;

		result.anns.push_back(std::move(a));
	}

	sort_annotations(result.anns);

	return result;
}

// case-insensitive on Windows (like the file system), sensitive elsewhere
static bool component_equal(const std::wstring& a, const std::wstring& b)
{
#ifdef _WIN32
	if (a.size() != b.size()) return false;

	for (size_t i = 0; i < a.size(); ++i)
		if (towlower(a[i]) != towlower(b[i]))
			return false;

	return true;
#else
	return a == b;
#endif
}

// is child strictly inside root? (component-wise, lexical)
static bool path_within(const fs::path& child, const fs::path& root)
{
	fs::path	c	= child.lexically_normal();
	fs::path	r	= root.lexically_normal();
	auto		ci	= c.begin();

	for (auto ri = r.begin(); ri != r.end(); ++ri)
	{
		if (ri->native().empty() || *ri == fs::path("."))
			continue;				// trailing-separator / dot elements

		if (ci == c.end()) return false;

		if (!component_equal(ci->wstring(), ri->wstring())) return false;

		++ci;
	}

	return ci != c.end();
}

bool save_annotations(
	const fs::path&					ann_path,
	const std::string&				rel_name,
	const std::vector<Annotation>&	anns,
	const fs::path*					prune_root)
{
	std::error_code ec;

	if (anns.empty())
	{
		fs::remove(ann_path, ec);

		if (prune_root)
		{
			fs::path	root	= fs::absolute(*prune_root, ec).lexically_normal();
			fs::path	d		= fs::absolute(ann_path.parent_path(), ec).lexically_normal();

			while (d != root && path_within(d, root))
			{
				if (!fs::is_directory(d, ec) || !fs::is_empty(d, ec))
					break;

				if (!fs::remove(d, ec) || ec)
					break;

				d = d.parent_path();
			}
		}

		return true;
	}

	fs::create_directories(ann_path.parent_path(), ec);

	json::Value entries = json::Value::make_arr();

	for (const auto& a : anns)			// optional fields only written when set
	{
		json::Value e = json::Value::make_obj();

		e.set("id",			json::Value::make_str(a.id));
		e.set("startLine",	json::Value::make_int(a.start_line));
		e.set("endLine",	json::Value::make_int(a.end_line));
		e.set("color",		json::Value::make_str(a.color));
		e.set("text",		json::Value::make_str(a.text));

		if (a.orphaned) e.set("orphaned", json::Value::make_bool(true));

		if (!a.anchor.empty()) e.set("anchor", json::Value::make_str(a.anchor));

		entries.arr.push_back(std::move(e));
	}

	json::Value data = json::Value::make_obj();

	data.set("version",		json::Value::make_int(1));
	data.set("file",		json::Value::make_str(rel_name));
	data.set("annotations",	std::move(entries));

	// Python writes via text mode, so sidecars use CRLF on Windows — match
	// it to keep files byte-identical between the two implementations
	std::string text = json::dump(data, 2);
	std::string crlf;

	crlf.reserve(text.size() + 64);

	for (char c : text)
	{
		if (c == '\n')
			crlf += "\r\n";
		else
			crlf += c;
	}

	return write_file_atomic(ann_path, crlf);
}

std::pair<int, int> reconcile_annotations(
	std::vector<Annotation>&			anns,
	const std::vector<std::wstring>&	lines)
{
	int	repaired	= 0;
	int	lost		= 0;
	int	n			= (int)lines.size();

	for (auto& a : anns)
	{
		if (a.anchor.empty())
		{
			if (!a.orphaned && a.start_line >= 1 && a.start_line <= n)
				a.anchor = anchor_text(lines[(size_t)a.start_line - 1]);

			continue;
		}

		int idx = a.start_line - 1;

		if (idx >= 0 && idx < n && anchor_text(lines[(size_t)idx]) == a.anchor)
			continue;

		int best = 0;							   // 0 = no candidate

		for (int i = 0; i < n; ++i)
		{
			if (anchor_text(lines[(size_t)i]) != a.anchor) continue;

			int ln = i + 1;

			if (!best || std::abs(ln - a.start_line) < std::abs(best - a.start_line))
				best = ln;						   // ties keep the earlier line
		}

		if (best)
		{
			int span = a.end_line - a.start_line;

			a.start_line	= best;
			a.end_line		= std::min(best + span, n);
			a.orphaned		= false;

			++repaired;
		}
		else if (!a.orphaned)
		{
			a.orphaned		= true;
			a.start_line	= std::min(a.start_line, n);
			a.end_line		= std::min(a.end_line, n);

			++lost;
		}
	}

	sort_annotations(anns);

	return {repaired, lost};
}

// --------------------------------------------------------------- workspace

void Workspace::open(const fs::path& f)
{
	std::error_code ec;

	folder = fs::absolute(f, ec).lexically_normal();

	// annotation paths mirror the source tree relative to the git repo root
	// (when inside one) so the same tree serves the whole repo
	auto git = find_git_root(folder);

	source_base = git ? *git : folder;

	ann_root =	!ann_root_arg.empty() ?
					fs::absolute(ann_root_arg, ec).lexically_normal() :
					source_base / ANN_DIR_NAME;
}

std::string Workspace::rel_name(const fs::path& file) const
{
	std::error_code	ec;
	fs::path		abs	= fs::absolute(file, ec).lexically_normal();

	return abs.lexically_relative(source_base).generic_string();
}

fs::path Workspace::sidecar_for(const fs::path& file) const
{
	std::error_code	ec;
	fs::path		abs	= fs::absolute(file, ec).lexically_normal();
	fs::path		rel	= abs.lexically_relative(source_base);
	fs::path		out	= ann_root / rel;

	out += SIDECAR_SUFFIX;

	return out;
}

bool Workspace::save(
	const fs::path&					file,
	const std::vector<Annotation>&	anns) const
{
	return save_annotations(sidecar_for(file), rel_name(file), anns, &ann_root);
}

// ------------------------------------------------------------------- check

static void walk_sidecars(const fs::path& dir, std::vector<fs::path>& out)
{
	std::error_code			ec;
	std::vector<fs::path>	dirs;
	std::vector<fs::path>	files;

	// NOTE: The default constructor makes an 'end iterator', which is what the regular iterator matches when it is at the end.
	for (fs::directory_iterator it(dir, ec), end;
		 !ec && it != end;
		 it.increment(ec))
	{
		if (it->is_directory(ec)) dirs.push_back(it->path());
		else files.push_back(it->path());
	}

	auto by_name =
		[](const fs::path& a, const fs::path& b)
		{
			std::wstring	x	= a.filename().wstring();
			std::wstring	y	= b.filename().wstring();
			size_t			n	= std::min(x.size(), y.size());

			for (size_t i = 0; i < n; ++i)
			{
				wchar_t	cx	= (wchar_t)towlower(x[i]);
				wchar_t	cy	= (wchar_t)towlower(y[i]);

				if (cx != cy) return cx < cy;
			}

			return x.size() < y.size();
		};

	std::sort(files.begin(), files.end(), by_name);
	std::sort(dirs.begin(), dirs.end(), by_name);

	for (const auto& f : files)
	{
		std::string	name	= f.filename().string();
		size_t		sfx		= std::strlen(SIDECAR_SUFFIX);

		if (name.size() > sfx &&
			name.compare(name.size() - sfx, sfx, SIDECAR_SUFFIX) == 0)
		{
			out.push_back(f);
		}
	}

	for (const auto& d : dirs)
		walk_sidecars(d, out);
}

int check_annotations(const fs::path& folder, bool fix, const fs::path* ann_root)
{
	Workspace ws;

	if (ann_root) ws.ann_root_arg = *ann_root;

	ws.open(folder);

	std::error_code ec;

	if (!fs::is_directory(ws.ann_root, ec))
	{
		std::printf("no annotation tree at %s\n", ws.ann_root.string().c_str());

		return 0;
	}

	int						files		= 0;
	int						problems	= 0;
	int						unfixable	= 0;
	std::vector<fs::path>	sidecars;

	walk_sidecars(ws.ann_root, sidecars);

	for (const auto& ann_path : sidecars)
	{
		++files;

		fs::path	rel		= ann_path.lexically_relative(ws.ann_root);
		std::string	rel_str	= rel.string();

		rel_str.resize(rel_str.size() - std::strlen(SIDECAR_SUFFIX));

		std::string	label	= rel.generic_string();

		label.resize(label.size() - std::strlen(SIDECAR_SUFFIX));

		fs::path	src		= ws.source_base / fs::path(rel_str);
		AnnLoad		loaded	= load_annotations(ann_path);

		if (!loaded.ok)
		{
			std::printf("ERROR	%s: unparseable annotation file (%s)\n",
						label.c_str(),
						loaded.error.c_str());

			++problems;
			++unfixable;

			continue;
		}

		if (!fs::is_regular_file(src, ec))
		{
			std::printf("ERROR	%s: source file is missing\n", label.c_str());

			++problems;
			++unfixable;

			continue;
		}

		auto						lines_opt	= read_source_lines(src);
		std::vector<std::wstring>	lines		=
			lines_opt ? *lines_opt : std::vector<std::wstring>{L""};
		int							n			= (int)lines.size();
		int							issues		= 0;

		for (const auto& a : loaded.anns)
		{
			if (a.end_line > n)
			{
				std::printf("WARN	%s: %s: range %d-%d beyond %d lines\n",
							label.c_str(),
							a.id.c_str(),
							a.start_line,
							a.end_line, n);

				++issues;
			}
			else if (!a.anchor.empty() &&
					 !a.orphaned &&
					anchor_text(lines[(size_t)a.start_line - 1]) != a.anchor)
			{
				std::printf("WARN	%s: %s: anchor drifted from line %d\n",
							label.c_str(),
							a.id.c_str(),
							a.start_line);

				++issues;
			}
		}

		problems += issues;

		if (issues && fix)
		{
			auto		[repaired, lost]	= reconcile_annotations(loaded.anns, lines);
			std::string	rel_fwd				= fs::path(rel_str).generic_string();

			save_annotations(ann_path, rel_fwd, loaded.anns, &ws.ann_root);

			std::printf("FIXED	%s: %d re-anchored, %d orphaned\n",
						label.c_str(),
						repaired, lost);
		}
	}

	std::printf("checked %d annotation file(s): %d problem(s)%s\n",
				files,
				problems,
				(fix && problems) ? " (fix applied)" : "");

	if (problems == 0) return 0;

	return (!fix || unfixable) ? 1 : 0;
}

// ---------------------------------------------------------------- edit mode

bool is_valid_utf8(const std::string& s)
{
	size_t i = 0;

	while (i < s.size())
	{
		unsigned char	c	= (unsigned char)s[i];
		int				len;
		uint32_t		cp;

		if (c < 0x80)					{ ++i; continue; }
		else if ((c & 0xE0) == 0xC0)	{ len = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0)	{ len = 3; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0)	{ len = 4; cp = c & 0x07; }
		else							return false;

		if (i + (size_t)len > s.size()) return false;

		for (int k = 1; k < len; ++k)
		{
			unsigned char cc = (unsigned char)s[i + (size_t)k];

			if ((cc & 0xC0) != 0x80) return false;

			cp = (cp << 6) | (cc & 0x3F);
		}

		// reject overlong forms, surrogates, and out-of-range code points,
		// exactly like Python's strict utf-8 codec
		static const uint32_t min_cp[] = {0, 0, 0x80, 0x800, 0x10000};

		if (cp < min_cp[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return false;

		i += (size_t)len;
	}

	return true;
}

EditSource load_source_for_edit(const fs::path& p)
{
	EditSource	src;
	auto		bytes	= read_file_bytes(p);

	if (!bytes)
	{
		src.status = EditSource::Unreadable;

		return src;
	}

	if (!is_valid_utf8(*bytes))
	{
		src.status = EditSource::NotUtf8;

		return src;
	}

	std::string_view body = *bytes;

	if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
	{
		src.bom = true;

		body.remove_prefix(3);
	}

	src.crlf = body.find("\r\n") != std::string_view::npos;

	src.lines = split_source_lines(utf8_to_wide(std::string(body)));

	return src;
}

std::vector<std::wstring> split_source_lines(const std::wstring& text)
{
	std::vector<std::wstring>	lines;
	std::wstring				cur;

	for (size_t i = 0; i < text.size(); ++i)
	{
		wchar_t c = text[i];

		if (c == L'\r' || c == L'\n')
		{
			if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
				++i;

			lines.push_back(std::move(cur));
			cur.clear();
		}
		else
		{
			cur += c;
		}
	}

	lines.push_back(std::move(cur));

	return lines;
}

std::string join_source(const std::vector<std::wstring>& lines, bool crlf, bool bom)
{
	std::string out = bom ? "\xEF\xBB\xBF" : "";

	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (i) out += crlf ? "\r\n" : "\n";

		out += wide_to_utf8(lines[i]);
	}

	return out;
}

bool rebase_annotation(
	Annotation&							a,
	int									start_line0,
	int									end_line0,
	int									end_col,
	bool								empty,
	const std::vector<std::wstring>&	lines)
{
	// the empty "line" after a trailing newline isn't shown by the viewer
	int total = (int)lines.size();

	if (total > 1 && lines.back().empty()) --total;

	total = std::max(1, total);

	if (empty)
	{
		int ln = std::max(1, std::min(start_line0 + 1, total));

		if (a.start_line == ln && a.end_line == ln && a.orphaned)
			return false;

		a.start_line	= ln;
		a.end_line		= ln;
		a.orphaned		= true;

		return true;
	}

	int start	= std::max(1, std::min(start_line0 + 1, total));
	int end		= end_line0 + 1;

	if (end_col == 0) --end;			// region ends at a line start: exclusive

	end = std::max(start, std::min(end, total));

	std::string anchor =
		(size_t)start <= lines.size() ? anchor_text(lines[(size_t)start - 1]) : "";

	if (a.start_line == start && a.end_line == end && a.anchor == anchor)
		return false;

	a.start_line	= start;
	a.end_line		= end;
	a.anchor		= anchor;

	return true;
}

}  // namespace core
