// xlsx.cpp —— 不依赖任何第三方库的 xlsx 读表实现。
//
// 只做 xlsx 里真正用得到的三件事:
//   1. zip 里取出 workbook.xml / rels / sharedStrings.xml / 目标 sheet xml
//   2. 扫描 XML 标签, 取单元格文本 (共享字符串、内联字符串、普通值)
//   3. 按 (行, 列) 建表, 与 Python 版 read_sheet() 的行为保持一致
#include "xlsx.h"

#include "util.h"
#include "zip.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace k1d500 {
namespace {

const size_t npos = (size_t)-1;

struct Tag {
    std::string name;
    std::map<std::string, std::string> attrs;
    bool selfClose = false;
    size_t end = 0;  // '>' 的位置
};

std::string XmlUnescape(const std::string& in) {
    if (in.find('&') == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back(in[i++]);
            continue;
        }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") {
            out.push_back('&');
        } else if (ent == "lt") {
            out.push_back('<');
        } else if (ent == "gt") {
            out.push_back('>');
        } else if (ent == "quot") {
            out.push_back('"');
        } else if (ent == "apos") {
            out.push_back('\'');
        } else if (ent.size() > 1 && ent[0] == '#') {
            unsigned long cp = 0;
            bool ok = true;
            try {
                if (ent[1] == 'x' || ent[1] == 'X')
                    cp = std::stoul(ent.substr(2), nullptr, 16);
                else
                    cp = std::stoul(ent.substr(1), nullptr, 10);
            } catch (...) {
                ok = false;
            }
            if (ok && cp > 0 && cp <= 0x10FFFF) {
                std::vector<uint32_t> v{(uint32_t)cp};
                out += Utf8Encode(v);
            }
        } else {
            out += in.substr(i, semi - i + 1);
        }
        i = semi + 1;
    }
    return out;
}

