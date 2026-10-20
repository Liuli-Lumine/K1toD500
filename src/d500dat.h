// d500dat.h —— D500 .dat 的格式常量与编解码原语 (正/反向转换共用)
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

// ---------------------------------------------------------------- 结构常量
constexpr int ROWS = 1025;          // 总行数
constexpr int CH_COUNT = 224;       // 信道记录区槽数 (linear 0..223)
constexpr int D500_CH_COUNT = 200;  // D500 实际信道数: 信道 1..200 = 槽 0..199
constexpr int NAME_BASE = 256;      // 名称表起始 linear
constexpr uint8_t EMPTY = 0xFF;     // 空信道填充
constexpr uint8_t NARROW_BIT = 0x40;  // byte15 bit6: 1 = 窄带
constexpr uint8_t BUSY_BIT = 0x08;    // byte15 bit3: 1 = 繁忙禁发
constexpr int TONE_DIGITAL_MAX = 210; // 1..105 正向 / 106..210 反向; 更大是模拟
constexpr int DCS_INVERT_OFFSET = 105;

// 数字亚音(DCS)码表 —— 顺序即写频软件下拉列表顺序
extern const char* const DCS_CODES[];
extern const int DCS_COUNT;

// 一行 19 个字段: [bank] [offset] [linear] + 16 字节数据。
// 前三个字段是十进制数值 (linear 可到 1024), 所以用 int 存, 不能用 uint8_t。
using Row = std::array<int, 19>;

// ---------------------------------------------------------------- .dat 读写
bool ParseDat(const std::vector<uint8_t>& bytes, std::vector<Row>& rows, std::string& err);
std::string BuildDatText(const std::vector<Row>& rows);

// ---------------------------------------------------------------- 编解码
bool EncFreq(double mhz, uint8_t* out, std::string& err);  // -> 4 字节小端 BCD
void EncToneValue(int v, uint8_t* out);                    // -> 2 字节小端
bool DecFreq(const uint8_t* b4, double& mhz, std::string& err);
int DecTone(const uint8_t* b2);                            // 0 / 1..210 / Hz*10
std::string ToneText(int v);                               // "-" / "88.5Hz" / "D023N"
std::string GbkToText(const uint8_t* p, size_t n);
std::string FmtMhz(double mhz);                            // 438.5 / 145.6125
std::string FmtHz(double hz);                              // 88.5 / 100

}  // namespace k1d500
