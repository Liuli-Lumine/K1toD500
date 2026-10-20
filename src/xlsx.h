// xlsx.h —— 极简 xlsx 读取 (zip + SpreadsheetML), 返回 {(行号1基, 列号0基): 文本}
#pragma once
#include <map>
#include <string>
#include <utility>

namespace k1d500 {

struct Cells {
    std::map<std::pair<int, int>, std::string> m;

    std::string Get(int row, int col) const {
        auto it = m.find(std::make_pair(row, col));
        return it == m.end() ? std::string() : it->second;
    }
    int MaxRow() const {
        int mx = 0;
        for (const auto& kv : m) mx = kv.first.first > mx ? kv.first.first : mx;
        return mx;
    }
};

bool LoadSheet(const std::wstring& path, const std::string& sheetName, Cells& out,
               std::string& err);

}  // namespace k1d500
