#include "text.h"
#include "platform.h"
#include <chrono>
#include <cwchar>
#include <cwctype>

// ============================================================================ UTF-8 <-> wide

namespace {

using WUnit = std::make_unsigned_t<wchar_t>;

// One UTF-8 sequence at p[i], advancing i. An invalid byte decodes to U+FFFD and is skipped on its own.
char32_t DecodeUtf8(const unsigned char* p, size_t n, size_t& i) {
    const unsigned char c = p[i];
    int len = 0;
    char32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
    else { ++i; return 0xFFFD; }
    if (i + (size_t)len > n) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        const unsigned char cc = p[i + (size_t)k];
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;  // overlong form, surrogate or out of range
        return 0xFFFD;
    }
    i += (size_t)len;
    return cp;
}

void PutWide(std::wstring& out, char32_t cp) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
            return;
        }
    }
    out.push_back((wchar_t)cp);
}

void PutUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

void WidenAppend(std::string_view s, std::wstring& out) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        // ASCII runs (most commit text) are copied without decoding
        size_t j = i;
        while (j < n && p[j] < 0x80) ++j;
        if (j > i) {
            const size_t base = out.size();
            out.resize(base + (j - i));
            for (size_t k = i; k < j; ++k) out[base + (k - i)] = (wchar_t)p[k];
            i = j;
            if (i >= n) break;
        }
        PutWide(out, DecodeUtf8(p, n, i));
    }
}

void WidenInto(std::string_view s, std::wstring& out) {
    out.clear();
    WidenAppend(s, out);
}

std::wstring Widen(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    WidenAppend(s, out);
    return out;
}

std::string Narrow(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = (char32_t)(WUnit)s[i];
        if (cp < 0x80) {
            out.push_back((char)cp);
            continue;
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
                const char32_t lo = (char32_t)(WUnit)s[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;  // unpaired surrogate or invalid
        PutUtf8(out, cp);
    }
    return out;
}

// ============================================================================ time

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int64_t UnixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool TestMode() {
    static const bool on = !EnvVar("DREAMTREE_DEBUG").empty();
    return on;
}

namespace {

// Days since 1970-01-01 of a calendar date (proleptic Gregorian).
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// English month names: the interface is in English; the time of day follows the user's locale.
const wchar_t* const kMonths[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };

std::wstring DayMonth(const LocalTime& t, bool withYear) {
    std::wstring s = std::to_wstring(t.day) + L" " + kMonths[(t.month + 11) % 12];
    if (withYear) s += L" " + std::to_wstring(t.year);
    return s;
}

}  // namespace

std::wstring FormatRelative(int64_t t) {
    const int64_t now = UnixNow();
    LocalTime lt, ln;
    if (!ToLocalTime(t, lt) || !ToLocalTime(now, ln)) return L"";
    const int64_t d = now - t;
    if (d < -120) return DayMonth(lt, lt.year != ln.year);  // in the future: a skewed clock
    if (d < 60) return L"just now";
    if (d < 3600) return d < 120 ? L"1 minute ago" : std::to_wstring(d / 60) + L" minutes ago";
    const int64_t days = DaysFromCivil(ln.year, ln.month, ln.day) - DaysFromCivil(lt.year, lt.month, lt.day);
    if (days == 0 || d < 6 * 3600) return d < 7200 ? L"1 hour ago" : std::to_wstring(d / 3600) + L" hours ago";
    if (days == 1) return L"yesterday";
    if (days < 7) return std::to_wstring(days) + L" days ago";
    return DayMonth(lt, lt.year != ln.year);
}

std::wstring FormatDateLong(int64_t t) {
    LocalTime lt;
    if (!ToLocalTime(t, lt)) return L"";
    return DayMonth(lt, true) + L", " + FormatClock(t);
}

std::wstring FormatCount(size_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, 1, L'.');
    return s;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) { return FormatCount(n) + L" " + (n == 1 ? one : many); }

// ============================================================================ hashing

uint32_t Fnv1a(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

uint32_t Fnv1aW(std::wstring_view s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) { h ^= (uint32_t)(WUnit)c; h *= 16777619u; }
    return h;
}

// ============================================================================ command lines

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++backslashes; }
        if (i == a.size()) { q.append(backslashes * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(backslashes * 2 + 1, L'\\'); q.push_back(L'"'); }
        else { q.append(backslashes, L'\\'); q.push_back(a[i]); }
    }
    q.push_back(L'"');
    return q;
}

// The parsing rules of CommandLineToArgvW: backslashes are literal unless they precede a quote; 2n
// backslashes + quote = n backslashes and a quote that opens or closes; 2n+1 = n backslashes and a literal
// quote; inside quotes, "" is a literal quote.
std::vector<std::wstring> SplitCommandLine(std::wstring_view s) {
    std::vector<std::wstring> args;
    size_t i = 0;
    for (;;) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n')) ++i;
        if (i >= s.size()) break;
        std::wstring a;
        bool quoted = false;
        while (i < s.size()) {
            const wchar_t c = s[i];
            if (!quoted && (c == L' ' || c == L'\t' || c == L'\n')) break;
            if (c == L'\\') {
                size_t n = 0;
                while (i < s.size() && s[i] == L'\\') { ++n; ++i; }
                if (i < s.size() && s[i] == L'"') {
                    a.append(n / 2, L'\\');
                    if (n % 2) { a.push_back(L'"'); ++i; }  // an escaped quote; else the quote is handled next
                } else {
                    a.append(n, L'\\');
                }
                continue;
            }
            if (c == L'"') {
                if (quoted && i + 1 < s.size() && s[i + 1] == L'"') { a.push_back(L'"'); i += 2; continue; }
                quoted = !quoted;
                ++i;
                continue;
            }
            a.push_back(c);
            ++i;
        }
        args.push_back(std::move(a));
    }
    return args;
}

// ============================================================================ paths

#ifdef _WIN32
static constexpr const wchar_t* kSeparators = L"\\/";
#else
static constexpr const wchar_t* kSeparators = L"/";  // a backslash is an ordinary character in a file name
#endif

std::wstring BaseName(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && wcschr(kSeparators, p.back())) p.pop_back();
    const size_t i = p.find_last_of(kSeparators);
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring NormalizePath(const std::wstring& path) {
    std::wstring p = path;
#ifdef _WIN32
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() >= 2 && p[1] == L':') p[0] = (wchar_t)std::towupper(p[0]);
#else
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
#endif
    return p;
}

std::wstring PathJoin(const std::wstring& dir, std::wstring_view rel) {
    std::wstring p = dir;
    if (!p.empty() && !wcschr(kSeparators, p.back())) p.push_back(kPathSep);
    for (wchar_t c : rel) p.push_back(c == L'/' ? kPathSep : c);
    return p;
}

// ============================================================================ trimming

std::string_view TrimSv(std::string_view s) {
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.remove_prefix(1);
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.remove_suffix(1);
    return s;
}

std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace((std::wint_t)s[a])) ++a;
    while (b > a && std::iswspace((std::wint_t)s[b - 1])) --b;
    return s.substr(a, b - a);
}
