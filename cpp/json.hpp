// Copyright (c) 2026 Andy Hendrickson
// SPDX-License-Identifier: MIT
//
// json.hpp — minimal JSON parser/serializer for the Code Annotator C++ port.
//
// The serializer reproduces Python's json.dump(data, indent=2) output
// (ensure_ascii, ": " separators, "\n" line breaks) so annotation files
// written by this port are byte-identical to the Python app's.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value
{
	enum Type { Null, Bool, Num, Str, Arr, Obj };

	Type				type	= Null;
	bool				boolean	= false;
	double				number	= 0.0;
	bool				is_int	= false;
	std::string			str;							// UTF-8
	std::vector<Value>	arr;
	std::vector<std::pair<std::string, Value>>	obj;	// insertion-ordered

	static Value make_int(long long v)
	{
		Value x;	x.type = Num;	x.number = double(v);	x.is_int = true;	return x;
	}
	static Value make_num(double v)
	{
		Value x;	x.type = Num;	x.number = v;								return x;
	}
	static Value make_str(std::string s)
	{
		Value x;	x.type = Str;	x.str = std::move(s);						return x;
	}
	static Value make_bool(bool b)
	{
		Value x;	x.type = Bool;	x.boolean = b;								return x;
	}
	static Value make_arr() { Value x;	x.type = Arr;	return x; }
	static Value make_obj() { Value x;	x.type = Obj;	return x; }

	const Value* get(const char* key) const
	{
		if (type != Obj) return nullptr;

		for (const auto& kv : obj)
		{
			if (kv.first == key) return &kv.second;
		}

		return nullptr;
	}
	void set(std::string key, Value v)
	{
		obj.emplace_back(std::move(key), std::move(v));
	}
};

// ------------------------------------------------------------------ parsing

class Parser
{
public:
	explicit Parser(const std::string& s) : s_(s) {}

	bool parse(Value& out)
	{
		skip();

		if (!value(out)) return false;

		skip();

		return pos_ == s_.size();
	}

private:
	const std::string&	s_;
	size_t				pos_	= 0;

	void skip()
	{
		while (pos_ < s_.size())
		{
			char c = s_[pos_];

			if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				++pos_;

			else break;
		}
	}
	bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
	bool lit(const char* t)
	{
		size_t n = std::strlen(t);

		if (s_.compare(pos_, n, t) == 0)
		{
			pos_ += n;

			return true;
		}

		return false;
	}

	bool value(Value& v)
	{
		skip();

		if (pos_ >= s_.size()) return false;

		char c = s_[pos_];

		if (c == '{')		{											return object(v);		}
		if (c == '[')		{											return array(v);		}
		if (c == '"')		{ v.type = Value::Str;						return string(v.str);	}
		if (lit("true"))	{ v.type = Value::Bool; v.boolean = true;	return true;			}
		if (lit("false"))	{ v.type = Value::Bool; v.boolean = false;	return true;			}
		if (lit("null"))	{ v.type = Value::Null;						return true;			}

		return number(v);
	}

	bool object(Value& v)
	{
		v.type = Value::Obj;

		++pos_;										   // '{'

		skip();

		if (peek('}'))
		{
			++pos_;

			return true;
		}

		while (true)
		{
			skip();

			std::string key;

			if (!peek('"') || !string(key)) return false;

			skip();

			if (!peek(':')) return false;

			++pos_;

			Value val;

			if (!value(val)) return false;

			v.obj.emplace_back(std::move(key), std::move(val));

			skip();

			if (peek(',')) { ++pos_; continue; }
			if (peek('}')) { ++pos_; return true; }

			return false;
		}
	}

	bool array(Value& v)
	{
		v.type = Value::Arr;

		++pos_;										   // '['

		skip();

		if (peek(']')) { ++pos_; return true; }

		while (true)
		{
			Value item;

			if (!value(item)) return false;

			v.arr.push_back(std::move(item));

			skip();

			if (peek(',')) { ++pos_; continue; }
			if (peek(']')) { ++pos_; return true; }

			return false;
		}
	}

