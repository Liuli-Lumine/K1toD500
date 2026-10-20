// xlsxgen.h —— 用内置的空白信道表做底, 填好数据后重新打包成 xlsx
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

// 一行信道 (对应 K1 表格里的 # 1..200)
struct K1Row {
    int no = 0;             // 1..200
    bool used = false;      // false = 空信道, 只写 #
    std::string name;       // UTF-8
    int narrow = 0;         // 0 宽带 / 1 窄带
    double rx = 0, tx = 0;  // MHz
    int power = 0;          // 0 高 / 1 中 / 2 低
    bool busy = false;
    int rTone = 0;          // 0 无 / 1..210 数字 / >210 模拟(Hz*10)
    int tTone = 0;
};

// 以 tpl 为模板: 保留表头与其余结构, 只重写第 2..201 行
bool BuildK1Xlsx(const uint8_t* tpl, size_t tplLen, const std::vector<K1Row>& rows,
                 std::vector<uint8_t>& out, std::string& err);

}  // namespace k1d500
