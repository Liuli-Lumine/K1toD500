// core.cpp —— Python 版「泉盛UV-K1信道转换到即时通D500.py」的 C++ 复刻。
//
// 规则(与 Python 版逐条一致):
//   * 输出 D500_<yymmdd>.dat, 与程序同目录; 重名加 _1 _2 …
//   * 备份取 K5Channel<yymmdd>.xlsx 中日期最新的一份; 没有带日期的则用 K5Channel.xlsx (日期用今天)
//   * 信道区固定 200 条 (槽 0..199 = D500 信道 1..200), 其余槽恒 0xFF
//   * 信道号严格对应槽号, 唯一例外: 第 200 信道 -> 槽 0
//   * 同号重复以先出现者为准
//   * 位域: 功率 byte14 / 窄带 byte15.bit6 / 繁忙禁发 byte15.bit3
//   * 亚音: 模拟 Hz*10, 数字 = 码表序号 (+105 反向)
//   * 写盘后回读并独立解码, 与编码前预期逐项比对
#include "core.h"

#include "d500dat.h"
#include "util.h"
#include "xlsx.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace k1d500 {
namespace {

// ---------------------------------------------------------------- 常量 (共用的见 d500dat.h)
const int SLOT_BLANK = 0;
const int K1_FIRST_CH = 1;
const int K1_LAST_CH = 200;

const uint8_t BASE_TAIL[4] = {0x00, 0x00, 0x00, 0x04};

// xlsx 列号 (0 基)
enum : int {
    C_NO = 0,
    C_NAME = 1,
    C_BW = 2,
    C_RX = 3,
    C_TX = 4,
    C_PWR = 5,
    C_RTYPE = 6,
    C_RVAL = 7,
    C_RCODE = 8,
    C_TTYPE = 9,
    C_TVAL = 10,
    C_TCODE = 11,
    C_BUSY = 15,
    C_MODE = 17
};

struct HeaderRule {
    int col;
    const char* keys[3];
};

const HeaderRule HEADER_RULES[] = {
    {C_NO, {"#", nullptr, nullptr}},
    {C_NAME, {"信道名", nullptr, nullptr}},
    {C_BW, {"带宽", nullptr, nullptr}},
    {C_RX, {"接收", "频率", nullptr}},
    {C_TX, {"发送", "频率", nullptr}},
    {C_PWR, {"功率", nullptr, nullptr}},
    {C_RTYPE, {"接收", "类型", nullptr}},
    {C_RVAL, {"接收", "HZ", nullptr}},
    {C_RCODE, {"接收", "数码", nullptr}},
    {C_TTYPE, {"发送", "类型", nullptr}},
    {C_TVAL, {"发送", "HZ", nullptr}},
    {C_TCODE, {"发送", "数码", nullptr}},
    {C_BUSY, {"繁忙", nullptr, nullptr}},
};

// ---------------------------------------------------------------- 小工具
struct LogBuf {
    std::string s;
    void Line(const std::string& t) {
        s += t;
        s += '\n';
    }
    void Warn(const std::string& t) { Line("    ! " + t); }
};

std::string N(long long v) { return std::to_string(v); }
std::string FmtG(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", v);
    return std::string(b);
}
std::string Hex2(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", v & 0xFF);
    return std::string(b);
}

// 只认十进制数字: 允许前后空白、最多一个小数点; 不接受正负号、指数、单位、千分位
bool ParseStrictNum(const std::string& raw, double& out) {
    std::string s = TrimAscii(raw);
    if (s.empty()) return false;
    bool dot = false;
    bool digit = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            digit = true;
            continue;
        }
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    if (!digit) return false;
    out = std::strtod(s.c_str(), nullptr);
    return true;
}

// ---------------------------------------------------------------- 编码
// 名称 -> GBK 定长(0xFF 填充), 超长在字符边界截断
std::vector<uint8_t> EncName(const std::string& name, size_t nbytes, size_t& used) {
    std::vector<uint8_t> b;
    for (uint32_t cp : Utf8Decode(name)) {
        std::string g = GbkChar(cp);
        if (b.size() + g.size() > nbytes) break;
        b.insert(b.end(), g.begin(), g.end());
    }
    used = b.size();
    b.resize(nbytes, EMPTY);
    return b;
}

int PowerCode(const std::string& raw) {
    std::string s = TrimAscii(raw);
    if (s == "高" || s == "HIGH" || s == "H") return 0x00;
    if (s == "中" || s == "MID" || s == "M") return 0x01;
    if (s == "低" || s == "LOW" || s == "L") return 0x02;
    return -1;
}

int BwNarrow(const std::string& raw) {  // -1 未指定/认不得, 0 宽, 1 窄
    std::string k = AsciiUpper(NfkcLite(TrimAscii(raw)));
    std::string t;
    for (char c : k)
        if (c != ' ') t.push_back(c);
    if (t.empty() || t == "N/A") return -1;
    if (t == "25KHZ" || t == "25K" || t == "WIDE" || t == "宽" || t == "宽带") return 0;
    if (t == "12.5KHZ" || t == "12.5K" || t == "NARROW" || t == "窄" || t == "窄带") return 1;
    return -1;
}

bool BusyOn(const std::string& raw) {
    std::string s = AsciiUpper(TrimAscii(NfkcLite(raw)));
    return s == "开" || s == "是" || s == "ON" || s == "YES" || s == "TRUE" || s == "Y" || s == "1";
}