	static void append_utf8(std::string& out, uint32_t cp)
	{
		if (cp < 0x80)
			out += char(cp);
		else if (cp < 0x800)
		{
			out += char(0xC0 | (cp >>  6));
			out += char(0x80 | (cp			& 0x3F));
		} else if (cp < 0x10000)
		{
			out += char(0xE0 |  (cp >> 12));
			out += char(0x80 | ((cp >>  6)	& 0x3F));
			out += char(0x80 | (cp			& 0x3F));
		} else
		{
			out += char(0xF0 |  (cp >> 18));
			out += char(0x80 | ((cp >> 12)	& 0x3F));
			out += char(0x80 | ((cp >>  6)	& 0x3F));
			out += char(0x80 | (cp			& 0x3F));
		}
	}

	bool hex4(uint32_t& out)
	{
		if (pos_ + 4 > s_.size()) return false;

		out = 0;

		for (int k = 0; k < 4; ++k)
		{
			char c = s_[pos_ + k];

			out <<= 4;

			if (c >= '0' && c <= '9')		out |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f')	out |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')	out |= uint32_t(c - 'A' + 10);
			else return false;
		}

		pos_ += 4;

		return true;
	}

	bool string(std::string& out)
	{
		if (!peek('"')) return false;

		++pos_;

		out.clear();

		while (pos_ < s_.size())
		{
			char c = s_[pos_];

			if (c == '"')
			{
				++pos_;

				return true;
			}

			if (c == '\\')		// Escaped character expected.
			{
				++pos_;

				if (pos_ >= s_.size()) return false;

				char e = s_[pos_++];

				switch (e)
				{
					case '"':	out += '"';		break;
					case '\\':	out += '\\';	break;
					case '/':	out += '/';		break;
					case 'b':	out += '\b';	break;
					case 'f':	out += '\f';	break;
					case 'n':	out += '\n';	break;
					case 'r':	out += '\r';	break;
					case 't':	out += '\t';	break;

					case 'u':
					{
						uint32_t cp;

						if (!hex4(cp)) return false;

						if (cp >= 0xD800 &&
							cp <= 0xDBFF &&
							pos_ + 1 < s_.size() &&
							s_[pos_] == '\\' &&
							s_[pos_ + 1] == 'u')
						{
							size_t save = pos_;

							pos_ += 2;

							uint32_t lo;

							if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							else
								pos_ = save;			   // lone high surrogate
						}

						append_utf8(out, cp);

						break;
					}

					default: return false;
				}
			}
			else
			{
				out += c;

				++pos_;
			}
		}

		return false;								   // unterminated
	}

	bool number(Value& v)
	{
		size_t start = pos_;

		if (peek('-')) ++pos_;

		while (	pos_ < s_.size() &&
				(std::isdigit((unsigned char)s_[pos_]) ||
				s_[pos_] == '.' ||
				s_[pos_] == 'e' ||
				s_[pos_] == 'E' ||
				s_[pos_] == '+' ||
				s_[pos_] == '-'))
		{
			++pos_;
		}

		if (pos_ == start) return false;

		std::string tok = s_.substr(start, pos_ - start);

		char*	end	= nullptr;
		double	d	= std::strtod(tok.c_str(), &end);

		if (!end || *end != '\0') return false;

		v.type		= Value::Num;
		v.number	= d;
		v.is_int	=	tok.find('.') == std::string::npos &&
						tok.find('e') == std::string::npos &&
						tok.find('E') == std::string::npos;

		return true;
	}
};

inline bool parse(const std::string& text, Value& out)
{
	return Parser(text).parse(out);
}

// ------------------------------------------------------------ serialization