size_t FindTagStart(const std::string& x, size_t from, const std::string& name) {
    size_t p = from;
    for (;;) {
        p = x.find("<" + name, p);
        if (p == npos) return npos;
        size_t after = p + 1 + name.size();
        if (after < x.size()) {
            char c = x[after];
            if (c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n') return p;
        }
        p = after;
    }
}

bool ParseTag(const std::string& x, size_t p, Tag& t) {
    if (p >= x.size() || x[p] != '<') return false;
    size_t e = x.find('>', p);
    if (e == npos) return false;
    std::string body = x.substr(p + 1, e - p - 1);
    t = Tag();
    t.end = e;
    if (!body.empty() && body.back() == '/') {
        t.selfClose = true;
        body.pop_back();
    }
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t i = 0;
    while (i < body.size() && !isws(body[i])) t.name.push_back(body[i++]);
    while (i < body.size()) {
        while (i < body.size() && isws(body[i])) ++i;
        if (i >= body.size()) break;
        size_t ks = i;
        while (i < body.size() && !isws(body[i]) && body[i] != '=') ++i;
        std::string key = body.substr(ks, i - ks);
        while (i < body.size() && isws(body[i])) ++i;
        if (i < body.size() && body[i] == '=') {
            ++i;
            while (i < body.size() && isws(body[i])) ++i;
            if (i < body.size() && (body[i] == '"' || body[i] == '\'')) {
                char q = body[i++];
                size_t vs = i;
                size_t ve = body.find(q, i);
                if (ve == npos) ve = body.size();
                if (!key.empty()) t.attrs[key] = XmlUnescape(body.substr(vs, ve - vs));
                i = ve + 1;
            } else {
                size_t vs = i;
                while (i < body.size() && !isws(body[i]) && body[i] != '/') ++i;
                if (!key.empty()) t.attrs[key] = XmlUnescape(body.substr(vs, i - vs));
            }
        } else if (!key.empty()) {
            t.attrs[key] = "";
        }
    }
    return true;
}

// 取元素内容区间 [beg, en)
bool ElementContent(const std::string& x, size_t afterTagEnd, const std::string& name, size_t& beg,
                    size_t& en) {
    size_t c = x.find("</" + name, afterTagEnd);
    if (c == npos) return false;
    size_t ce = x.find('>', c);
    if (ce == npos) return false;
    beg = afterTagEnd;
    en = c;
    return true;
}

int ColIndexOf(const std::string& ref) {
    int n = 0;
    for (char ch : ref) {
        if (ch >= 'A' && ch <= 'Z') {
            n = n * 26 + (ch - 'A' + 1);
        } else if (ch >= 'a' && ch <= 'z') {
            n = n * 26 + (ch - 'a' + 1);
        } else {
            break;
        }
    }
    return n - 1;
}

// 收集 [beg,en) 内所有 <t> 的文本并拼接 (富文本 run 与 inlineStr 共用)
std::string CollectTexts(const std::string& x, size_t beg, size_t en) {
    std::string txt;
    size_t p = beg;
    while (p < en) {
        size_t t = FindTagStart(x, p, "t");
        if (t == npos || t > en) break;
        Tag tt;
        if (!ParseTag(x, t, tt)) break;
        if (tt.selfClose) {
            p = tt.end + 1;
            continue;
        }
        size_t tb, te;
        if (ElementContent(x, tt.end + 1, "t", tb, te)) {
            if (te > en) te = en;
            txt += XmlUnescape(x.substr(tb, te - tb));
            p = x.find("</t", tb);
            if (p == npos) break;
            p = x.find('>', p);
            if (p == npos) break;
            ++p;
        } else {
            p = tt.end + 1;
        }
    }
    return txt;
}

std::string SheetPath(const std::vector<uint8_t>& zipBytes, const std::vector<ZipEntry>& entries,
                      const std::string& sheetName) {
    const ZipEntry* wb = ZipFind(entries, "xl/workbook.xml");
    const ZipEntry* rels = ZipFind(entries, "xl/_rels/workbook.xml.rels");
    if (wb && rels) {
        std::vector<uint8_t> wbb, rb;
        std::string e;
        if (ZipExtract(zipBytes, *wb, wbb, e) && ZipExtract(zipBytes, *rels, rb, e)) {
            std::string wbx((const char*)wbb.data(), wbb.size());
            std::string rbx((const char*)rb.data(), rb.size());
            std::map<std::string, std::string> target;
            size_t pos = 0;
            for (;;) {
                size_t p = FindTagStart(rbx, pos, "Relationship");
                if (p == npos) break;
                Tag t;
                if (!ParseTag(rbx, p, t)) break;
                target[t.attrs["Id"]] = t.attrs["Target"];
                pos = t.end + 1;
            }
            pos = 0;
            for (;;) {
                size_t p = FindTagStart(wbx, pos, "sheet");
                if (p == npos) break;
                Tag t;
                if (!ParseTag(wbx, p, t)) break;
                if (t.attrs["name"] == sheetName) {
                    auto it = target.find(t.attrs["r:id"]);
                    if (it != target.end()) {
                        std::string s = it->second;
                        while (!s.empty() && s[0] == '/') s.erase(0, 1);
                        if (s.compare(0, 3, "xl/") != 0) s = "xl/" + s;
                        return s;
                    }
                }
                pos = t.end + 1;
            }
        }
    }
    return "xl/worksheets/sheet1.xml";
}

std::vector<std::string> ParseSharedStrings(const std::string& xml) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t p = FindTagStart(xml, pos, "si");
        if (p == npos) break;
        Tag t;
        if (!ParseTag(xml, p, t)) break;
        if (t.selfClose) {
            out.push_back(std::string());
            pos = t.end + 1;
            continue;
        }
        size_t cb, ce;
        if (!ElementContent(xml, t.end + 1, "si", cb, ce)) break;
        out.push_back(CollectTexts(xml, cb, ce));
        pos = ce + 5;
    }
    return out;
}

}  // namespace