// 空 = 用默认(关); 认得的开/关都算规范; 其余算瞎填
bool BusyRecognized(const std::string& raw) {
    std::string s = AsciiUpper(TrimAscii(NfkcLite(raw)));
    if (s.empty()) return true;
    return s == "开" || s == "是" || s == "ON" || s == "YES" || s == "TRUE" || s == "Y" ||
           s == "1" || s == "关" || s == "否" || s == "OFF" || s == "NO" || s == "FALSE" ||
           s == "N" || s == "0";
}

struct DcsParsed {
    bool ok = false;
    std::string code;  // 3 位
    int suffix = 0;    // 0 无, 1 N(正向), 2 I(反向)
};

// 只接受 "023"/"23"/"D023"/"023N"/"D263I"; 故意不「抠出所有数字」
DcsParsed ParseDcs(const std::string& raw) {
    DcsParsed r;
    std::string s = AsciiUpper(TrimAscii(NfkcLite(raw)));
    if (s.empty()) return r;
    size_t i = 0;
    if (s[0] == 'D') i = 1;
    size_t j = i;
    while (j < s.size() && j - i < 3 && s[j] >= '0' && s[j] <= '9') ++j;
    if (j == i) return r;
    size_t k = j;
    int suf = 0;
    if (k < s.size() && (s[k] == 'N' || s[k] == 'I')) {
        suf = (s[k] == 'I') ? 2 : 1;
        ++k;
    }
    if (k != s.size()) return r;
    std::string code = s.substr(i, j - i);
    while (code.size() < 3) code.insert(code.begin(), '0');
    r.ok = true;
    r.code = code;
    r.suffix = suf;
    return r;
}

int DcsIndex(const std::string& code3) {
    for (int i = 0; i < DCS_COUNT; ++i)
        if (code3 == DCS_CODES[i]) return i + 1;
    return 0;
}

// ---------------------------------------------------------------- 信道
struct TonePlan {
    int kind = 0;   // 0 无, 1 模拟, 2 数字
    int value = 0;  // 实际存进 .dat 的整数 (模拟: Hz*10; 数字: 序号(+105))
};

struct TailPlan {
    int pwr = -1;  // -1 = 沿用基准
    int nar = -1;  // -1 = 沿用基准
    bool busy = false;
};

struct Channel {
    int row = 0;
    int no = 0;
    std::string name;
    double rx = 0, tx = 0;
    // 亚音原文 (解析用)
    std::string rType, tType, rCode, tCode;
    double rVal = 0, tVal = 0;
    bool hasRVal = false, hasTVal = false;
    std::string bw, pwrText, busyText, mode;
    // 解析结果
    TonePlan rTone, tTone;
    int pwr = -1;   // -1 = 表格没填, 沿用基准
    int nar = -1;   // -1 = 表格没填, 沿用基准
    bool busy = false;
};

struct Plan {
    double rx = 0, tx = 0;
    TonePlan rTone, tTone;
    TailPlan tail;
};

// ---------------------------------------------------------------- 读表 + 校验
// 一行里任何一项填得不规范, 整条信道都不转换, 并在日志里写明是哪一项
struct ReadOutcome {
    std::vector<Channel> chans;
    int skipped = 0;
    std::vector<std::string> problems;  // 已排好序的日志行
};

// 亚音输入 -> 编码决策 (优先级与 Python 版一致); 返回 false = 这一侧填得不完整
bool TonePlanFor(const Channel& ch, bool txSide, TonePlan& out, std::string& why, LogBuf& log) {
    const std::string& typ = txSide ? ch.tType : ch.rType;
    const std::string& codeRaw = txSide ? ch.tCode : ch.rCode;
    double hz = txSide ? ch.tVal : ch.rVal;
    bool hasVal = txSide ? ch.hasTVal : ch.hasRVal;
    const std::string which = txSide ? "发送" : "接收";

    out = TonePlan();
    why.clear();
    std::string t = TrimAscii(typ);
    std::string s = TrimAscii(codeRaw);
    DcsParsed d = ParseDcs(s);
    int idx = d.ok ? DcsIndex(d.code) : 0;
    bool hasDig = idx > 0;
    bool hasAna = hasVal && hz > 0;
    bool inverted = (d.suffix != 0) ? (d.suffix == 2) : (t == "反向数字亚音");

    auto analog = [&]() {
        // Python: round(hz,1) 再 *10 取整
        double hv = std::nearbyint(hz * 10.0) / 10.0;
        int v10 = (int)std::nearbyint(hv * 10.0);
        if (v10 <= TONE_DIGITAL_MAX)
            log.Warn("信道" + N(ch.no) + " " + which + " 模拟亚音 " + FmtG(hz) + "Hz 编码后为 " +
                     N(v10) + ", 落在数字亚音区间 (1.." + N(TONE_DIGITAL_MAX) +
                     ") —— D500 靠数值区间区分模拟/数字, 电台会当成 DCS 码");
        out.kind = 1;
        out.value = v10;
    };
    auto digital = [&]() {
        out.kind = 2;
        out.value = idx + (inverted ? DCS_INVERT_OFFSET : 0);
    };

    if (hasDig && d.suffix != 0) {  // 1. 后缀最硬
        if (!t.empty() && t != "数字亚音" && t != "反向数字亚音")
            log.Warn("信道" + N(ch.no) + " " + which + " 类型列 “" + t + "” 与码列 “" + s +
                     "” 的后缀不一致, 以后缀为准(" + (inverted ? "反向" : "正向") + ")");
        digital();
        return true;
    }
    if (hasDig && hasAna) {  // 2. 两列都填、码无后缀 -> 类型列说了算
        if (t == "数字亚音" || t == "反向数字亚音") {
            digital();
            return true;
        }
        if (t.empty())
            log.Warn("信道" + N(ch.no) + " " + which + " 数字亚音码 “" + s + "” 与模拟亚音 " +
                     FmtG(hz) + "Hz 都填了、类型列空且码无后缀, 按模拟亚音处理");
        analog();
        return true;
    }
    if (hasDig) {  // 3. 只有数字亚音码 -> 忽略类型列
        if (!t.empty() && t != "数字亚音" && t != "反向数字亚音")
            log.Warn("信道" + N(ch.no) + " " + which + " 类型列 “" + t + "” 但只填了数字亚音码 “" +
                     s + "”, 以数字亚音为准");
        digital();
        return true;
    }
    if (hasAna) {  // 4. 只有模拟亚音频率 -> 忽略类型列
        if (!t.empty() && t != "亚音频")
            log.Warn("信道" + N(ch.no) + " " + which + " 类型列 “" + t + "” 但只填了模拟亚音 " +
                     FmtG(hz) + "Hz, 以模拟亚音为准");
        analog();
        return true;
    }
    if (!t.empty()) {  // 5. 填了类型, 但既没有可用的码也没有可用的频率
        why = which + "亚音";
        return false;
    }
    return true;  // 什么都没填 = 不保留亚音
}

