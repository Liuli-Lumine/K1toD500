// core.h —— K1 信道备份 (xlsx) -> 即时通 D500 信道文件 (.dat)
#pragma once
#include <cstddef>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

struct Options {
    std::wstring backupPath;  // 空 = 自动取日期最新的 K5Channel<yymmdd>.xlsx
    std::wstring outDir;      // 空 = 程序所在目录
    bool headerCheck = true;  // 表头列序校验
    int slot0 = 0;            // K1 第 200 信道映射到的 D500 槽
    bool keepDatText = false; // 把生成内容也放进 Result (测试用)
    // 目标文件已存在时的决策: 返回 0 取消 / 1 替换 / 2 保留两者 (不设置则默认保留两者)
    std::function<int(const std::wstring& existingPath)> onConflict;
};

struct Result {
    int code = 1;  // 0 成功 / 1 错误 / 2 结构自检失败 / 3 往返自检失败 / 4 用户取消
    std::string log;       // UTF-8, 多行
    std::wstring outPath;  // 生成的文件路径 (失败为空)
    std::string datText;   // 仅当 Options.keepDatText 时填充
    bool tooLarge = false; // 输入文件超过大小上限 (界面据此单独弹窗)
};

// baseBlob: 内置基准 (577 行 × 16 字节, 来自 data/base.bin)
Result Convert(const uint8_t* baseBlob, size_t baseLen, const Options& opt);

struct BackupItem {
    std::string date;  // 由修改时间推出来的 yy mm dd (永远非空)
    std::wstring path;
    unsigned long long mtime = 0;
};

std::vector<BackupItem> ListBackups(const std::wstring& dir);  // 按修改时间升序

bool PickBackup(const std::wstring& dir, const std::wstring& explicitPath, std::wstring& outPath,
                std::string& yymmdd, std::string& how, std::string& err);

}  // namespace k1d500
