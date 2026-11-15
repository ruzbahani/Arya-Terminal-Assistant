#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <bcrypt.h>
#  include <shellapi.h>
#else
#  include <arpa/inet.h>
#  include <dirent.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <pwd.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#    include <sys/sysctl.h>
#  endif
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace arya {

#ifdef _WIN32
const bool IS_WIN = true;
const char SEP = '\\';
#else
const bool IS_WIN = false;
const char SEP = '/';
#endif

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
inline bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
inline bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

#ifdef _WIN32
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

inline void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

inline bool utf8_append(std::string& carry, const char* data, size_t n, std::string& out) {
    std::string buf = carry;
    buf.append(data, n);
    carry.clear();
    bool clean = true;
    size_t i = 0;
    while (i < buf.size()) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len > 0;
        if (ok && i + len > buf.size()) {
            for (size_t k = i + 1; k < buf.size(); k++) if (((unsigned char)buf[k] & 0xC0) != 0x80) ok = false;
            if (ok) { carry = buf.substr(i); break; }
        }
        if (ok) {
            for (int k = 1; k < len; k++) if (((unsigned char)buf[i + k] & 0xC0) != 0x80) ok = false;
            unsigned char c1 = (unsigned char)buf[i + 1];
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) ok = false;
        }
        if (!ok) { out += "\xEF\xBF\xBD"; clean = false; i++; continue; }
        out.append(buf, i, len);
        i += len;
    }
    return clean;
}

inline void random_bytes(unsigned char* buf, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#elif defined(__APPLE__)
    arc4random_buf(buf, n);
    return;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { size_t got = fread(buf, 1, n, f); fclose(f); if (got == n) return; }
#endif
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(std::rand() ^ (int)std::time(nullptr));
}
inline std::string random_token(size_t bytes) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<unsigned char> b(bytes);
    random_bytes(b.data(), bytes);
    std::string out;
    for (size_t i = 0; i < bytes; i++) out += A[b[i] & 63];
    return out;
}
inline std::string random_hex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    random_bytes(b.data(), bytes);
    static const char* H = "0123456789abcdef";
    std::string out;
    for (auto c : b) { out += H[c >> 4]; out += H[c & 15]; }
    return out;
}

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* get(const char* key) const {
        if (t != OBJ) return nullptr;
        for (auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const char* key) const { const JVal* v = get(key); return v && v->t == STR ? v->s : std::string(); }
    bool flag(const char* key) const { const JVal* v = get(key); return v && ((v->t == BOOL && v->b) || (v->t == NUM && v->n != 0)); }
};

class JParser {
public:
    explicit JParser(const std::string& s) : s_(s) {}
    bool parse_all(JVal& v) { if (!value(v, 0)) return false; ws(); return i_ == s_.size(); }

private:
    const std::string& s_;
    size_t i_ = 0;

    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++; }
    bool lit(const char* w) { size_t n = strlen(w); if (s_.compare(i_, n, w) != 0) return false; i_ += n; return true; }
    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        i_++;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo = 0;
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') { i_ += 2; if (!hex4(lo)) return false; }
                        cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(JVal& v, int depth) {
        if (depth > 100) return false;
        ws();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') {
            v.t = JVal::OBJ;
            i_++; ws();
            if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
            for (;;) {
                ws();
                if (i_ >= s_.size() || s_[i_] != '"') return false;
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                i_++;
                JVal item;
                if (!value(item, depth + 1)) return false;
                v.o.emplace_back(std::move(key), std::move(item));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = JVal::ARR;
            i_++; ws();
            if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
            for (;;) {
                JVal item;
                if (!value(item, depth + 1)) return false;
                v.a.push_back(std::move(item));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::STR; return string(v.s); }
        if (lit("true")) { v.t = JVal::BOOL; v.b = true; return true; }
        if (lit("false")) { v.t = JVal::BOOL; v.b = false; return true; }
        if (lit("null")) { v.t = JVal::NUL; return true; }
        size_t st = i_;
        if (i_ < s_.size() && s_[i_] == '-') i_++;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) i_++;
        if (i_ == st) return false;
        v.t = JVal::NUM;
        v.n = strtod(s_.substr(st, i_ - st).c_str(), nullptr);
        return true;
    }
};

inline bool json_parse(const std::string& text, JVal& out) { return JParser(text).parse_all(out); }

inline void json_esc(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}
inline std::string jq(const std::string& s) { std::string o; json_esc(o, s); return o; }

inline void json_dump(std::string& out, const JVal& v, int indent = -1, int level = 0) {
    auto nl = [&](int lv) { if (indent >= 0) { out += '\n'; out.append((size_t)(indent * lv), ' '); } };
    switch (v.t) {
        case JVal::NUL: out += "null"; break;
        case JVal::BOOL: out += v.b ? "true" : "false"; break;
        case JVal::NUM: {
            char b[40];
            if (v.n == (double)(long long)v.n && v.n < 9e15 && v.n > -9e15) snprintf(b, sizeof b, "%lld", (long long)v.n);
            else snprintf(b, sizeof b, "%.17g", v.n);
            out += b;
            break;
        }
        case JVal::STR: json_esc(out, v.s); break;
        case JVal::ARR:
            out += '[';
            for (size_t k = 0; k < v.a.size(); k++) { if (k) out += ','; nl(level + 1); json_dump(out, v.a[k], indent, level + 1); }
            if (!v.a.empty()) nl(level);
            out += ']';
            break;
        case JVal::OBJ:
            out += '{';
            for (size_t k = 0; k < v.o.size(); k++) {
                if (k) out += ',';
                nl(level + 1);
                json_esc(out, v.o[k].first);
                out += indent >= 0 ? ": " : ":";
                json_dump(out, v.o[k].second, indent, level + 1);
            }
            if (!v.o.empty()) nl(level);
            out += '}';
            break;
    }
}

}