Plan ToPlan(const Channel& ch) {
    Plan p;
    p.rx = ch.rx;
    p.tx = ch.tx;
    p.rTone = ch.rTone;
    p.tTone = ch.tTone;
    p.tail.pwr = ch.pwr;
    p.tail.nar = ch.nar;
    p.tail.busy = ch.busy;
    return p;
}

// 逐行读表: 空行跳过; 有任何一项认不出来就整条跳过并记录原因
ReadOutcome ReadChannels(const Cells& cells, LogBuf& log) {
    ReadOutcome st;
    int maxRow = cells.MaxRow();
    for (int r = 2; r <= maxRow; ++r) {
        std::string rawNo = cells.Get(r, C_NO);
        std::string rawRx = cells.Get(r, C_RX);
        if (TrimAscii(rawNo).empty() && TrimAscii(rawRx).empty()) continue;  // 整行空白

        // 模板把 200 个信道号预先填好了: 只填了号、别的全空 -> 当成空行, 不当成填错
        if (TrimAscii(rawRx).empty()) {
            const int otherCols[] = {C_NAME, C_BW,  C_TX,   C_PWR,  C_RTYPE, C_RVAL,
                                     C_RCODE, C_TTYPE, C_TVAL, C_TCODE, C_BUSY, C_MODE};
            bool untouched = true;
            for (int c : otherCols) {
                if (!TrimAscii(cells.Get(r, c)).empty()) {
                    untouched = false;
                    break;
                }
            }
            if (untouched) continue;
        }

        std::vector<std::string> bad;
        Channel ch;
        ch.row = r;
        ch.name = cells.Get(r, C_NAME);

        // 信道号
        double noV = 0;
        bool noOk = ParseStrictNum(rawNo, noV);
        bool noInt = noOk && std::fabs(noV - std::nearbyint(noV)) < 1e-9;
        std::string label =
            (noOk && noInt) ? ("信道" + N((long long)std::nearbyint(noV))) : ("第" + N(r) + "行");
        if (!noOk)
            bad.push_back("信道号");
        else if (!noInt)
            bad.push_back("信道号(不是整数)");
        else if (std::fabs(noV) > 1e6)
            bad.push_back("信道号(超出范围)");
        else
            ch.no = (int)std::nearbyint(noV);

        // 收发频率
        if (TrimAscii(rawRx).empty())
            bad.push_back("接收频率(没填)");
        else if (!ParseStrictNum(rawRx, ch.rx))
            bad.push_back("接收频率");
        else if (!(ch.rx > 0.0 && ch.rx < 1000.0))
            bad.push_back("接收频率(超出范围)");

        std::string rawTx = cells.Get(r, C_TX);
        if (TrimAscii(rawTx).empty())
            ch.tx = ch.rx;  // 不填 = 直频
        else if (!ParseStrictNum(rawTx, ch.tx))
            bad.push_back("发送频率");
        else if (ch.tx == 0.0)
            ch.tx = ch.rx;  // 填 0 也按直频
        else if (!(ch.tx > 0.0 && ch.tx < 1000.0))
            bad.push_back("发送频率(超出范围)");

        // 亚音 (先只做语法检查, 语义在后面统一判)
        ch.rType = TrimAscii(cells.Get(r, C_RTYPE));
        ch.tType = TrimAscii(cells.Get(r, C_TTYPE));
        ch.rCode = TrimAscii(cells.Get(r, C_RCODE));
        ch.tCode = TrimAscii(cells.Get(r, C_TCODE));
        std::string rawRVal = cells.Get(r, C_RVAL);
        std::string rawTVal = cells.Get(r, C_TVAL);
        ch.hasRVal = ParseStrictNum(rawRVal, ch.rVal);
        ch.hasTVal = ParseStrictNum(rawTVal, ch.tVal);
        if (!ch.hasRVal && !TrimAscii(rawRVal).empty()) bad.push_back("接收亚音频率");
        if (!ch.hasTVal && !TrimAscii(rawTVal).empty()) bad.push_back("发送亚音频率");
        if (!ch.rCode.empty()) {
            DcsParsed d = ParseDcs(ch.rCode);
            if (!d.ok || DcsIndex(d.code) == 0) bad.push_back("接收数字亚音码");
        }
        if (!ch.tCode.empty()) {
            DcsParsed d = ParseDcs(ch.tCode);
            if (!d.ok || DcsIndex(d.code) == 0) bad.push_back("发送数字亚音码");
        }

        // 功率 / 带宽 / 繁忙禁发
        ch.pwrText = cells.Get(r, C_PWR);
        ch.pwr = PowerCode(ch.pwrText);
        if (ch.pwr < 0 && !TrimAscii(ch.pwrText).empty()) bad.push_back("发送功率");

        ch.bw = cells.Get(r, C_BW);
        ch.nar = BwNarrow(ch.bw);
        if (ch.nar < 0 && !TrimAscii(ch.bw).empty()) bad.push_back("带宽");

        ch.busyText = cells.Get(r, C_BUSY);
        if (!BusyRecognized(ch.busyText)) bad.push_back("繁忙禁发");
        ch.busy = BusyOn(ch.busyText);

        ch.mode = cells.Get(r, C_MODE);

        // 语法都没问题, 再判亚音语义 (避免给被跳过的信道刷警告)
        if (bad.empty()) {
            std::string why;
            if (!TonePlanFor(ch, false, ch.rTone, why, log)) bad.push_back(why);
            if (!TonePlanFor(ch, true, ch.tTone, why, log)) bad.push_back(why);
        }

        if (!bad.empty()) {
            ++st.skipped;
            for (size_t i = 0; i < bad.size(); ++i) {
                bool dup = false;
                for (size_t j = 0; j < i; ++j)
                    if (bad[j] == bad[i]) dup = true;
                if (!dup) st.problems.push_back(label + " " + bad[i] + "不规范，不进行转换");
            }
            continue;
        }
        st.chans.push_back(ch);
    }
    return st;
}