bool LoadSheet(const std::wstring& path, const std::string& sheetName, Cells& out,
               std::string& err) {
    out.m.clear();
    err.clear();
    unsigned long long fsz = 0;
    if (FileSizeBytes(path, fsz) && fsz > kMaxInputBytes) {
        err = "文件过大(" + std::to_string(fsz / 1024ull) + " KB)，上限 " +
              std::to_string(kMaxInputBytes / 1024ull) + " KB";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ReadAll(path, bytes)) {
        err = "无法读取文件";
        return false;
    }
    std::vector<ZipEntry> entries;
    if (!ZipList(bytes, entries, err)) return false;

    std::vector<std::string> shared;
    if (const ZipEntry* ss = ZipFind(entries, "xl/sharedStrings.xml")) {
        std::vector<uint8_t> sb;
        std::string e;
        if (ZipExtract(bytes, *ss, sb, e))
            shared = ParseSharedStrings(std::string((const char*)sb.data(), sb.size()));
    }

    std::string target = SheetPath(bytes, entries, sheetName);
    const ZipEntry* se = ZipFind(entries, target);
    if (!se) se = ZipFind(entries, "xl/worksheets/sheet1.xml");
    if (!se) {
        err = "xlsx 里找不到工作表";
        return false;
    }
    std::vector<uint8_t> xb;
    if (!ZipExtract(bytes, *se, xb, err)) return false;
    std::string xml((const char*)xb.data(), xb.size());

    size_t pos = 0;
    for (;;) {
        size_t rp = FindTagStart(xml, pos, "row");
        if (rp == npos) break;
        Tag rt;
        if (!ParseTag(xml, rp, rt)) break;
        int rn = 0;
        {
            auto it = rt.attrs.find("r");
            if (it != rt.attrs.end()) rn = std::atoi(it->second.c_str());
        }
        size_t cb = rt.end + 1, ce = rt.end + 1;
        if (!rt.selfClose) {
            if (!ElementContent(xml, rt.end + 1, "row", cb, ce)) {
                pos = rt.end + 1;
                continue;
            }
        }
        int seq = 0;
        size_t cpos = cb;
        while (cpos < ce) {
            size_t cp = FindTagStart(xml, cpos, "c");
            if (cp == npos || cp >= ce) break;
            Tag ct;
            if (!ParseTag(xml, cp, ct)) break;
            int ci = seq;
            {
                auto it = ct.attrs.find("r");
                if (it != ct.attrs.end()) ci = ColIndexOf(it->second);
            }
            seq = ci + 1;

            size_t cellEnd = ct.end;
            size_t vb = ct.end + 1, ve = ct.end + 1;
            if (!ct.selfClose) {
                size_t eb, ee;
                if (ElementContent(xml, ct.end + 1, "c", eb, ee)) {
                    vb = eb;
                    ve = ee;
                    cellEnd = ee;
                }
            }
            auto typeIt = ct.attrs.find("t");
            std::string typ = typeIt == ct.attrs.end() ? std::string() : typeIt->second;

            std::string val;
            bool have = false;
            size_t vp = FindTagStart(xml, vb, "v");
            if (vp != npos && vp < ve) {
                Tag vt;
                if (ParseTag(xml, vp, vt)) {
                    if (vt.selfClose) {
                        val.clear();
                        have = true;
                    } else {
                        size_t tb, te;
                        if (ElementContent(xml, vt.end + 1, "v", tb, te)) {
                            val = XmlUnescape(xml.substr(tb, te - tb));
                            have = true;
                        }
                    }
                }
            }
            if (!have) {
                size_t ip = FindTagStart(xml, vb, "is");
                if (ip != npos && ip < ve) {
                    Tag it2;
                    if (ParseTag(xml, ip, it2)) {
                        if (it2.selfClose) {
                            val.clear();
                            have = true;
                        } else {
                            size_t ib, ie;
                            if (ElementContent(xml, it2.end + 1, "is", ib, ie)) {
                                val = CollectTexts(xml, ib, ie);
                                have = true;
                            }
                        }
                    }
                }
            }
            if (have) {
                if (typ == "s") {
                    int idx = std::atoi(val.c_str());
                    if (idx >= 0 && (size_t)idx < shared.size()) val = shared[(size_t)idx];
                    else val.clear();
                }
                out.m[std::make_pair(rn, ci)] = TrimAscii(val);
            }
            cpos = cellEnd + 1;
        }
        pos = ce + 6;
    }
    return true;
}

}  // namespace k1d500
