// util.h —— 字符串 / 路径 / 文件 小工具 (Windows + MinGW/MSVC 通用)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

// 读取上限: 满信道的表格其实只有几十 KB, 超过这个数一律当异常文件拒收
constexpr unsigned long long kMaxInputBytes = 2ull * 1024ull * 1024ull;

// ---- 编码转换 ------------------------------------------------------------
std::wstring U8(const std::string& s);          // UTF-8 -> UTF-16
std::string  WToU8(const std::wstring& s);      // UTF-16 -> UTF-8
std::string  GbkChar(uint32_t cp);              // 单个码位 -> GBK 字节 (不可映射 -> "?")
std::vector<uint32_t> Utf8Decode(const std::string& s);
std::string  Utf8Encode(const std::vector<uint32_t>& cps);
std::string  AsciiUpper(std::string s);
std::string  TrimAscii(const std::string& s);
std::string  NfkcLite(const std::string& s);    // 全角 -> 半角 (仅 ASCII 区)
int          DispWidth(const std::string& u8);  // 显示宽度: 中日韩 = 2 列
std::string  PadRight(const std::string& u8, int width);

// ---- 文件 / 路径 ---------------------------------------------------------
bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out);
bool WriteAll(const std::wstring& path, const uint8_t* data, size_t len);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool FileSizeBytes(const std::wstring& path, unsigned long long& size);  // 不打开文件, 只取大小

std::wstring ExePath();
std::wstring ExeDir();
std::wstring BaseName(const std::wstring& path);   // 含扩展名
std::wstring StemName(const std::wstring& path);   // 不含扩展名
std::wstring DirName(const std::wstring& path);
std::wstring Join(const std::wstring& dir, const std::wstring& name);
std::string  TodayYymmdd();                        // 本地日期 yy mm dd
std::string  YymmddFromFileTime(unsigned long long ft);  // FILETIME -> 本地 yy mm dd (失败用今天)
// 保留两者时用: 已在则 "名字 (1).ext"，再有 "名字 (2).ext" …
std::wstring UniqueSuffixedPath(const std::wstring& dir, const std::wstring& stem,
                          const std::wstring& ext);

}  // namespace k1d500
