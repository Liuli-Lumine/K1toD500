// reverse.cpp —— D500 .dat 读回 -> 生成泉盛信道表格 (用内置空白表做底)
#include "reverse.h"

#include "d500dat.h"
#include "util.h"
#include "xlsxgen.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace k1d500 {
namespace {

struct LogBuf {
    std::string s;
    void Line(const std::string& t) {
        s += t;
        s += '\n';
    }
    void Warn(const std::string& t) { Line("    ! " + t); }
};

std::string N(long long v) { return std::to_string(v); }

std::wstring FileTimeText(unsigned long long ft) {
    FILETIME f;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFull);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    FILETIME local;
    FileTimeToLocalFileTime(&f, &local);
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&local, &st)) return std::wstring();
    wchar_t b[40];
    std::swprintf(b, 40, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute);
    return std::wstring(b);
}

// D500 .dat 的“文件头”特征: 1025 行, 每行 19 个 0..255 的字段,
// 且前三个字段必须是 [bank] [offset] [linear] = [i/16] [i%16] [i]
bool CheckD500Structure(const std::vector<Row>& rows, std::string& why) {
    if ((int)rows.size() != ROWS) {
        why = "行数是 " + N((long long)rows.size()) + "，D500 信道文件应为 " + N(ROWS) + " 行";
        return false;
    }
    for (int i = 0; i < ROWS; ++i) {
        const Row& r = rows[(size_t)i];
        if (r[0] != i / 16 || r[1] != i % 16 || r[2] != i) {
            why = "第 " + N(i + 1) + " 行的前三个字段不是 D500 的 bank/offset/linear 结构";
            return false;
        }
        // 只有后 16 个字节必须是 0..255; 前三个是 bank/offset/linear (linear 可到 1024)
        for (int k = 3; k < 19; ++k) {
            if (r[(size_t)k] < 0 || r[(size_t)k] > 255) {
                why = "第 " + N(i + 1) + " 行有超出 0..255 的字节";
                return false;
            }
        }
    }
    return true;
}

bool SlotHasData(const std::vector<Row>& rows, int slot) {
    for (int k = 0; k < 16; ++k)
        if (rows[(size_t)slot][(size_t)(3 + k)] != (int)EMPTY) return true;
    return false;
}

}  // namespace

std::vector<DatFile> ListDatFiles(const std::wstring& dir) {
    std::vector<DatFile> out;
    WIN32_FIND_DATAW fd;
    std::wstring pat = Join(dir, L"*.dat");
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.size() >= 2 && name[0] == L'~' && name[1] == L'$') continue;
        DatFile d;
        d.path = Join(dir, name);
        d.mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                  (unsigned long long)fd.ftLastWriteTime.dwLowDateTime;
        d.modified = FileTimeText(d.mtime);
        out.push_back(d);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(),
              [](const DatFile& a, const DatFile& b) { return a.mtime > b.mtime; });
    return out;
}

std::wstring DefaultReverseOutputPath(const std::wstring& dir) {
    return Join(dir, L"K5Channel" + U8(TodayYymmdd()) + L".xlsx");
}

ReverseResult ConvertDatToK1(const uint8_t* xlsxTemplate, size_t tplLen,
                             const std::wstring& datPath, const std::wstring& outPath) {
    ReverseResult res;
    LogBuf log;
    try {
        std::vector<uint8_t> bytes;
        if (!ReadAll(datPath, bytes)) {
            log.Line("读取失败: " + WToU8(datPath));
            res.log = log.s;
            return res;
        }
        std::vector<Row> rows;
        std::string err;
        if (!ParseDat(bytes, rows, err)) {
            log.Line(WToU8(BaseName(datPath)) + " 不是 D500 信道文件: " + err);
            res.log = log.s;
            return res;
        }
        if (!CheckD500Structure(rows, err)) {
            log.Line(WToU8(BaseName(datPath)) + " 不是 D500 信道文件: " + err);
            res.log = log.s;
            return res;
        }
        log.Line("格式检查 : " + N(ROWS) + " 行 × 19 字段 OK（D500 信道文件）");

        std::vector<K1Row> k1((size_t)D500_CH_COUNT);
        for (int i = 0; i < D500_CH_COUNT; ++i) {
            k1[(size_t)i].no = i + 1;
            k1[(size_t)i].used = false;
        }
        int used = 0;
        for (int slot = 0; slot < D500_CH_COUNT; ++slot) {
            if (!SlotHasData(rows, slot)) continue;
            int no = (slot == 0) ? 200 : slot;  // 与正向规则对应: 第 200 信道 <-> 0 号槽
            K1Row& dst = k1[(size_t)(no - 1)];

            uint8_t p[16];
            for (int k = 0; k < 16; ++k) p[k] = (uint8_t)rows[(size_t)slot][(size_t)(3 + k)];
            double rx = 0, tx = 0;
            std::string fe;
            if (!DecFreq(p, rx, fe) || !DecFreq(p + 4, tx, fe)) {
                log.Warn("槽 " + N(slot) + " 的收发频率不是 BCD（" + fe + "），该信道已跳过");
                continue;
            }
            dst.used = true;
            dst.rx = rx;
            dst.tx = tx;
            dst.narrow = (p[15] & NARROW_BIT) ? 1 : 0;
            dst.power = (p[14] <= 2) ? p[14] : 0;
            dst.busy = (p[15] & BUSY_BIT) != 0;
            dst.rTone = DecTone(p + 8);
            dst.tTone = DecTone(p + 10);

            uint8_t nameBuf[16];
            for (int k = 0; k < 16; ++k)
                nameBuf[k] = (uint8_t)rows[(size_t)(NAME_BASE + slot)][(size_t)(3 + k)];
            size_t nl = 0;
            while (nl < 16 && nameBuf[nl] != EMPTY) ++nl;
            dst.name = GbkToText(nameBuf, nl);
            ++used;
        }

        std::vector<uint8_t> xlsx;
        if (!BuildK1Xlsx(xlsxTemplate, tplLen, k1, xlsx, err)) {
            log.Line("生成表格失败: " + err);
            res.log = log.s;
            return res;
        }
        const std::wstring out = outPath;
        if (!WriteAll(out, xlsx.data(), xlsx.size())) {
            log.Line("写入失败: " + WToU8(out) + " (目录是否可写?)");
            res.log = log.s;
            return res;
        }

        log.Line("读取信道 : 信道区有内容 " + N(used) + " 条");
        log.Line("生成表格 : " + WToU8(out));
        log.Line("           共 " + N(D500_CH_COUNT) + " 行：有内容 " + N(used) + " 条，其余 " +
                 N(D500_CH_COUNT - used) + " 条留空（转换回电台时按默认值处理）");
        log.Line("           频率步进/信道模式/扫描列表已按默认填好（V 段 25、U 段 12.5；航空段 AM；扫描 I,II）");
        log.Line("           未支持项（倒频/加密/信令/DTMF）留空，需要时手动填");

        res.code = 0;
        res.outPath = out;
        res.channels = used;
        res.log = log.s;
        return res;
    } catch (const std::exception& e) {
        log.Line(std::string("内部错误: ") + e.what());
        res.log = log.s;
        res.code = 1;
        return res;
    } catch (...) {
        log.Line("内部错误(未知)");
        res.log = log.s;
        res.code = 1;
        return res;
    }
}

}  // namespace k1d500