inline void escape_string(std::string& out, const std::string& s)
{
	out += '"';

	size_t i = 0;

	while (i < s.size())
	{
		unsigned char c = (unsigned char)s[i];

		if 		(c == '"')	{ out += "\\\"";	++i; }
		else if (c == '\\')	{ out += "\\\\";	++i; }
		else if (c == '\n')	{ out += "\\n";		++i; }
		else if (c == '\t')	{ out += "\\t";		++i; }
		else if (c == '\r')	{ out += "\\r";		++i; }
		else if (c == '\b')	{ out += "\\b";		++i; }
		else if (c == '\f')	{ out += "\\f";		++i; }
		else if (c < 0x20)
		{
			char buf[8];

			std::snprintf(buf, sizeof buf, "\\u%04x", c);

			out += buf;

			++i;
		}
		else if (c < 0x80)
		{
			out += char(c);

			++i;
		}
		else
		{
		   // non-ASCII: \uXXXX like ensure_ascii
			uint32_t	cp	= 0xFFFD;
			int			len	= 1;

			if 		((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
			else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
			else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }

			if (len > 1)
			{
				for (int k = 1; k < len; ++k)
				{
					if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80)
					{
						cp	= 0xFFFD;
						len	= 1;

						break;
					}

					cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
				}
			}

			i += len;

			char buf[16];

			if (cp >= 0x10000)
			{
				uint32_t v = cp - 0x10000;

				std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
			}
			else
			{
				std::snprintf(buf, sizeof buf, "\\u%04x", cp);
			}

			out += buf;
		}
	}

	out += '"';
}

inline void format_number(std::string& out, const Value& v)
{
	char buf[40];

	if (v.is_int)
	{
		std::snprintf(buf, sizeof buf, "%lld", (long long)v.number);
	}
	else if (std::isfinite(v.number) && v.number == std::floor(v.number) && std::fabs(v.number) < 1e15)
	{
		std::snprintf(buf, sizeof buf, "%.1f", v.number);	// 55.0, not 55
	} else
	{
		std::snprintf(buf, sizeof buf, "%.17g", v.number);

		// trim to the shortest round-trip form Python would print
		double parsed;

		for (int prec = 1; prec <= 17; ++prec)
		{
			char t[40];

			std::snprintf(t, sizeof t, "%.*g", prec, v.number);

			if (std::sscanf(t, "%lf", &parsed) == 1 && parsed == v.number)
			{
				std::memcpy(buf, t, sizeof t);

				break;
			}
		}
	}

	out += buf;
}

inline void dump_value(const Value& v, std::string& out, int level, int indent)
{
	const std::string pad((size_t)(level + 1) * indent, ' ');
	const std::string pad_close((size_t)level * indent, ' ');

	switch (v.type)
	{
		case Value::Null:	out += "null"; break;
		case Value::Bool:	out += v.boolean ? "true" : "false"; break;
		case Value::Num:	format_number(out, v); break;
		case Value::Str:	escape_string(out, v.str); break;

		case Value::Arr:
			if (v.arr.empty())
			{
				out += "[]";

				break;
			}

			out += "[\n";

			for (size_t i = 0; i < v.arr.size(); ++i)
			{
				out += pad;

				dump_value(v.arr[i], out, level + 1, indent);

				if (i + 1 < v.arr.size())
					out += ',';

				out += '\n';
			}

			out += pad_close;
			out += ']';

			break;

		case Value::Obj:
			if (v.obj.empty())
			{
				out += "{}";

				break;
			}

			out += "{\n";

			for (size_t i = 0; i < v.obj.size(); ++i)
			{
				out += pad;

				escape_string(out, v.obj[i].first);

				out += ": ";

				dump_value(v.obj[i].second, out, level + 1, indent);

				if (i + 1 < v.obj.size())
					out += ',';

				out += '\n';
			}

			out += pad_close;
			out += '}';

			break;
	}
}

inline std::string dump(const Value& v, int indent = 2)
{
	std::string out;

	dump_value(v, out, 0, indent);

	return out;
}

}  // namespace json
