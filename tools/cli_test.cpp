// cli_test.cpp —— 命令行测试壳 (开发期用, 不随成品发布)
//
//   cli_test.exe <base.bin> <out-dir> [xlsx]          正向: 生成 .dat 文本到 stdout
//   cli_test.exe --rev <tpl.xlsx> <dat> <out-dir>     反向: 生成 xlsx
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/core.h"
#include "../src/reverse.h"
#include "../src/util.h"

int main() {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) {
        std::fprintf(stderr, "usage: cli_test <base.bin> <out-dir> [xlsx]\n");
        std::fprintf(stderr, "       cli_test --rev <tpl.xlsx> <dat> <out-dir>\n");
        return 64;
    }

    if (std::wstring(argv[1]) == L"--rev") {
        if (argc < 5) {
            std::fprintf(stderr, "usage: cli_test --rev <tpl.xlsx> <dat> <out-dir>\n");
            return 64;
        }
        std::vector<uint8_t> tpl;
        if (!k1d500::ReadAll(argv[2], tpl)) {
            std::fprintf(stderr, "cannot read template\n");
            return 65;
        }
        k1d500::ReverseResult r =
            k1d500::ConvertDatToK1(tpl.data(), tpl.size(), argv[3], k1d500::DefaultReverseOutputPath(argv[4]));
        std::fwrite(r.log.data(), 1, r.log.size(), stderr);
        std::fprintf(stderr, "EXITCODE=%d\n", r.code);
        if (!r.outPath.empty())
            std::fprintf(stderr, "OUTFILE=%s\n", k1d500::WToU8(r.outPath).c_str());
        LocalFree(argv);
        return 0;
    }

    if (argc < 3) {
        std::fprintf(stderr, "usage: cli_test <base.bin> <out-dir> [xlsx]\n");
        LocalFree(argv);
        return 64;
    }
    std::vector<uint8_t> base;
    if (!k1d500::ReadAll(argv[1], base)) {
        std::fprintf(stderr, "cannot read base\n");
        return 65;
    }
    k1d500::Options opt;
    opt.outDir = argv[2];
    if (argc > 3) opt.backupPath = argv[3];
    opt.keepDatText = true;

    k1d500::Result r = k1d500::Convert(base.data(), base.size(), opt);
    if (!r.datText.empty()) std::fwrite(r.datText.data(), 1, r.datText.size(), stdout);
    std::fwrite(r.log.data(), 1, r.log.size(), stderr);
    std::fprintf(stderr, "EXITCODE=%d\n", r.code);
    if (!r.outPath.empty())
        std::fprintf(stderr, "OUTFILE=%s\n", k1d500::WToU8(r.outPath).c_str());
    LocalFree(argv);
    return 0;
}
