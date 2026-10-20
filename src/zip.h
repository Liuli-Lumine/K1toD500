// zip.h —— 最小 ZIP 读写 (EOCD + 中央目录; stored/deflate)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t localOff = 0;
};

bool ZipList(const std::vector<uint8_t>& zip, std::vector<ZipEntry>& out, std::string& err);
const ZipEntry* ZipFind(const std::vector<ZipEntry>& list, const std::string& name);
bool ZipExtract(const std::vector<uint8_t>& zip, const ZipEntry& e, std::vector<uint8_t>& out,
                std::string& err);
// 取出条目未经解压的原始字节 (用于「原样搬过去」)
bool ZipRaw(const std::vector<uint8_t>& zip, const ZipEntry& e, std::vector<uint8_t>& raw,
            std::string& err);

// ---- 写 ----
struct ZipOutItem {
    std::string name;
    uint16_t method = 0;  // 0 = stored, 8 = deflate (压缩数据须已压好)
    uint32_t crc = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    std::vector<uint8_t> data;  // 实际写入的字节 (stored 时就是原文)
};

uint32_t Crc32(const uint8_t* data, size_t len);
bool ZipWrite(const std::vector<ZipOutItem>& items, std::vector<uint8_t>& out, std::string& err);

}  // namespace k1d500
