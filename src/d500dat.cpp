// d500dat.cpp
#include "d500dat.h"

#include "util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace k1d500 {

const char* const DCS_CODES[] = {
    "023", "025", "026", "031", "032", "036", "043", "047", "051", "053", "054",
    "065", "071", "072", "073", "074", "114", "115", "116", "122", "125", "131",
    "132", "134", "143", "145", "152", "155", "156", "162", "165", "172", "174",
    "205", "212", "223", "225", "226", "243", "244", "245", "246", "251", "252",
    "255", "261", "263", "265", "266", "271", "274", "306", "311", "315", "325",
    "331", "332", "343", "346", "351", "356", "364", "365", "371", "411", "412",
    "413", "423", "431", "432", "445", "446", "452", "454", "455", "462", "464",
    "465", "466", "503", "506", "516", "523", "526", "532", "546", "565", "606",
    "612", "624", "627", "631", "632", "645", "654", "662", "664", "703", "712",
    "723", "731", "732", "734", "743", "754",
};
const int DCS_COUNT = (int)(sizeof(DCS_CODES) / sizeof(DCS_CODES[0]));

namespace {

std::string Hex2(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", v & 0xFF);
    return std::string(b);
}

std::string FmtG(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", v);
    return std::string(b);
}

}  // namespace

bool ParseDat(const std::vector<uint8_t>& bytes, std::vector<Row>& rows, std::string& err) {
    rows.clear();
    size_t i = 0;
    const size_t n = bytes.size();
    while (i < n) {
        size_t lineEnd = i;
        while (lineEnd < n && bytes[lineEnd] != '\r' && bytes[lineEnd] != '\n') ++lineEnd;
        std::string line((const char*)bytes.data() + i, lineEnd - i);
        i = lineEnd;
        while (i < n && (bytes[i] == '\r' || bytes[i] == '\n')) ++i;
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        Row row{};
        int count = 0;
        size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            if (p >= line.size()) break;
            int v = 0;
            bool any = false;
            while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
                v = v * 10 + (line[p] - '0');
                any = true;
                ++p;
            }
            if (!any) {
                err = "回读 .dat 时遇到非数字内容";
                return false;
            }
            if (count >= 19) {
                err = "回读 .dat 时某行字段过多";
                return false;
            }
            row[(size_t)count++] = v;
        }
        if (count != 19) {
            err = "回读 .dat 时某行字段数为 " + std::to_string(count) + " (应为 19)";
            return false;
        }
        rows.push_back(row);
    }
    return true;
}

std::string BuildDatText(const std::vector<Row>& rows) {
    std::string out;
    out.reserve(rows.size() * 80);
    char tmp[8];
    for (const Row& r : rows) {
        for (int k = 0; k < 19; ++k) {
            out.push_back(' ');
            std::snprintf(tmp, sizeof(tmp), "%d", (int)r[(size_t)k]);
            out += tmp;
            out.push_back(' ');
        }
        out += "\r\n";
    }
    return out;
}

bool EncFreq(double mhz, uint8_t* out, std::string& err) {
    double r = std::nearbyint(mhz * 100000.0);  // Python round(): 就近取偶
    if (!(r >= 0.0 && r <= 99999999.0)) {
        err = "频率超出 4 字节 BCD 范围: " + FmtG(mhz);
        return false;
    }
    long long v = (long long)r;
    char s[24];
    std::snprintf(s, sizeof(s), "%08lld", v);
    uint8_t bcd[4];
    for (int i = 0; i < 4; ++i) bcd[i] = (uint8_t)((s[2 * i] - '0') * 16 + (s[2 * i + 1] - '0'));
    out[0] = bcd[3];
    out[1] = bcd[2];
    out[2] = bcd[1];
    out[3] = bcd[0];
    return true;
}

void EncToneValue(int v, uint8_t* out) {
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
}

bool DecFreq(const uint8_t* b4, double& mhz, std::string& err) {
    long long v = 0;
    for (int i = 3; i >= 0; --i) {
        int hi = b4[i] >> 4;
        int lo = b4[i] & 0x0F;
        if (hi > 9 || lo > 9) {
            err = "非 BCD 字节 0x" + Hex2(b4[i]);
            return false;
        }
        v = v * 100 + hi * 10 + lo;
    }
    mhz = (double)v / 100000.0;
    return true;
}

int DecTone(const uint8_t* b2) { return (int)b2[0] | ((int)b2[1] << 8); }

std::string ToneText(int v) {
    if (v == 0) return "-";
    if (v <= TONE_DIGITAL_MAX) {
        bool inv = v > DCS_INVERT_OFFSET;
        int i = inv ? v - DCS_INVERT_OFFSET : v;
        std::string code = (i >= 1 && i <= DCS_COUNT) ? DCS_CODES[i - 1] : ("?" + std::to_string(i));
        return "D" + code + (inv ? "I" : "N");
    }
    return FmtG((double)v / 10.0) + "Hz";
}

std::string GbkToText(const uint8_t* p, size_t n) {
    if (n == 0) return std::string();
    int w = MultiByteToWideChar(936, 0, (const char*)p, (int)n, nullptr, 0);
    if (w <= 0) return std::string();
    std::wstring wide((size_t)w, L'\0');
    MultiByteToWideChar(936, 0, (const char*)p, (int)n, &wide[0], w);
    return WToU8(wide);
}

std::string FmtMhz(double mhz) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.5f", mhz);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string FmtHz(double hz) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", hz);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace k1d500