std::array<uint8_t, 16> BuildPayload(const Channel& ch, const Plan& plan, bool& ok,
                                     std::string& err) {
    std::array<uint8_t, 16> out{};
    ok = true;
    if (!EncFreq(plan.rx, out.data(), err) || !EncFreq(plan.tx, out.data() + 4, err)) {
        ok = false;
        return out;
    }
    EncToneValue(plan.rTone.value, out.data() + 8);
    EncToneValue(plan.tTone.value, out.data() + 10);

    uint8_t t[4] = {BASE_TAIL[0], BASE_TAIL[1], BASE_TAIL[2], BASE_TAIL[3]};
    if (plan.tail.pwr >= 0) t[2] = (uint8_t)plan.tail.pwr;
    if (plan.tail.nar >= 0)
        t[3] = (uint8_t)((t[3] & (uint8_t)~NARROW_BIT) | (plan.tail.nar ? NARROW_BIT : 0));
    t[3] = (uint8_t)((t[3] & (uint8_t)~BUSY_BIT) | (plan.tail.busy ? BUSY_BIT : 0));
    for (int i = 0; i < 4; ++i) out[12 + i] = t[i];
    (void)ch;
    return out;
}

const char* PowerName(int code) {
    switch (code) {
        case 0x00:
            return "高(H)";
        case 0x01:
            return "中(M)";
        case 0x02:
            return "低(L)";
        default:
            return "?";
    }
}

std::string PowerNameStr(int code) {
    if (code >= 0 && code <= 2) return PowerName(code);
    return "0x" + Hex2(code);
}

// .dat 读写与解码共用 d500dat.cpp 里的实现

std::vector<std::string> RoundtripCheck(const std::vector<Row>& rows,
                                        const std::map<int, Channel>& mapping,
                                        const std::map<int, Plan>& plans) {
    std::vector<std::string> bad;
    for (const auto& kv : mapping) {
        int slot = kv.first;
        const Channel& ch = kv.second;
        const Plan& plan = plans.at(slot);
        uint8_t p[16];
        for (int k = 0; k < 16; ++k) p[k] = (uint8_t)rows[(size_t)slot][(size_t)(3 + k)];

        auto note = [&](const std::string& field, const std::string& exp, const std::string& got) {
            bad.push_back("    槽 " + PadRight(N(slot), 4) + " 信道" + PadRight(N(ch.no), 4) + " “" +
                          ch.name + "” " + field + ": 预期 " + exp + " / 实际 " + got);
        };

        double rx = 0, tx = 0;
        std::string e1, e2;
        bool okRx = DecFreq(p, rx, e1);
        bool okTx = DecFreq(p + 4, tx, e2);
        if (!okRx || !okTx) {
            note("收发频率", "BCD 编码", okRx ? e2 : e1);
        } else {
            if (std::fabs(rx - plan.rx) > 1e-6)
                note("接收频率", FmtG(plan.rx) + " MHz", FmtG(rx) + " MHz");
            if (std::fabs(tx - plan.tx) > 1e-6)
                note("发送频率", FmtG(plan.tx) + " MHz", FmtG(tx) + " MHz");
        }
        int gotR = DecTone(p + 8), gotT = DecTone(p + 10);
        if (plan.rTone.value != gotR)
            note("接收亚音", ToneText(plan.rTone.value), ToneText(gotR));
        if (plan.tTone.value != gotT)
            note("发送亚音", ToneText(plan.tTone.value), ToneText(gotT));
        if (plan.tail.pwr >= 0 && p[14] != (uint8_t)plan.tail.pwr)
            note("发送功率", PowerNameStr(plan.tail.pwr), PowerNameStr(p[14]));
        bool gotNar = (p[15] & NARROW_BIT) != 0;
        if (plan.tail.nar >= 0 && gotNar != (plan.tail.nar != 0))
            note("带宽", plan.tail.nar ? "窄带" : "宽带", gotNar ? "窄带" : "宽带");
        bool gotBusy = (p[15] & BUSY_BIT) != 0;
        if (gotBusy != plan.tail.busy)
            note("繁忙禁发", plan.tail.busy ? "开" : "关", gotBusy ? "开" : "关");

        size_t used = 0;
        std::vector<uint8_t> want = EncName(ch.name, 16, used);
        bool same = true;
        for (int k = 0; k < 16; ++k)
            if (rows[(size_t)(NAME_BASE + slot)][(size_t)(3 + k)] != want[(size_t)k]) same = false;
        if (!same) {
            const int* g = rows[(size_t)(NAME_BASE + slot)].data() + 3;
            size_t gl = 0;
            std::vector<uint8_t> gb;
            while (gl < 16 && (uint8_t)g[gl] != EMPTY) {
                gb.push_back((uint8_t)g[gl]);
                ++gl;
            }
            note("信道名称", GbkToText(want.data(), used), GbkToText(gb.data(), gb.size()));
        }
    }
    return bad;
}

