// zip.cpp
#include "zip.h"

#include "inflate.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace k1d500 {
namespace {

uint16_t R16(const std::vector<uint8_t>& d, size_t o) {
    return (uint16_t)((uint16_t)d[o] | ((uint16_t)d[o + 1] << 8));
}
uint32_t R32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) |
           ((uint32_t)d[o + 3] << 24);
}

void Put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v & 0xFF));
    o.push_back((uint8_t)(v >> 8));
}
void Put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v & 0xFF));
    o.push_back((uint8_t)((v >> 8) & 0xFF));
    o.push_back((uint8_t)((v >> 16) & 0xFF));
    o.push_back((uint8_t)((v >> 24) & 0xFF));
}

}  // namespace

bool ZipList(const std::vector<uint8_t>& zip, std::vector<ZipEntry>& out, std::string& err) {
    out.clear();
    err.clear();
    if (zip.size() < 22) {
        err = "文件太小, 不是 zip";
        return false;
    }
    size_t eocd = (size_t)-1;
    size_t lowest = zip.size() >= 22 + 65535 ? zip.size() - 22 - 65535 : 0;
    for (size_t i = zip.size() - 22 + 1; i-- > lowest;) {
        if (R32(zip, i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == (size_t)-1) {
        err = "找不到 zip 中央目录结尾 (EOCD)";
        return false;
    }
    uint16_t total = R16(zip, eocd + 10);
    uint32_t cdOff = R32(zip, eocd + 16);
    if (total == 0xFFFF || cdOff == 0xFFFFFFFFu) {
        err = "不支持 ZIP64";
        return false;
    }
    size_t pos = cdOff;
    for (uint16_t k = 0; k < total; ++k) {
        if (pos + 46 > zip.size() || R32(zip, pos) != 0x02014b50u) {
            err = "中央目录条目损坏";
            return false;
        }
        ZipEntry e;
        e.method = R16(zip, pos + 10);
        e.crc = R32(zip, pos + 16);
        e.compSize = R32(zip, pos + 20);
        e.uncompSize = R32(zip, pos + 24);
        uint16_t nameLen = R16(zip, pos + 28);
        uint16_t extraLen = R16(zip, pos + 30);
        uint16_t commentLen = R16(zip, pos + 32);
        e.localOff = R32(zip, pos + 42);
        if (pos + 46 + nameLen > zip.size()) {
            err = "中央目录条目名字越界";
            return false;
        }
        e.name.assign((const char*)zip.data() + pos + 46, nameLen);
        out.push_back(e);
        pos += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ZipFind(const std::vector<ZipEntry>& list, const std::string& name) {
    for (const auto& e : list)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipExtract(const std::vector<uint8_t>& zip, const ZipEntry& e, std::vector<uint8_t>& out,
                std::string& err) {
    out.clear();
    err.clear();
    size_t lo = e.localOff;
    if (lo + 30 > zip.size() || R32(zip, lo) != 0x04034b50u) {
        err = "本地文件头损坏: " + e.name;
        return false;
    }
    uint16_t nameLen = R16(zip, lo + 26);
    uint16_t extraLen = R16(zip, lo + 28);
    size_t dataOff = lo + 30 + nameLen + extraLen;
    if (dataOff + e.compSize > zip.size()) {
        err = "条目数据越界: " + e.name;
        return false;
    }
    if (e.method == 0) {
        out.assign(zip.begin() + (ptrdiff_t)dataOff,
                   zip.begin() + (ptrdiff_t)(dataOff + e.compSize));
    } else if (e.method == 8) {
        if (!InflateRaw(zip.data() + dataOff, e.compSize, e.uncompSize, out, err)) {
            err = e.name + ": " + err;
            return false;
        }
    } else {
        err = "不支持的压缩方法 " + std::to_string(e.method) + ": " + e.name;
        return false;
    }
    // CRC 校验: 压缩包里每个条目都带校验码, 对不上说明文件损坏 / 被改过
    if (Crc32(out.data(), out.size()) != e.crc) {
        err = "CRC 校验失败(文件可能已损坏): " + e.name;
        out.clear();
        return false;
    }
    return true;
}

bool ZipRaw(const std::vector<uint8_t>& zip, const ZipEntry& e, std::vector<uint8_t>& raw,
            std::string& err) {
    raw.clear();
    err.clear();
    size_t lo = e.localOff;
    if (lo + 30 > zip.size() || R32(zip, lo) != 0x04034b50u) {
        err = "本地文件头损坏: " + e.name;
        return false;
    }
    uint16_t nameLen = R16(zip, lo + 26);
    uint16_t extraLen = R16(zip, lo + 28);
    size_t dataOff = lo + 30 + nameLen + extraLen;
    if (dataOff + e.compSize > zip.size()) {
        err = "条目数据越界: " + e.name;
        return false;
    }
    raw.assign(zip.begin() + (ptrdiff_t)dataOff,
               zip.begin() + (ptrdiff_t)(dataOff + e.compSize));
    return true;
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool ZipWrite(const std::vector<ZipOutItem>& items, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    SYSTEMTIME st;
    GetLocalTime(&st);
    uint16_t dosTime =
        (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    uint16_t dosDate =
        (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);

    struct Central {
        const ZipOutItem* it;
        uint32_t localOff;
    };
    std::vector<Central> central;
    central.reserve(items.size());

    for (const auto& it : items) {
        Central c;
        c.it = &it;
        c.localOff = (uint32_t)out.size();
        central.push_back(c);

        Put32(out, 0x04034b50u);
        Put16(out, 20);  // version needed
        Put16(out, 0);   // flags
        Put16(out, it.method);
        Put16(out, dosTime);
        Put16(out, dosDate);
        Put32(out, it.crc);
        Put32(out, it.compSize);
        Put32(out, it.uncompSize);
        Put16(out, (uint16_t)it.name.size());
        Put16(out, 0);  // extra
        out.insert(out.end(), it.name.begin(), it.name.end());
        out.insert(out.end(), it.data.begin(), it.data.end());
    }

    uint32_t cdOff = (uint32_t)out.size();
    for (const auto& c : central) {
        const ZipOutItem& it = *c.it;
        Put32(out, 0x02014b50u);
        Put16(out, 20);  // version made by
        Put16(out, 20);  // version needed
        Put16(out, 0);
        Put16(out, it.method);
        Put16(out, dosTime);
        Put16(out, dosDate);
        Put32(out, it.crc);
        Put32(out, it.compSize);
        Put32(out, it.uncompSize);
        Put16(out, (uint16_t)it.name.size());
        Put16(out, 0);  // extra
        Put16(out, 0);  // comment
        Put16(out, 0);  // disk start
        Put16(out, 0);  // internal attrs
        Put32(out, 0);  // external attrs
        Put32(out, c.localOff);
        out.insert(out.end(), it.name.begin(), it.name.end());
    }
    uint32_t cdSize = (uint32_t)out.size() - cdOff;

    Put32(out, 0x06054b50u);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, (uint16_t)items.size());
    Put16(out, (uint16_t)items.size());
    Put32(out, cdSize);
    Put32(out, cdOff);
    Put16(out, 0);
    return true;
}

}  // namespace k1d500
