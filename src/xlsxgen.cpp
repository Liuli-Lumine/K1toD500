// xlsxgen.cpp —— 复用内置空白表 (表头/样式/结构全部保留), 只替换 sheetData。
#include "xlsxgen.h"

#include "d500dat.h"
#include "util.h"
#include "zip.h"

#include <cstdio>
#include <cstring>

namespace k1d500 {
namespace {

std::string ColName(int col) {  // 0 -> A
    std::string s;
    int n = col + 1;
    while (n > 0) {
        int r = (n - 1) % 26;
        s.insert(s.begin(), (char)('A' + r));
        n = (n - 1) / 26;
    }
    return s;
}

std::string XmlEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20) continue;  // XML 1.0 不允许的控制字符, 直接丢掉
        switch (c) {
            case '&':
                o += "&amp;";
                break;
            case '<':
                o += "&lt;";
                break;
            case '>':
                o += "&gt;";
                break;
            default:
                o.push_back(c);
        }
    }
    return o;
}

std::string CellText(int row, int col, const std::string& v) {
    return "<c r=\"" + ColName(col) + std::to_string(row) + "\" t=\"str\"><v>" + XmlEsc(v) +
           "</v></c>";
}

std::string CellNum(int row, int col, long long v) {
    return "<c r=\"" + ColName(col) + std::to_string(row) + "\"><v>" + std::to_string(v) +
           "</v></c>";
}

// 航空频段 (108–137 MHz) 用 AM
bool IsAirBand(double mhz) { return mhz >= 108.0 && mhz < 137.0; }

std::string ToneTypeText(int v) {
    if (v == 0) return std::string();
    if (v <= TONE_DIGITAL_MAX) return v > DCS_INVERT_OFFSET ? "反向数字亚音" : "数字亚音";
    return "亚音频";
}

const char* PowerText(int p) {
    switch (p) {
        case 1:
            return "中";
        case 2:
            return "低";
        default:
            return "高";
    }
}

std::string RowXml(int rowNo, const K1Row& r) {
    std::string x = "<row r=\"" + std::to_string(rowNo) + "\">";
    x += CellNum(rowNo, 0, r.no);  // A: 信道号
    if (r.used) {
        if (!r.name.empty()) x += CellText(rowNo, 1, r.name);
        x += CellText(rowNo, 2, r.narrow ? "12.5KHz" : "25KHz");
        x += CellText(rowNo, 3, FmtMhz(r.rx));
        x += CellText(rowNo, 4, FmtMhz(r.tx));
        x += CellText(rowNo, 5, PowerText(r.power));
        if (r.rTone) {
            x += CellText(rowNo, 6, ToneTypeText(r.rTone));
            if (r.rTone > TONE_DIGITAL_MAX) x += CellText(rowNo, 7, FmtHz(r.rTone / 10.0));
            else x += CellText(rowNo, 8, ToneText(r.rTone));
        }
        if (r.tTone) {
            x += CellText(rowNo, 9, ToneTypeText(r.tTone));
            if (r.tTone > TONE_DIGITAL_MAX) x += CellText(rowNo, 10, FmtHz(r.tTone / 10.0));
            else x += CellText(rowNo, 11, ToneText(r.tTone));
        }
        // 频率步进: V 段 25、U 段 12.5；信道模式: 航空段 AM、其余 FM；扫描列表: I,II
        x += CellText(rowNo, 12, (r.rx < 300.0) ? "25" : "12.5");
        if (r.busy) x += CellText(rowNo, 15, "开");
        x += CellText(rowNo, 17, IsAirBand(r.rx) ? "AM" : "FM");
        x += CellText(rowNo, 19, "I,II");
    }
    x += "</row>";
    return x;
}

}  // namespace

bool BuildK1Xlsx(const uint8_t* tpl, size_t tplLen, const std::vector<K1Row>& rows,
                 std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    if (!tpl || tplLen == 0) {
        err = "内置模板为空";
        return false;
    }
    std::vector<uint8_t> tplBytes(tpl, tpl + tplLen);
    std::vector<ZipEntry> entries;
    if (!ZipList(tplBytes, entries, err)) {
        err = "内置模板不是有效的 xlsx: " + err;
        return false;
    }
    const ZipEntry* se = ZipFind(entries, "xl/worksheets/sheet1.xml");
    if (!se) {
        err = "内置模板里找不到 sheet1.xml";
        return false;
    }
    std::vector<uint8_t> xmlBytes;
    if (!ZipExtract(tplBytes, *se, xmlBytes, err)) return false;
    std::string xml((const char*)xmlBytes.data(), xmlBytes.size());

    size_t sdOpen = xml.find("<sheetData");
    size_t sdOpenEnd = xml.find('>', sdOpen);
    size_t sdClose = xml.find("</sheetData>");
    if (sdOpen == std::string::npos || sdOpenEnd == std::string::npos ||
        sdClose == std::string::npos || sdClose < sdOpenEnd) {
        err = "内置模板的 sheetData 结构异常";
        return false;
    }
    // 表头整行原样保留
    size_t hRow = xml.find("<row ", sdOpenEnd);
    size_t hRowEnd = (hRow == std::string::npos) ? std::string::npos : xml.find("</row>", hRow);
    if (hRow == std::string::npos || hRowEnd == std::string::npos || hRow > sdClose) {
        err = "内置模板缺少表头行";
        return false;
    }
    std::string headerRow = xml.substr(hRow, hRowEnd + 6 - hRow);

    std::string data;
    data.reserve(200 * 260);
    data += headerRow;
    for (size_t i = 0; i < rows.size(); ++i) data += RowXml((int)i + 2, rows[i]);

    std::string newXml = xml.substr(0, sdOpenEnd + 1) + data + xml.substr(sdClose);
    std::vector<uint8_t> newXmlBytes(newXml.begin(), newXml.end());

    // 重新打包: sheet1.xml 换成新内容, 其余条目原样搬
    std::vector<ZipOutItem> items;
    items.reserve(entries.size());
    bool replaced = false;
    for (const auto& e : entries) {
        ZipOutItem it;
        it.name = e.name;
        if (e.name == "xl/worksheets/sheet1.xml") {
            it.method = 0;
            it.data = newXmlBytes;
            it.compSize = (uint32_t)it.data.size();
            it.uncompSize = (uint32_t)it.data.size();
            it.crc = Crc32(it.data.data(), it.data.size());
            replaced = true;
        } else {
            if (!ZipRaw(tplBytes, e, it.data, err)) return false;
            it.method = e.method;
            it.crc = e.crc;
            it.compSize = e.compSize;
            it.uncompSize = e.uncompSize;
        }
        items.push_back(std::move(it));
    }
    if (!replaced) {
        err = "内置模板里没有替换到工作表";
        return false;
    }
    return ZipWrite(items, out, err);
}

}  // namespace k1d500