// ---------------------------------------------------------------- 基准
bool BuildRows(const uint8_t* blob, size_t len, std::vector<Row>& rows, std::string& err) {
    std::vector<int> keep;
    for (int i = 224; i < 256; ++i) keep.push_back(i);
    for (int i = 480; i < ROWS; ++i) keep.push_back(i);
    if (len != keep.size() * 16) {
        err = "内置基准数据损坏(长度不符: " + N(len) + " 字节, 应为 " + N(keep.size() * 16) + ")";
        return false;
    }
    rows.assign((size_t)ROWS, Row{});
    for (int i = 0; i < ROWS; ++i) {
        rows[(size_t)i][0] = i / 16;   // bank
        rows[(size_t)i][1] = i % 16;   // offset
        rows[(size_t)i][2] = i;        // linear (可到 1024, 不能截断)
        for (int k = 0; k < 16; ++k) rows[(size_t)i][(size_t)(3 + k)] = EMPTY;
    }
    for (size_t n = 0; n < keep.size(); ++n)
        for (int k = 0; k < 16; ++k) rows[(size_t)keep[n]][(size_t)(3 + k)] = blob[n * 16 + (size_t)k];
    return true;
}

// 取文件的修改时间 (FILETIME); 失败返回 false
bool FileMtime(const std::wstring& path, unsigned long long& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    ft = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
         (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::string HeaderCheckError(const Cells& cells) {
    std::string bad;
    for (const auto& rule : HEADER_RULES) {
        std::string got = cells.Get(1, rule.col);
        std::string up = AsciiUpper(got);
        std::string missing;
        bool miss = false;
        for (int i = 0; i < 3 && rule.keys[i]; ++i) {
            if (up.find(rule.keys[i]) == std::string::npos) {
                if (!missing.empty()) missing += ", ";
                missing += rule.keys[i];
                miss = true;
            }
        }
        if (miss) {
            bad += "  第 " + N(rule.col + 1) + " 列 应为含 “" + rule.keys[0] + "” 的列, 实际表头 “" +
                   got + "”";
            if (!missing.empty()) bad += " (缺 " + missing + ")";
            bad += "\n";
        }
    }
    return bad;
}

}  // namespace

// ---------------------------------------------------------------- 备份选择
// 一律按文件修改时间排序, 不再猜文件名里的日期
std::vector<BackupItem> ListBackups(const std::wstring& dir) {
    std::vector<BackupItem> out;
    WIN32_FIND_DATAW fd;
    std::wstring pat = Join(dir, L"K5Channel*.xlsx");
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.size() >= 2 && name[0] == L'~' && name[1] == L'$') continue;
        BackupItem it;
        it.path = Join(dir, name);
        it.mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                   (unsigned long long)fd.ftLastWriteTime.dwLowDateTime;
        it.date = YymmddFromFileTime(it.mtime);
        out.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const BackupItem& a, const BackupItem& b) {
        if (a.mtime != b.mtime) return a.mtime < b.mtime;
        return a.path < b.path;
    });
    return out;
}

