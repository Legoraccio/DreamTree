#pragma once
#include "base.h"

// Portable text, path and time helpers (no system calls except through platform.h).

// UTF-8 <-> wide text (UTF-16 or UTF-32, after the size of wchar_t). Invalid input becomes U+FFFD.
std::wstring Widen(std::string_view s);
void WidenInto(std::string_view s, std::wstring& out);    // replaces out
void WidenAppend(std::string_view s, std::wstring& out);  // appends to out
std::string Narrow(std::wstring_view s);
inline bool IsHighSurrogate(wchar_t c) { return sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF; }
inline bool IsLowSurrogate(wchar_t c) { return sizeof(wchar_t) == 2 && c >= 0xDC00 && c <= 0xDFFF; }

double NowMs();     // monotonic clock, milliseconds
int64_t UnixNow();  // seconds since 1970
bool TestMode();    // DREAMTREE_DEBUG is set: diagnostics for UI tests

std::wstring FormatRelative(int64_t unixSec);  // "5 minutes ago", "yesterday", "14 Oct", "3 Mar 2024"
std::wstring FormatDateLong(int64_t unixSec);  // "18 Oct 2026, 10:42"
std::wstring FormatCount(size_t n);            // thousands separators
std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many);  // "1 file", "3 files"

uint32_t Fnv1a(std::string_view s);
uint32_t Fnv1aW(std::wstring_view s);

// Command lines: QuoteArg quotes one argument (Windows rules); SplitCommandLine is its inverse, used on
// systems that start programs with an argument vector.
std::wstring QuoteArg(const std::wstring& a);
std::vector<std::wstring> SplitCommandLine(std::wstring_view s);

std::wstring BaseName(const std::wstring& path);
std::wstring NormalizePath(const std::wstring& path);                // native separators, no trailing one
std::wstring PathJoin(const std::wstring& dir, std::wstring_view rel);  // rel may use '/'

std::string_view TrimSv(std::string_view s);
std::wstring TrimW(const std::wstring& s);
