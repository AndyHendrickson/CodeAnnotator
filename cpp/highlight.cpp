// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
// highlight.cpp — see highlight.hpp. Tables are copied verbatim from the
// Python original; each entry is a whitespace-separated word list.
#include "highlight.hpp"

#include <algorithm>
#include <cwctype>
#include <map>
#include <sstream>

namespace hl {

namespace {

struct WordList { const char* lang; const char* words; };

const WordList KEYWORDS[] =
{
	{"py", R"(and as assert async await break class continue def del elif else
		except finally for from global if import in is lambda nonlocal not or
		pass raise return try while with yield match case)"},
	{"js", R"(abstract as async await break case catch class const continue
		debugger default delete do else enum export extends finally for from
		function get if implements import in instanceof interface let new of
		private protected public return set static super switch this throw
		try type typeof var void while with yield)"},
	{"cs", R"(abstract as base break case catch checked class const continue
		default delegate do else enum event explicit extern finally fixed for
		foreach goto if implicit in interface internal is lock namespace new
		operator out override params private protected public readonly ref
		return sealed static struct switch this throw try typeof unsafe using
		var virtual volatile while get set async await record init required
		partial when where yield)"},
	{"c", R"(auto break case const continue default do else enum extern for
		goto if inline register return signed sizeof static struct switch
		typedef union unsigned volatile while class delete friend namespace
		new operator private protected public template this throw try catch
		typename using virtual override final constexpr nullptr)"},
	{"java", R"(abstract assert break case catch class const continue default
		do else enum extends final finally for goto if implements import
		instanceof interface native new package private protected public
		return static strictfp super switch synchronized this throw throws
		transient try volatile while var record sealed permits yield)"},
	{"go", R"(break case chan const continue default defer else fallthrough
		for func go goto if import interface map package range return select
		struct switch type var)"},
	{"rust", R"(as async await break const continue crate dyn else enum extern
		fn for if impl in let loop match mod move mut pub ref return self
		static struct super trait type unsafe use where while)"},
	{"sql", R"(select from where insert update delete into values set join
		left right inner outer on group by order having limit offset union
		all distinct as and or not null is in like between exists case when
		then else end create table drop alter index view primary key foreign
		references default unique check)"},
	{"sh", R"(if then else elif fi for while until do done case esac function
		in select return exit break continue local declare export readonly
		unset shift eval exec source alias)"},
};

const WordList LITERALS[] =
{
	{"py", "True False None self cls"},
	{"js", "true false null undefined NaN Infinity"},
	{"cs", "true false null this value"},
	{"c", "true false NULL nullptr this"},
	{"java", "true false null this"},
	{"go", "true false nil iota"},
	{"rust", "true false Self self None Some Ok Err"},
	{"sql", "true false null"},
	{"sh", "true false"},
};

const WordList TYPES[] =
{
	{"py", R"(int float str bool list dict set tuple bytes object Exception
		print len range type super isinstance)"},
	{"js", R"(boolean number string object symbol bigint any unknown never
		Array Object String Number Boolean Promise Map Set Date RegExp Error
		console window document Math JSON)"},
	{"cs", R"(bool byte char decimal double float int long object sbyte short
		string uint ulong ushort void Task List Dictionary IEnumerable String
		Console Exception)"},
	{"c", R"(bool char double float int long short signed unsigned void size_t
		int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t std
		string vector map)"},
	{"java", R"(boolean byte char double float int long short void String
		Integer Boolean Object List Map ArrayList HashMap System Exception)"},
	{"go", R"(bool byte complex64 complex128 error float32 float64 int int8
		int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr)"},
	{"rust", R"(bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64
		u128 usize String Vec Option Result Box HashMap)"},
	{"sql", R"(int integer bigint smallint varchar char text date datetime
		timestamp decimal numeric float real boolean blob)"},
	{"sh", "echo printf read cd ls rm mv cp mkdir grep sed awk cat test"},
};

void add_words(std::unordered_map<std::wstring, Tok>& map,
			   const char* lang, Tok cls, const WordList* table, size_t count)
{
	for (size_t i = 0; i < count; ++i) 
	{
		if (std::string(table[i].lang) != lang) continue;

		std::istringstream	ss(table[i].words);
		std::string			w;

		while (ss >> w)
			map[std::wstring(w.begin(), w.end())] = cls;
	}
}

LangCfg make_lang(const char* id)
{
	LangCfg cfg;

	add_words(cfg.words,	id,	Tok::Kw,	KEYWORDS,	std::size(KEYWORDS));
	add_words(cfg.words,	id,	Tok::Lit,	LITERALS,	std::size(LITERALS));
	add_words(cfg.words,	id,	Tok::Typ,	TYPES,		std::size(TYPES));

	return cfg;
}

std::map<std::string, LangCfg> build_langs()
{
	std::map<std::string, LangCfg>	langs;
	LangCfg							c;

	c = make_lang("py");	c.line = L"#";	c.triple = true;													langs["py"]		= c;
	c = make_lang("js");	c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";	c.tmpl = true;		langs["js"]		= c;
	c = make_lang("cs");	c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";						langs["cs"]		= c;
	c = make_lang("c");		c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";						langs["c"]		= c;
	c = make_lang("java");	c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";						langs["java"]	= c;
	c = make_lang("go");	c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";	c.tmpl = true;		langs["go"]		= c;
	c = make_lang("rust");	c.line = L"//";	c.block_start = L"/*";	c.block_end = L"*/";						langs["rust"]	= c;
	c = make_lang("sql");	c.line = L"--";	c.block_start = L"/*";	c.block_end = L"*/";	c.casefold = true;	langs["sql"]	= c;
	c = make_lang("sh");	c.line = L"#";																		langs["sh"]		= c;

	c = LangCfg{};							c.block_start = L"/*";	c.block_end = L"*/";						langs["css"]	= c;
	c = LangCfg{};			c.words = {{L"true", Tok::Lit}, {L"false", Tok::Lit}, {L"null", Tok::Lit}};			langs["json"]	= c;
	c = LangCfg{};		   c.html = true;																		langs["html"]	= c;

	return langs;
}

const std::map<std::string, LangCfg>& langs()
{
	static const std::map<std::string, LangCfg> table = build_langs();

	return table;
}

const std::map<std::string, std::string>& ext_lang()
{
	static const std::map<std::string, std::string> table =
	{
		{"py", "py"}, {"pyw", "py"}, {"rb", "py"}, {"lua", "py"}, {"r", "py"},
		{"js", "js"}, {"jsx", "js"}, {"ts", "js"}, {"tsx", "js"},
		{"mjs", "js"}, {"cjs", "js"}, {"vue", "js"}, {"php", "js"},
		{"swift", "js"}, {"dart", "js"},
		{"cs", "cs"},
		{"c", "c"}, {"h", "c"}, {"cpp", "c"}, {"hpp", "c"}, {"cc", "c"},
		{"cxx", "c"}, {"ino", "c"},
		{"java", "java"}, {"kt", "java"}, {"kts", "java"}, {"scala", "java"},
		{"go", "go"},
		{"rs", "rust"},
		{"sql", "sql"},
		{"sh", "sh"}, {"bash", "sh"}, {"zsh", "sh"}, {"ps1", "sh"},
		{"psm1", "sh"}, {"bat", "sh"}, {"cmd", "sh"}, {"yml", "sh"},
		{"yaml", "sh"}, {"toml", "sh"}, {"ini", "sh"}, {"env", "sh"},
		{"gitignore", "sh"},
		{"css", "css"}, {"scss", "css"}, {"less", "css"},
		{"json", "json"}, {"jsonc", "json"},
		{"html", "html"}, {"htm", "html"}, {"xml", "html"}, {"svg", "html"},
		{"xaml", "html"}, {"csproj", "html"}, {"config", "html"},
	};
	return table;
}

bool ident_char(wchar_t c)
{
	return iswalnum(c) || c == L'_' || c == L'$';
}

bool starts_with(const std::wstring& s, const std::wstring& prefix, size_t at)
{
	return !prefix.empty() && s.compare(at, prefix.size(), prefix) == 0;
}

}  // namespace

std::string lang_for(const std::wstring& filename)
{
	std::wstring lower = filename;

	std::transform(lower.begin(), lower.end(), lower.begin(), towlower);

	std::string narrow;

	for (wchar_t c : lower)
		narrow += (c < 128) ? char(c) : '?';

	if (narrow == "dockerfile" || narrow == "makefile")
		return "sh";

	size_t		dot	= narrow.rfind('.');
	std::string	ext	= dot == std::string::npos ? "" : narrow.substr(dot + 1);
	auto		it	= ext_lang().find(ext);

	return it == ext_lang().end() ? "plain" : it->second;
}

const LangCfg* lang_cfg(const std::string& lang_id)
{
	auto it = langs().find(lang_id);

	return it == langs().end() ? nullptr : &it->second;
}

std::vector<Token> scan_line(const std::wstring&	line,
							 const LangCfg&			cfg,
							 ScanState&				st)
{
	std::vector<Token>	out;
	size_t				i	= 0;
	size_t				n	= line.size();

	while (i < n)
	{
		if (st.mode == ScanState::Block)
		{
			size_t end = line.find(cfg.block_end, i);

			if (end == std::wstring::npos)
			{
				out.push_back({(int)i, line.substr(i), Tok::Com});

				return out;
			}

			end += cfg.block_end.size();

			out.push_back({(int)i, line.substr(i, end - i), Tok::Com});

			i		= end;
			st.mode	= ScanState::NoneM;

			continue;
		}

		if (st.mode == ScanState::Str)
		{
			size_t end = line.find(st.delim, i);

			if (end == std::wstring::npos)
			{
				out.push_back({(int)i, line.substr(i), Tok::Str});

				return out;
			}

			end += st.delim.size();

			out.push_back({(int)i, line.substr(i, end - i), Tok::Str});

			i		= end;
			st.mode	= ScanState::NoneM;

			continue;
		}

		wchar_t ch = line[i];

		if (!cfg.block_start.empty() && starts_with(line, cfg.block_start, i))
		{
			size_t end = line.find(cfg.block_end, i + cfg.block_start.size());

			if (end == std::wstring::npos)
			{
				out.push_back({(int)i, line.substr(i), Tok::Com});

				st.mode = ScanState::Block;

				return out;
			}

			end += cfg.block_end.size();

			out.push_back({(int)i, line.substr(i, end - i), Tok::Com});

			i = end;

			continue;
		}

		if (!cfg.line.empty() && starts_with(line, cfg.line, i))
		{
			out.push_back({(int)i, line.substr(i), Tok::Com});

			return out;
		}

		if (cfg.triple && i + 3 <= n)
		{
			std::wstring three = line.substr(i, 3);

			if (three == L"'''" || three == L"\"\"\"")
			{
				size_t end = line.find(three, i + 3);

				if (end == std::wstring::npos)
				{
					out.push_back({(int)i, line.substr(i), Tok::Str});

					st.mode		= ScanState::Str;
					st.delim	= three;

					return out;
				}

				out.push_back({(int)i, line.substr(i, end + 3 - i), Tok::Str});

				i = end + 3;

				continue;
			}
		}

		if (cfg.tmpl && ch == L'`')
		{
			size_t j = i + 1;

			while (j < n && line[j] != L'`')
				j += (line[j] == L'\\') ? 2 : 1;

			if (j >= n)
			{
				out.push_back({(int)i, line.substr(i), Tok::Str});

				st.mode		= ScanState::Str;
				st.delim	= L"`";

				return out;
			}

			out.push_back({(int)i, line.substr(i, j + 1 - i), Tok::Str});

			i = j + 1;

			continue;
		}

		if (cfg.strings.find(ch) != std::wstring::npos)
		{
			size_t j = i + 1;

			while (j < n && line[j] != ch)
				j += (line[j] == L'\\') ? 2 : 1;

			j = std::min(j + 1, n);

			out.push_back({(int)i, line.substr(i, j - i), Tok::Str});

			i = j;

			continue;
		}

		if (iswdigit(ch) || (ch == L'.' && i + 1 < n && iswdigit(line[i + 1])))
		{
			size_t j = i;

			while (j < n && (iswalnum(line[j]) || line[j] == L'.' || line[j] == L'_'))
				++j;

			out.push_back({(int)i, line.substr(i, j - i), Tok::Num});

			i = j;

			continue;
		}

		if (iswalpha(ch) || ch == L'_' || ch == L'$')
		{
			size_t j = i;

			while (j < n && ident_char(line[j])) ++j;

			std::wstring	word	= line.substr(i, j - i);
			std::wstring	key		= word;

			if (cfg.casefold)
				std::transform(key.begin(), key.end(), key.begin(), towlower);

			Tok		cls	= Tok::None;
			auto	it	= cfg.words.find(key);

			if (it != cfg.words.end())
				cls = it->second;
			else if (j < n && line[j] == L'(')
				cls = Tok::Fn;

			out.push_back({(int)i, std::move(word), cls});

			i = j;

			continue;
		}

		out.push_back({(int)i, std::wstring(1, ch), Tok::None});

		++i;
	}
	return out;
}

std::vector<Token> scan_html_line(const std::wstring& line, ScanState& st)
{
	std::vector<Token>	out;
	size_t				i	= 0;
	size_t				n	= line.size();

	while (i < n)
	{
		if (st.mode == ScanState::HtmlComment)
		{
			size_t end = line.find(L"-->", i);

			if (end == std::wstring::npos)
			{
				out.push_back({(int)i, line.substr(i), Tok::Com});

				return out;
			}

			out.push_back({(int)i, line.substr(i, end + 3 - i), Tok::Com});

			i = end + 3;

			st.mode = ScanState::NoneM;

			continue;
		}

		if (st.mode == ScanState::HtmlTag)
		{
			wchar_t ch = line[i];

			if (ch == L'>')
			{
				out.push_back({(int)i, L">", Tok::None});

				++i;

				st.mode = ScanState::NoneM;

				continue;
			}

			if (ch == L'\'' || ch == L'"')
			{
				size_t j = i + 1;

				while (j < n && line[j] != ch) ++j;

				j = std::min(j + 1, n);

				out.push_back({(int)i, line.substr(i, j - i), Tok::Str});

				i = j;

				continue;
			}

			if (iswalpha(ch) || ch == L'_' || ch == L':' || ch == L'-')
			{
				size_t j = i;

				while (j < n && (iswalnum(line[j]) || line[j] == L'_'
								 || line[j] == L':' || line[j] == L'.'
								 || line[j] == L'-'))
					++j;

				out.push_back({(int)i, line.substr(i, j - i), Tok::Attr});

				i = j;

				continue;
			}

			out.push_back({(int)i, std::wstring(1, ch), Tok::None});

			++i;

			continue;
		}

		if (starts_with(line, L"<!--", i))
		{
			size_t end = line.find(L"-->", i);

			if (end == std::wstring::npos)
			{
				out.push_back({(int)i, line.substr(i), Tok::Com});

				st.mode = ScanState::HtmlComment;

				return out;
			}

			out.push_back({(int)i, line.substr(i, end + 3 - i), Tok::Com});

			i = end + 3;

			continue;
		}
		if (line[i] == L'<')
		{
			size_t j = i + 1;

			if (j < n && (line[j] == L'/' || line[j] == L'!' || line[j] == L'?'))
				++j;

			size_t k = j;

			while (	k < n &&
					(
						iswalnum(line[k]) ||
						line[k] == L'_' ||
						line[k] == L':' ||
						line[k] == L'.' ||
						line[k] == L'-'
					)
			)
			{
				++k;
			}

			out.push_back({(int)i, line.substr(i, j - i), Tok::None});

			if (k > j)
				out.push_back({(int)j, line.substr(j, k - j), Tok::Tag});

			i = k;

			st.mode = ScanState::HtmlTag;

			continue;
		}

		size_t j = line.find(L'<', i);

		if (j == std::wstring::npos) j = n;

		out.push_back({(int)i, line.substr(i, j - i), Tok::None});

		i = j;
	}

	return out;
}

std::vector<std::vector<Token>> highlight_lines(
	const std::vector<std::wstring>& lines, const std::string& lang_id)
{
	std::vector<std::vector<Token>> out;

	out.reserve(lines.size());

	const LangCfg*	cfg		= lang_cfg(lang_id);
	ScanState		st;

	for (const auto& ln : lines)
	{
		if (!cfg)
		{
			std::vector<Token> t;

			if (!ln.empty()) t.push_back({0, ln, Tok::None});

			out.push_back(std::move(t));
		} else if (cfg->html)
		{
			out.push_back(scan_html_line(ln, st));
		} else
		{
			out.push_back(scan_line(ln, *cfg, st));
		}
	}

	return out;
}

}  // namespace hl
