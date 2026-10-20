// util.cpp
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace k1d500 {

std::wstring U8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WToU8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}

std::string GbkChar(uint32_t cp) {
    wchar_t w[2];
    int wlen = 0;
    if (cp <= 0xFFFF) {
        w[0] = (wchar_t)cp;
        wlen = 1;
    } else {
        uint32_t t = cp - 0x10000;
        w[0] = (wchar_t)(0xD800 + (t >> 10));
        w[1] = (wchar_t)(0xDC00 + (t & 0x3FF));
        wlen = 2;
    }
    char buf[16];
    BOOL usedDefault = FALSE;
    int n = WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, w, wlen, buf, (int)sizeof(buf),
                                "?", &usedDefault);
    if (n <= 0) return "?";
    return std::string(buf, (size_t)n);
}

std::vector<uint32_t> Utf8Decode(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t extra;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= s.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += extra + 1;
        out.push_back(cp);
    }
    return out;
}

std::string Utf8Encode(const std::vector<uint32_t>& cps) {
    std::string o;
    o.reserve(cps.size());
    for (uint32_t cp : cps) {
        if (cp < 0x80) {
            o.push_back((char)cp);
        } else if (cp < 0x800) {
            o.push_back((char)(0xC0 | (cp >> 6)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o.push_back((char)(0xE0 | (cp >> 12)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            o.push_back((char)(0xF0 | (cp >> 18)));
            o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return o;
}

std::string AsciiUpper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

std::string NfkcLite(const std::string& s) {
    std::vector<uint32_t> cps = Utf8Decode(s);
    for (uint32_t& cp : cps) {
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;   // ！-～ -> !-~
        else if (cp == 0x3000) cp = 0x20;                  // 全角空格
    }
    return Utf8Encode(cps);
}

int DispWidth(const std::string& u8) {
    int w = 0;
    for (uint32_t cp : Utf8Decode(u8)) w += (cp > 0x2E7F) ? 2 : 1;
    return w;
}

std::string PadRight(const std::string& u8, int width) {
    int w = DispWidth(u8);
    std::string o = u8;
    if (w < width) o.append((size_t)(width - w), ' ');
    return o;
}

// ---------------------------------------------------------------- 文件
bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return false;
    }
    if (sz.QuadPart > 0 && sz.QuadPart < (LONGLONG)1 << 31) {
        out.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (got < out.size()) {
            DWORD want = (DWORD)(out.size() - got);
            DWORD read = 0;
            if (!ReadFile(h, out.data() + got, want, &read, nullptr) || read == 0) break;
            got += read;
        }
        out.resize(got);
    }
    CloseHandle(h);
    return true;
}

bool WriteAll(const std::wstring& path, const uint8_t* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t put = 0;
    bool ok = true;
    while (put < len) {
        DWORD want = (DWORD)((len - put) > 1u << 30 ? (1u << 30) : (len - put));
        DWORD wrote = 0;
        if (!WriteFile(h, data + put, want, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        put += wrote;
    }
    CloseHandle(h);
    return ok;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileSizeBytes(const std::wstring& path, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    size = ((unsigned long long)fad.nFileSizeHigh << 32) | (unsigned long long)fad.nFileSizeLow;
    return true;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t i = p.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? L"." : p.substr(0, i);
}

std::wstring BaseName(const std::wstring& path) {
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? path : path.substr(i + 1);
}

std::wstring StemName(const std::wstring& path) {
    std::wstring b = BaseName(path);
    size_t i = b.find_last_of(L'.');
    return (i == std::wstring::npos || i == 0) ? b : b.substr(0, i);
}

std::wstring DirName(const std::wstring& path) {
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? std::wstring(L".") : path.substr(0, i);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::string TodayYymmdd() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d%02d%02d", st.wYear % 100, st.wMonth, st.wDay);
    return std::string(buf);
}

// FILETIME (UTC, 100ns) -> 本地 yy mm dd; 失败时退回今天, 不会产生空串
std::string YymmddFromFileTime(unsigned long long ft) {
    if (ft == 0) return TodayYymmdd();
    FILETIME f;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFull);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    FILETIME local;
    if (!FileTimeToLocalFileTime(&f, &local)) return TodayYymmdd();
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&local, &st)) return TodayYymmdd();
    if (st.wYear < 1980 || st.wYear > 2200) return TodayYymmdd();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d%02d%02d", st.wYear % 100, st.wMonth, st.wDay);
    return std::string(buf);
}

std::wstring UniqueSuffixedPath(const std::wstring& dir, const std::wstring& stem,
                                const std::wstring& ext) {
    std::wstring p = Join(dir, stem + ext);
    for (int n = 1; FileExists(p) && n < 10000; ++n) {
        p = Join(dir, stem + L" (" + std::to_wstring(n) + L")" + ext);
    }
    return p;
}
}  // namespace k1d500