bool PickBackup(const std::wstring& dir, const std::wstring& explicitPath, std::wstring& outPath,
                std::string& yymmdd, std::string& how, std::string& err) {
    if (!explicitPath.empty()) {
        std::wstring p = explicitPath;
        bool absolute = (!p.empty() && p[0] == L'\\') ||
                        (p.size() > 2 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'));
        if (!absolute) p = Join(dir, p);
        if (!FileExists(p)) {
            err = "备份文件不存在: " + WToU8(p);
            return false;
        }
        unsigned long long mt = 0;
        if (!FileMtime(p, mt) || mt == 0) {
            err = "读不到文件修改时间: " + WToU8(p);
            return false;
        }
        yymmdd = YymmddFromFileTime(mt);
        how = "手动选择 (输出日期取修改时间)";
        outPath = p;
        return true;
    }

    std::vector<BackupItem> list = ListBackups(dir);
    if (list.empty()) {
        err = WToU8(dir) + " 内没找到 K5Channel*.xlsx 备份";
        return false;
    }
    const BackupItem& best = list.back();  // 已按修改时间升序
    outPath = best.path;
    yymmdd = best.date;
    how = "修改时间最新的备份";
    return true;
}

// ---------------------------------------------------------------- 主流程
Result Convert(const uint8_t* baseBlob, size_t baseLen, const Options& opt) {
    Result res;
    LogBuf log;
    try {
        std::wstring here = ExeDir();
        std::wstring outDir = opt.outDir.empty() ? here : opt.outDir;
        if (!DirExists(outDir)) {
            log.Line("输出目录不存在: " + WToU8(outDir));
            res.log = log.s;
            return res;
        }

        std::wstring backup;
        std::string yymmdd, how, err;
        if (!PickBackup(here, opt.backupPath, backup, yymmdd, how, err)) {
            log.Line(err);
            res.log = log.s;
            return res;
        }
        log.Line("K1 备份 : " + WToU8(BaseName(backup)) + "   (" + how + ", 输出日期 " + yymmdd +
                 ")");

        // 大小上限: 满打满算的信道表格也就几十 KB, 超过 2 MB 一律当异常文件拒收
        unsigned long long fsz = 0;
        if (FileSizeBytes(backup, fsz) && fsz > kMaxInputBytes) {
            log.Line("文件过大: " + WToU8(BaseName(backup)) + " 有 " + N((long long)(fsz / 1024)) +
                     " KB, 超过上限 " + N((long long)(kMaxInputBytes / 1024)) +
                     " KB。正常的信道表格不会有这么大, 已拒绝读取。");
            res.log = log.s;
            res.tooLarge = true;
            return res;
        }

        Cells cells;
        if (!LoadSheet(backup, "Sheet1", cells, err)) {
            log.Line("读取 " + WToU8(BaseName(backup)) + " 失败: " + err);
            res.log = log.s;
            return res;
        }

        if (opt.headerCheck) {
            std::string bad = HeaderCheckError(cells);
            if (!bad.empty()) {
                log.Line(WToU8(BaseName(backup)) +
                         " 的表头与列号常量不符, 已停止(确认无误可关闭表头校验):");
                log.s += bad;
                res.log = log.s;
                return res;
            }
        }

        ReadOutcome rd = ReadChannels(cells, log);
        const std::vector<Channel>& chans = rd.chans;
        if (rd.skipped > 0) {
            log.Line("");
            log.Line("! 有 " + N(rd.skipped) + " 条信道填得不规范, 已跳过(不进行转换):");
            for (size_t i = 0; i < rd.problems.size() && i < 30; ++i)
                log.Line("   " + rd.problems[i]);
            if (rd.problems.size() > 30)
                log.Line("   … 另有 " + N((long long)rd.problems.size() - 30) + " 行同类提示");
        }
        if (chans.empty()) {
            log.Line(rd.skipped > 0
                         ? "备份里的信道全部填得不规范, 没有可转换的内容。"
                         : "备份里没有读到任何信道 —— 表格可能是空白的, 或第 1 行不是表头。");
            res.log = log.s;
            return res;
        }
        log.Line("读取信道: " + N(chans.size()) + " 条, 信道号 " + N(chans.front().no) + ".." +
                 N(chans.back().no) +
                 (rd.skipped > 0 ? ("   (另有 " + N(rd.skipped) + " 条不规范已跳过)") : ""));

        std::vector<Row> rows;
        if (!BuildRows(baseBlob, baseLen, rows, err)) {
            log.Line(err);
            res.log = log.s;
            return res;
        }
        log.Line("基准     : 内置基准 (无需外部文件)");
        log.Line("输出目录 : " + WToU8(outDir));
        log.Line("默认位域 : 00 00 00 04  (带宽/功率/繁忙禁发/亚音按 K1 表逐条覆盖)");

        // 清空信道槽 + 名称槽
        for (int i = 0; i < CH_COUNT; ++i) {
            for (int k = 0; k < 16; ++k) {
                rows[(size_t)i][(size_t)(3 + k)] = EMPTY;
                rows[(size_t)(NAME_BASE + i)][(size_t)(3 + k)] = EMPTY;
            }
        }

        // 信道号范围
        std::vector<int> outOfRange;
        for (const auto& ch : chans) {
            if (ch.no < K1_FIRST_CH || ch.no > K1_LAST_CH) {
                if (std::find(outOfRange.begin(), outOfRange.end(), ch.no) == outOfRange.end())
                    outOfRange.push_back(ch.no);
            }
        }
        if (!outOfRange.empty()) {
            std::sort(outOfRange.begin(), outOfRange.end());
            std::string list;
            for (size_t i = 0; i < outOfRange.size(); ++i) {
                if (i) list += ", ";
                list += N(outOfRange[i]);
            }
            log.Line("K1 表里有 " + N(outOfRange.size()) + " 个信道号超出 " + N(K1_FIRST_CH) + ".." +
                     N(K1_LAST_CH) + ": [" + list + "]");
            log.Line("  D500 只有 " + N(D500_CH_COUNT) + " 条信道(信道 1.." + N(D500_CH_COUNT) +
                     " = 文件槽 0.." + N(D500_CH_COUNT - 1) + "), 请先纠正表格后再生成。");
            res.log = log.s;
            return res;
        }

        std::map<int, Channel> mapping;
        std::vector<std::string> dups;
        for (const auto& ch : chans) {
            int slot = (ch.no == K1_LAST_CH) ? opt.slot0 : ch.no;
            if (slot < 0 || slot >= D500_CH_COUNT) {
                log.Line("信道 " + N(ch.no) + " 映射到槽 " + N(slot) + ", 超出 0.." +
                         N(D500_CH_COUNT - 1));
                res.log = log.s;
                return res;
            }
            auto it = mapping.find(slot);
            if (it != mapping.end()) {
                std::string why;
                if (it->second.no == ch.no) {
                    why = "信道 " + N(ch.no) + " 重复";
                } else {
                    why = "信道 " + N(ch.no) + " 与信道 " + N(it->second.no) + " 都落在槽 " +
                          N(slot) + " (第 200 信道映射槽 " + N(opt.slot0) + ")";
                }
                dups.push_back(why + ": 保留第 " + N(it->second.row) + " 行, 忽略第 " + N(ch.row) +
                               " 行");
                continue;
            }
            mapping[slot] = ch;
        }
        if (!dups.empty()) {
            log.Line("");
            log.Line("! 有 " + N(dups.size()) + " 行信道号重复 —— 以先出现的为准, 后面的不覆盖:");
            for (size_t i = 0; i < dups.size() && i < 20; ++i) log.Line("   " + dups[i]);
            if (dups.size() > 20) log.Line("   … 另有 " + N(dups.size() - 20) + " 行");
        }

        // 槽 0 / 槽 1 都为空的布局, 写频软件会一条都不显示
        if (mapping.find(SLOT_BLANK) == mapping.end() && mapping.find(1) == mapping.end()) {
            log.Line("");
            log.Line("!! 警告: 槽 " + N(SLOT_BLANK) + " 和槽 1 都没有信道(最低在 " +
                     N(mapping.begin()->first) + " 号槽)。");
            log.Line("   实测这种布局写频软件导入后会显示 0 条信道 —— 但文件仍按你的表格原样生成, "
                     "不自动搬移。");
        }

        // 逐条编码
        std::map<int, Plan> plans;
        for (auto& kv : mapping) {
            int slot = kv.first;
            const Channel& ch = kv.second;
            Plan plan = ToPlan(ch);
            std::string perr;
            bool ok = true;
            std::array<uint8_t, 16> payload = BuildPayload(ch, plan, ok, perr);
            if (!ok) {
                log.Line("信道 " + N(ch.no) + " “" + ch.name + "” 编码失败: " + perr);
                res.log = log.s;
                return res;
            }
            for (int k = 0; k < 16; ++k) rows[(size_t)slot][(size_t)(3 + k)] = payload[(size_t)k];

            size_t used = 0;
            std::vector<uint8_t> nb = EncName(ch.name, 16, used);
            for (int k = 0; k < 16; ++k)
                rows[(size_t)(NAME_BASE + slot)][(size_t)(3 + k)] = nb[(size_t)k];
            size_t total = 0;
            for (uint32_t cp : Utf8Decode(ch.name)) total += GbkChar(cp).size();
            if (total > used)
                log.Warn("信道" + N(ch.no) + " 名称 “" + ch.name + "” 超过 16 字节(8 个汉字), "
                         "已截断为 “" + GbkToText(nb.data(), used) + "”");
            plans[slot] = plan;
        }

        // 写盘 (目标 .dat 已存在时: 由界面问用户 替换 / 保留两者 / 取消)
        const std::wstring stem = L"D500_" + U8(yymmdd);
        std::wstring out = Join(outDir, stem + L".dat");
        if (FileExists(out)) {
            int choice = 2;  // 没提供回调时默认不覆盖
            if (opt.onConflict) choice = opt.onConflict(out);
            if (choice == 0) {
                log.Line("");
                log.Line("已取消：目标文件已存在（" + WToU8(out) + "），没有生成任何文件");
                res.log = log.s;
                res.code = 4;
                return res;
            }
            if (choice == 2) out = UniqueSuffixedPath(outDir, stem, L".dat");
        }
        std::string text = BuildDatText(rows);
        if (opt.keepDatText) res.datText = text;
        bool wrote = WriteAll(out, (const uint8_t*)text.data(), text.size());
        if (!wrote) log.Line("写入失败: " + WToU8(out) + " (目录是否可写?)");

        std::vector<int> slots;
        for (const auto& kv : mapping) slots.push_back(kv.first);

        // ---- 带宽 / 功率 摘要 ----
        std::vector<std::pair<std::string, int>> bwCount, pwCount;
        auto count = [](std::vector<std::pair<std::string, int>>& v, const std::string& k) {
            for (auto& p : v) {
                if (p.first == k) {
                    p.second++;
                    return;
                }
            }
            v.push_back(std::make_pair(k, 1));
        };
        for (int s : slots) {
            count(bwCount, (rows[(size_t)s][18] & NARROW_BIT) ? "窄带" : "宽带");
            count(pwCount, PowerNameStr(rows[(size_t)s][17]));
        }
        std::string bws, pws;
        for (size_t i = 0; i < bwCount.size(); ++i) {
            if (i) bws += ", ";
            bws += bwCount[i].first + " " + N(bwCount[i].second) + " 条";
        }
        for (size_t i = 0; i < pwCount.size(); ++i) {
            if (i) pws += ", ";
            pws += pwCount[i].first + " " + N(pwCount[i].second) + " 条";
        }
        log.Line("");
        log.Line("带宽     : " + bws);
        log.Line("发送功率 : " + pws);

        std::vector<int> odd;
        for (int s : slots) {
            bool same = true;
            for (int k = 0; k < 4; ++k)
                if (rows[(size_t)s][(size_t)(15 + k)] != BASE_TAIL[k]) same = false;
            if (!same) odd.push_back(s);
        }
        if (!odd.empty()) {
            log.Line("  与默认位域不同的信道:");
            size_t shown = 0;
            for (int s : odd) {
                if (shown++ >= 20) break;
                std::string tail;
                for (int k = 0; k < 4; ++k) {
                    if (k) tail += " ";
                    tail += Hex2(rows[(size_t)s][(size_t)(15 + k)]);
                }
                log.Line("    槽 " + PadRight(N(s), 4) + " " +
                         PadRight(mapping.at(s).name, 12) + " " +
                         ((rows[(size_t)s][18] & NARROW_BIT) ? "窄带" : "宽带") + " / 功率" +
                         PowerNameStr(rows[(size_t)s][17]) + "   tail=" + tail);
            }
            if (odd.size() > 20) log.Line("    … 另有 " + N(odd.size() - 20) + " 条");
        } else {
            log.Line("  (全部信道与模板默认一致)");
        }

        // ---- 信道映射 ----
        log.Line("");
        log.Line("信道映射 (K1 信道号 -> D500 槽):");
        size_t i = 0;
        while (i < slots.size()) {
            size_t j = i;
            while (j + 1 < slots.size() && slots[j + 1] == slots[j] + 1) ++j;
            std::string desc = (j > i) ? (N(slots[i]) + "-" + N(slots[j])) : N(slots[i]);
            log.Line("  槽 " + PadRight(desc, 9) + ": " + mapping.at(slots[i]).name + " … " +
                     mapping.at(slots[j]).name + "   (" + N(j - i + 1) + " 条)");
            i = j + 1;
        }
        auto s0it = mapping.find(opt.slot0);
        if (s0it != mapping.end() && s0it->second.no == K1_LAST_CH) {
            log.Line("  注: K1 第 " + N(K1_LAST_CH) + " 信道 “" + s0it->second.name +
                     "” 已映射到 D500 " + N(opt.slot0) + " 号槽");
        } else if (s0it == mapping.end()) {
            log.Line("  注: K1 第 " + N(K1_LAST_CH) + " 信道无内容, D500 " + N(opt.slot0) +
                     " 号槽是空信道");
        } else {
            log.Line("  注: K1 第 " + N(K1_LAST_CH) + " 信道无内容; D500 " + N(opt.slot0) +
                     " 号槽由 信道" + N(s0it->second.no) + " “" + s0it->second.name + "” 占用");
        }

        log.Line("");
        log.Line("信道区   : 槽 0.." + N(D500_CH_COUNT - 1) + " 共 " + N(D500_CH_COUNT) +
                 " 条 (= D500 信道 1.." + N(D500_CH_COUNT) + ")");
        log.Line("           有内容 " + N(slots.size()) + " 条, 空信道 " +
                 N(D500_CH_COUNT - slots.size()) + " 条  (xlsx 里没有的信道 = 空信道, 槽保持 0xFF)");

        // ---- 写入后自检 ----
        std::vector<uint8_t> back;
        std::vector<Row> chk;
        bool okRows = false;
        std::string rerr;
        bool diskOk = wrote && ReadAll(out, back);
        if (!diskOk) back.assign(text.begin(), text.end());  // 写盘失败时用内存副本继续自检
        if (ParseDat(back, chk, rerr)) {
            okRows = ((int)chk.size() == ROWS);
        } else {
            log.Line("回读失败: " + rerr);
        }
        std::vector<int> usedSlots;
        bool zoneOk = false;
        if (okRows) {
            for (int s = 0; s < D500_CH_COUNT; ++s) {
                for (int k = 0; k < 16; ++k)
                    if (chk[(size_t)s][(size_t)(3 + k)] != EMPTY) {
                        usedSlots.push_back(s);
                        break;
                    }
            }
            zoneOk = true;
            for (int s = D500_CH_COUNT; s < CH_COUNT && zoneOk; ++s)
                for (int k = 0; k < 16; ++k)
                    if (chk[(size_t)s][(size_t)(3 + k)] != EMPTY) {
                        zoneOk = false;
                        break;
                    }
        }
        bool ok0 = false;
        std::string s0Desc;
        if (okRows) {
            bool any = false;
            for (int k = 0; k < 16; ++k)
                if (chk[(size_t)opt.slot0][(size_t)(3 + k)] != EMPTY) any = true;
            if (s0it != mapping.end()) {
                s0Desc = "已写入 “" + s0it->second.name + "”";
                ok0 = any;
            } else {
                s0Desc = "空信道";
                ok0 = !any;
            }
        }
        std::vector<std::string> badRt;
        if (okRows) badRt = RoundtripCheck(chk, mapping, plans);

        log.Line("");
        log.Line("已生成   : " + WToU8(out));
        log.Line("自检     : 行数 " + N(chk.size()) + (okRows ? " OK" : " 异常") + " | 信道区 0.." +
                 N(D500_CH_COUNT - 1) + " 已写 " + N(usedSlots.size()) + " 条 | 槽 " +
                 N(D500_CH_COUNT) + ".." + N(CH_COUNT - 1) + " " +
                 (zoneOk ? "空 OK" : "异常(非信道区有数据)") + " | " + N(opt.slot0) + " 号槽 " +
                 s0Desc);
        if (!badRt.empty()) {
            log.Line("  往返自检 : 失败 " + N(badRt.size()) + " 处 <<<");
            for (size_t k = 0; k < badRt.size() && k < 40; ++k) log.Line(badRt[k]);
            if (badRt.size() > 40) log.Line("    … 另有 " + N(badRt.size() - 40) + " 处");
        } else {
            log.Line("  往返自检 : " + N(slots.size()) +
                     " 条信道的 收发频率·亚音·功率·带宽·繁忙禁发·名称 全部一致");
        }

        res.log = log.s;
        res.outPath = out;
        if (!wrote) {
            res.code = 1;
            return res;
        }
        if (!okRows || usedSlots != slots || !ok0 || !zoneOk) {
            res.code = 2;
            return res;
        }
        res.code = badRt.empty() ? 0 : 3;
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
