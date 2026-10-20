// reverse.h —— 反向转换: D500 .dat -> 泉盛信道表格 (xlsx)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

struct DatFile {
    std::wstring path;
    unsigned long long mtime = 0;  // FILETIME (用于排序)
    std::wstring modified;         // 本地时间文本, 方便显示
};

// 目录下所有 *.dat (排除 ~$ 临时文件), 按修改时间从新到旧
std::vector<DatFile> ListDatFiles(const std::wstring& dir);

struct ReverseResult {
    int code = 1;  // 0 成功
    std::string log;
    std::wstring outPath;
    int channels = 0;  // 有内容的信道数
    bool tooLarge = false;  // 输入文件超过大小上限
};

// 反向转换的默认输出名: K5Channel<今天日期>.xlsx
std::wstring DefaultReverseOutputPath(const std::wstring& dir);



// 写出的路径由调用方决定 (重名时由界面问过用户: 替换 / 保留两者 / 取消)
ReverseResult ConvertDatToK1(const uint8_t* xlsxTemplate, size_t tplLen,
                             const std::wstring& datPath, const std::wstring& outPath);

}  // namespace k1d500
