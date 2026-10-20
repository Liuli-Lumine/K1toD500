// inflate.cpp —— RFC 1951 (DEFLATE) 解码器。
// 结构参照 zlib 的 puff.c 思路自行实现: 位流 LSB 优先 + 规范霍夫曼码。
#include "inflate.h"

#include <cstring>

namespace k1d500 {
namespace {

const int MAXBITS = 15;

struct BitIn {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint64_t buf = 0;
    int cnt = 0;

    BitIn(const uint8_t* d, size_t len) : p(d), n(len) {}

    // 取 need 位; 输入耗尽返回 -1
    int Need(int need) {
        uint64_t v = buf;
        while (cnt < need) {
            if (pos >= n) return -1;
            v |= (uint64_t)p[pos++] << cnt;
            cnt += 8;
        }
        buf = v >> need;
        cnt -= need;
        return (int)(v & ((1ull << need) - 1));
    }

    // 丢弃不足一字节的残余位 (stored 块前对齐)
    void Align() {
        int drop = cnt & 7;
        buf >>= drop;
        cnt -= drop;
    }
};

struct Huff {
    short count[MAXBITS + 1];
    std::vector<short> symbol;
};

void BuildHuff(Huff& h, const short* length, int n) {
    for (int i = 0; i <= MAXBITS; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    h.count[0] = 0;

    short offs[MAXBITS + 2];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; ++len) offs[len + 1] = (short)(offs[len] + h.count[len]);

    h.symbol.assign((size_t)n, 0);
    for (int i = 0; i < n; ++i)
        if (length[i]) h.symbol[(size_t)offs[length[i]]++] = (short)i;
}

// 返回符号, 失败 -1
int DecodeSym(BitIn& br, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; ++len) {
        int b = br.Need(1);
        if (b < 0) return -1;
        code |= b;
        int count = h.count[len];
        if (code - first < count) return h.symbol[(size_t)(index + (code - first))];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

const short LEN_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DIST_BASE[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                             33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                             1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const short DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3,  3,  4,  4,  5,  5,  6,
                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void FixedTables(Huff& lit, Huff& dist) {
    short lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    BuildHuff(lit, lengths, 288);

    short dl[30];
    for (int i = 0; i < 30; ++i) dl[i] = 5;
    BuildHuff(dist, dl, 30);
}

int ReadDynamicTables(BitIn& br, Huff& lit, Huff& dist) {
    static const short ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
    int hlit = br.Need(5);
    int hdist = br.Need(5);
    int hclen = br.Need(4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    short cl[19];
    for (int i = 0; i < 19; ++i) cl[i] = 0;
    for (int i = 0; i < hclen; ++i) {
        int v = br.Need(3);
        if (v < 0) return -1;
        cl[ORDER[i]] = (short)v;
    }
    Huff clh;
    BuildHuff(clh, cl, 19);

    std::vector<short> lengths((size_t)(hlit + hdist), 0);
    int idx = 0;
    while (idx < hlit + hdist) {
        int sym = DecodeSym(br, clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[(size_t)idx++] = (short)sym;
            continue;
        }
        int len = 0, rep = 0;
        if (sym == 16) {
            if (idx == 0) return -1;
            len = lengths[(size_t)idx - 1];
            int e = br.Need(2);
            if (e < 0) return -1;
            rep = 3 + e;
        } else if (sym == 17) {
            int e = br.Need(3);
            if (e < 0) return -1;
            rep = 3 + e;
        } else {
            int e = br.Need(7);
            if (e < 0) return -1;
            rep = 11 + e;
        }
        if (idx + rep > hlit + hdist) return -1;
        while (rep-- > 0) lengths[(size_t)idx++] = (short)len;
    }
    if (lengths[256] == 0) return -1;  // 缺块结束符

    BuildHuff(lit, lengths.data(), hlit);
    BuildHuff(dist, lengths.data() + hlit, hdist);
    return 0;
}

bool InflateBlocks(BitIn& br, std::vector<uint8_t>& out, std::string& err) {
    int last = 0;
    do {
        last = br.Need(1);
        if (last < 0) {
            err = "DEFLATE 位流意外结束";
            return false;
        }
        int type = br.Need(2);
        if (type < 0) {
            err = "DEFLATE 位流意外结束";
            return false;
        }
        if (type == 0) {
            br.Align();
            int n = br.Need(16);
            int nn = br.Need(16);
            if (n < 0 || nn < 0) {
                err = "stored 块头缺失";
                return false;
            }
            if ((uint16_t)(~n & 0xFFFF) != (uint16_t)nn) {
                err = "stored 块长度校验失败";
                return false;
            }
            for (int i = 0; i < n; ++i) {
                int b = br.Need(8);
                if (b < 0) {
                    err = "stored 块数据截断";
                    return false;
                }
                out.push_back((uint8_t)b);
            }
            continue;
        }
        Huff lit, dist;
        if (type == 1) {
            FixedTables(lit, dist);
        } else if (type == 2) {
            if (ReadDynamicTables(br, lit, dist) != 0) {
                err = "动态霍夫曼表损坏";
                return false;
            }
        } else {
            err = "非法的块类型";
            return false;
        }

        for (;;) {
            int sym = DecodeSym(br, lit);
            if (sym < 0) {
                err = "霍夫曼解码失败";
                return false;
            }
            if (sym < 256) {
                out.push_back((uint8_t)sym);
                continue;
            }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) {
                err = "长度码越界";
                return false;
            }
            int e = br.Need(LEN_EXTRA[sym]);
            if (e < 0) {
                err = "长度附加位缺失";
                return false;
            }
            int len = LEN_BASE[sym] + e;
            int dsym = DecodeSym(br, dist);
            if (dsym < 0 || dsym >= 30) {
                err = "距离码非法";
                return false;
            }
            int de = br.Need(DIST_EXTRA[dsym]);
            if (de < 0) {
                err = "距离附加位缺失";
                return false;
            }
            size_t back = (size_t)(DIST_BASE[dsym] + de);
            if (back > out.size()) {
                err = "回溯距离超出已解压数据";
                return false;
            }
            size_t from = out.size() - back;
            for (int k = 0; k < len; ++k) out.push_back(out[from + (size_t)k]);
        }
    } while (!last);
    return true;
}

}  // namespace

bool InflateRaw(const uint8_t* in, size_t inLen, size_t expectedSize,
                std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    if (!in || inLen == 0) {
        err = "空的压缩数据";
        return false;
    }
    BitIn br(in, inLen);
    if (expectedSize > 0) out.reserve(expectedSize);
    if (!InflateBlocks(br, out, err)) return false;
    if (expectedSize > 0 && out.size() != expectedSize) {
        err = "解压长度不符";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace k1d500
