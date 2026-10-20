// gui.cpp —— 纯 Win32 界面 (无 MFC/Qt/运行库依赖)
//
// 界面:
//   [转 换] [选择文件…] [生成空白文件] [打开目录]
//    默认转换最新版表格          <- 选择文件后变成 “将要转换 <文件名>”
//   +--------------------------------------------------+
//   | 日志                                             |
//   +--------------------------------------------------+
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "core.h"
#include "res_ids.h"
#include "reverse.h"
#include "util.h"

#ifndef MSGFLT_ADD
#define MSGFLT_ADD 1
#endif

namespace {

enum : int {
    IDC_CONVERT = 1001,
    IDC_PICK = 1002,
    IDC_BLANK = 1003,
    IDC_OPEN = 1004,
    IDC_HINT = 1005,
    IDC_LOG = 1006,
    IDC_STATUS = 1007,
    IDC_TITLE = 1008,
    IDC_SUB = 1009,
    IDC_REVERSE = 1010,
};

HWND g_hMain = nullptr;
HWND g_hTitle = nullptr, g_hSub = nullptr;
HWND g_hConvert = nullptr, g_hPick = nullptr, g_hBlank = nullptr, g_hOpen = nullptr;
HWND g_hRev = nullptr;
HWND g_hHint = nullptr, g_hLog = nullptr, g_hStatus = nullptr;
HWND g_hTip = nullptr;

HFONT g_fUI = nullptr, g_fTitle = nullptr, g_fSub = nullptr, g_fConvert = nullptr, g_fLog = nullptr,
      g_fStatus = nullptr;
HBRUSH g_brBg = nullptr;

int g_dpi = 96;
std::wstring g_selected;          // 用户选的文件 (空 = 默认最新版)
std::wstring g_pendingSelect;     // 命令行(把 xlsx 拖到 exe 上)预选的文件
std::vector<uint8_t> g_base;      // 内置基准
std::vector<uint8_t> g_blankXlsx; // 内置 K5Channel.xlsx
bool g_busy = false;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// 布局需要的最小客户区 (与 Layout 里的常量对应), 高分屏下按 DPI 放大
int MinClientWidth() {
    return S(2 * 16 + 150 + 10 + 116 + 10 + 126 + 10 + 104 + 44 + 140);
}
int MinClientHeight() { return S(16 + 30 + 2 + 18 + 12 + 44 + 6 + 20 + 10 + 60 + 22 + 6 + 14); }

int GetDpi(HWND hwnd) {
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn =
        (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d >= 72) return (int)d;
    }
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d >= 72 ? d : 96;
}

void EnableDpiAwareness() {
    typedef BOOL(WINAPI * PFN_SetCtx)(int);
    static PFN_SetCtx fn = (PFN_SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                     "SetProcessDpiAwarenessContext");
    if (fn) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (fn(-4)) return;
    }
    SetProcessDPIAware();
}

HFONT MakeFont(int pt, bool bold, const wchar_t* face, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

void DestroyFonts() {
    for (HFONT* f : {&g_fUI, &g_fTitle, &g_fSub, &g_fConvert, &g_fLog, &g_fStatus}) {
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
    }
}

void CreateFonts() {
    DestroyFonts();
    g_fUI = MakeFont(9, false, L"Microsoft YaHei UI", g_dpi);
    g_fTitle = MakeFont(15, true, L"Microsoft YaHei UI", g_dpi);
    g_fSub = MakeFont(9, false, L"Microsoft YaHei UI", g_dpi);
    g_fConvert = MakeFont(12, true, L"Microsoft YaHei UI", g_dpi);
    g_fLog = MakeFont(9, false, L"Microsoft YaHei UI", g_dpi);
    g_fStatus = MakeFont(9, false, L"Microsoft YaHei UI", g_dpi);
    for (HWND h : {g_hTitle, g_hSub, g_hHint, g_hPick, g_hBlank, g_hOpen, g_hRev, g_hStatus})
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    if (g_hTitle) SendMessageW(g_hTitle, WM_SETFONT, (WPARAM)g_fTitle, TRUE);
    if (g_hSub) SendMessageW(g_hSub, WM_SETFONT, (WPARAM)g_fSub, TRUE);
    if (g_hConvert) SendMessageW(g_hConvert, WM_SETFONT, (WPARAM)g_fConvert, TRUE);
    if (g_hLog) SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fLog, TRUE);
    if (g_hHint) SendMessageW(g_hHint, WM_SETFONT, (WPARAM)g_fSub, TRUE);
    if (g_hStatus) SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_fStatus, TRUE);
}

void SetHint(const std::wstring& text) {
    if (g_hHint) SetWindowTextW(g_hHint, text.c_str());
}

void SetStatus(const std::wstring& text) {
    if (g_hStatus) SetWindowTextW(g_hStatus, text.c_str());
}

void Layout(int w, int h) {
    if (!g_hMain) return;
    const int m = S(16);
    const int bw = S(150), bh = S(44), gap = S(10);
    int y = m;

    int titleH = S(30);
    MoveWindow(g_hTitle, m, y, w - 2 * m, titleH, TRUE);
    y += titleH + S(2);
    int subH = S(18);
    MoveWindow(g_hSub, m, y, w - 2 * m, subH, TRUE);
    y += subH + S(12);

    int rowH = bh;
    int x = m;
    MoveWindow(g_hConvert, x, y, bw, rowH, TRUE);
    x += bw + gap;
    MoveWindow(g_hPick, x, y, S(116), rowH, TRUE);
    x += S(116) + gap;
    MoveWindow(g_hBlank, x, y, S(126), rowH, TRUE);
    x += S(126) + gap;
    MoveWindow(g_hOpen, x, y, S(104), rowH, TRUE);
    x += S(104) + S(44);  // 次要功能, 间隔留大一些
    MoveWindow(g_hRev, x, y, S(140), rowH, TRUE);
    y += rowH + S(6);

    int hintH = S(20);
    MoveWindow(g_hHint, m + S(4), y, w - 2 * m - S(4), hintH, TRUE);
    y += hintH + S(10);

    int statusH = S(22);
    int logH = h - y - statusH - S(14);
    if (logH < S(60)) logH = S(60);
    MoveWindow(g_hLog, m, y, w - 2 * m, logH, TRUE);
    MoveWindow(g_hStatus, m, h - statusH - S(6), w - 2 * m, statusH, TRUE);
}

// EDIT 控件只认 CRLF; core 的日志用 LF, 这里统一转换
std::wstring ToCrlf(const std::wstring& text) {
    std::wstring s;
    s.reserve(text.size() + 16);
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') s += L"\r\n";
        else s.push_back(c);
    }
    return s;
}

void AppendLog(const std::wstring& text) {
    if (!g_hLog) return;
    std::wstring s = ToCrlf(text);
    s += L"\r\n";
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

void AppendLogUtf8(const std::string& utf8) {
    if (utf8.empty()) return;
    std::wstring w = k1d500::U8(utf8);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    AppendLog(w);
}

void SetBusy(bool busy) {
    g_busy = busy;
    for (HWND h : {g_hConvert, g_hPick, g_hBlank, g_hOpen, g_hRev})
        if (h) EnableWindow(h, !busy);
    HCURSOR c = LoadCursorW(nullptr, busy ? (LPCWSTR)IDC_WAIT : (LPCWSTR)IDC_ARROW);
    SetCursor(c);
}

void OpenFolderSelect(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(g_hMain, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenFolder(const std::wstring& dir) {
    ShellExecuteW(g_hMain, L"open", L"explorer.exe", (L"\"" + dir + L"\"").c_str(), nullptr,
                  SW_SHOWNORMAL);
}

std::wstring DefaultBackupName(const std::wstring& dir) {
    std::wstring path, yymmdd, how;
    std::string y, ho, err;
    if (!k1d500::PickBackup(dir, L"", path, y, ho, err)) return std::wstring();
    return k1d500::BaseName(path);
}

enum class ConflictChoice { Cancel, Replace, KeepBoth };
ConflictChoice AskFileConflict(const std::wstring& path);

void DoConvert() {
    if (g_busy) return;
    SetBusy(true);
    SetStatus(L"转换中…");
    AppendLog(L"");

    k1d500::Options opt;
    opt.outDir = k1d500::ExeDir();
    opt.backupPath = g_selected;
    // 目标 .dat 已存在时问用户: 替换 / 保留两者 / 取消
    opt.onConflict = [](const std::wstring& existing) -> int {
        switch (AskFileConflict(existing)) {
            case ConflictChoice::Replace:
                return 1;
            case ConflictChoice::KeepBoth:
                return 2;
            default:
                return 0;
        }
    };
    k1d500::Result r = k1d500::Convert(g_base.data(), g_base.size(), opt);
    AppendLogUtf8(r.log);

    SetBusy(false);
    InvalidateRect(g_hMain, nullptr, TRUE);

    if (r.code == 0) {
        SetStatus(L"完成");
        std::wstring msg = L"已生成：\r\n" + r.outPath + L"\r\n\r\n现在打开所在文件夹吗？";
        if (MessageBoxW(g_hMain, msg.c_str(), L"转换成功", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            OpenFolderSelect(r.outPath);
    } else if (r.code == 4) {
        SetStatus(L"已取消");
        // 用户刚在重名对话框里选了取消, 不再弹窗
    } else if (r.tooLarge) {
        SetStatus(L"文件过大");
        MessageBoxW(g_hMain,
                    L"选中的文件超过 2 MB，已拒绝转换。\r\n\r\n"
                    L"正常的信道表格就算 200 条全部填满也只有几十 KB，\r\n"
                    L"请确认是不是选错了文件。",
                    L"文件过大", MB_OK | MB_ICONWARNING);
    } else {
        SetStatus(r.code == 3 ? L"往返自检失败" : (r.code == 2 ? L"结构自检失败" : L"转换失败"));
        std::wstring title = (r.code == 3 || r.code == 2) ? L"自检未通过" : L"转换失败";
        std::wstring body;
        if (r.code == 3)
            body = L"文件已生成，但往返自检发现不一致：\r\n" + r.outPath +
                   L"\r\n\r\n详情见下方日志（请把日志发给我）。";
        else if (r.code == 2)
            body = L"文件已生成，但结构自检失败：\r\n" + r.outPath + L"\r\n\r\n详情见下方日志。";
        else
            body = L"未能生成文件，原因见下方日志。";
        MessageBoxW(g_hMain, body.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
}

void DoPick() {
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"K1 信道备份 (*.xlsx)\0*.xlsx\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    std::wstring dir = k1d500::ExeDir();
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"选择要转换的 K1 信道表格";
    if (!GetOpenFileNameW(&ofn)) return;

    g_selected = file;
    SetHint(L"将要转换 " + k1d500::BaseName(g_selected));
    SetStatus(L"已选择文件");
    AppendLog(L"");
    AppendLog(L"已选择: " + g_selected);
}

void ResetSelection() {
    if (g_selected.empty()) return;
    g_selected.clear();
    SetHint(L"默认转换最新版表格");
    SetStatus(L"已恢复默认（最新版表格）");
    AppendLog(L"已恢复默认：自动选择日期最新的备份");
}

void DoBlank() {
    if (g_busy) return;
    if (g_blankXlsx.empty()) {
        MessageBoxW(g_hMain, L"内置的空白表格数据缺失，无法生成。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring dir = k1d500::ExeDir();
    std::wstring stem = L"K5Channel" + k1d500::U8(k1d500::TodayYymmdd());
    std::wstring out = k1d500::UniqueSuffixedPath(dir, stem, L".xlsx");
    if (!k1d500::WriteAll(out, g_blankXlsx.data(), g_blankXlsx.size())) {
        MessageBoxW(g_hMain, (L"写入失败：\r\n" + out +
                              L"\r\n\r\n请确认程序所在目录可写（不要放在 C:\\Program Files 下）。")
                                 .c_str(),
                    L"生成空白文件失败", MB_OK | MB_ICONERROR);
        return;
    }
    AppendLog(L"");
    AppendLog(L"已生成空白表格: " + out);
    SetStatus(L"已生成空白文件");
    std::wstring msg = L"已生成空白信道表格：\r\n" + out +
                       L"\r\n\r\n用泉盛 K1 写频软件打开它、填好信道（或直接从电台导出备份）后，"
                       L"回到本程序点【转换】即可。\r\n\r\n现在打开所在文件夹吗？";
    if (MessageBoxW(g_hMain, msg.c_str(), L"生成空白文件", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        OpenFolderSelect(out);
}

void DoOpenDir() { OpenFolder(k1d500::ExeDir()); }

// 目标文件重名: 黄色三角警告 + 三个按钮 (替换 / 保留两者 / 取消)
ConflictChoice AskFileConflict(const std::wstring& path) {
    typedef HRESULT(WINAPI * PFN_TDI)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    static PFN_TDI fn =
        (PFN_TDI)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    if (fn) {
        std::wstring content = L"已存在同名文件：\r\n" + path +
                               L"\r\n\r\n「保留两者」会另存为带 (1)、(2)… 的新名字。";
        TASKDIALOG_BUTTON btns[2] = {{1001, L"替换"}, {1002, L"保留两者"}};
        TASKDIALOGCONFIG tc{};
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_hMain;
        tc.hInstance = GetModuleHandleW(nullptr);
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = L"文件已存在";
        tc.pszMainIcon = TD_WARNING_ICON;  // 黄色三角感叹号
        tc.pszMainInstruction = L"目标文件已存在";
        tc.pszContent = content.c_str();
        tc.pButtons = btns;
        tc.cButtons = 2;
        tc.nDefaultButton = 1002;  // 默认「保留两者」，误按回车也不会覆盖
        int pressed = IDCANCEL;
        if (SUCCEEDED(fn(&tc, &pressed, nullptr, nullptr))) {
            if (pressed == 1001) return ConflictChoice::Replace;
            if (pressed == 1002) return ConflictChoice::KeepBoth;
            return ConflictChoice::Cancel;
        }
    }
    // 万一拿不到 TaskDialog (没有 comctl32 v6)，退回普通消息框
    std::wstring msg = L"目标文件已存在：\r\n" + path +
                       L"\r\n\r\n是 = 替换\r\n否 = 保留两者\r\n取消 = 不转换";
    int r = MessageBoxW(g_hMain, msg.c_str(), L"文件已存在", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDYES) return ConflictChoice::Replace;
    if (r == IDNO) return ConflictChoice::KeepBoth;
    return ConflictChoice::Cancel;
}

// D500 .dat -> 泉盛信道表格：默认取程序目录里修改时间最新的 .dat
void DoReverse() {
    if (g_busy) return;
    std::wstring dir = k1d500::ExeDir();
    auto dats = k1d500::ListDatFiles(dir);

    AppendLog(L"");
    if (dats.empty()) {
        AppendLog(L"反向转换 : 程序目录里没有 .dat 文件");
        SetStatus(L"没有 .dat 文件");
        MessageBoxW(g_hMain,
                    L"程序目录里没有找到 .dat 文件。\r\n\r\n"
                    L"请先把 D500 写频软件导出的 .dat 放到这个目录，再点一次。",
                    L"没有可转换的文件", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_blankXlsx.empty()) {
        MessageBoxW(g_hMain, L"内置的空白表格数据缺失，无法生成。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    const k1d500::DatFile& newest = dats.front();
    AppendLog(L"反向转换 : " + k1d500::BaseName(newest.path) + L"（修改于 " + newest.modified +
              L"，最新的 .dat）");

    // 转之前先让用户确认一次: 到底转的是哪个文件
    std::wstring confirm = L"将要转换下面这个文件：\r\n\r\n" + newest.path +
                           L"\r\n\r\n（修改于 " + newest.modified +
                           L"；这是程序目录里修改时间最新的 .dat）\r\n\r\n"
                           L"转换后会在同一目录生成 K5Channel<日期>.xlsx。\r\n\r\n确定继续吗？";
    if (MessageBoxW(g_hMain, confirm.c_str(), L"确认反向转换",
                    MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) != IDYES) {
        AppendLog(L"反向转换 : 已取消，未生成文件");
        SetStatus(L"已取消");
        return;
    }

    // 目标文件重名: 三个选项 (取消 / 替换 / 保留两者)
    std::wstring outPath = k1d500::DefaultReverseOutputPath(dir);
    if (k1d500::FileExists(outPath)) {
        ConflictChoice choice = AskFileConflict(outPath);
        if (choice == ConflictChoice::Cancel) {
            AppendLog(L"反向转换 : 目标文件已存在，已取消");
            SetStatus(L"已取消");
            return;
        }
        if (choice == ConflictChoice::KeepBoth) {
            std::wstring stem = L"K5Channel" + k1d500::U8(k1d500::TodayYymmdd());
            outPath = k1d500::UniqueSuffixedPath(dir, stem, L".xlsx");
        }
    }

    SetBusy(true);
    SetStatus(L"反向转换中…");
    k1d500::ReverseResult r =
        k1d500::ConvertDatToK1(g_blankXlsx.data(), g_blankXlsx.size(), newest.path, outPath);
    AppendLogUtf8(r.log);
    SetBusy(false);

    if (r.code == 0) {
        SetStatus(L"完成");
        std::wstring msg =
            L"已生成泉盛信道表格：\r\n" + r.outPath + L"\r\n\r\n现在打开所在文件夹吗？";
        if (MessageBoxW(g_hMain, msg.c_str(), L"转换成功", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            OpenFolderSelect(r.outPath);
    } else if (r.tooLarge) {
        SetStatus(L"文件过大");
        MessageBoxW(g_hMain,
                    L"选中的 .dat 超过 2 MB，已拒绝读取。\r\n\r\n"
                    L"D500 的写频文件只有一百多 KB，请确认是不是选错了文件。",
                    L"文件过大", MB_OK | MB_ICONWARNING);
    } else {
        SetStatus(L"反向转换失败");
        MessageBoxW(g_hMain, L"没有转换成功，原因见下方日志。", L"转换失败", MB_OK | MB_ICONWARNING);
    }
}

bool LoadResourceBytes(int id, std::vector<uint8_t>& out) {
    HRSRC h = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return false;
    DWORD sz = SizeofResource(nullptr, h);
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    void* p = LockResource(g);
    if (!p || sz == 0) return false;
    out.assign((const uint8_t*)p, (const uint8_t*)p + sz);
    return true;
}

// 启动时显示在下方的使用说明 (UTF-8)
const char* const kStartupHelp =
    "【使用说明】\n"
    "本程序把 https://k5.vicicode.com/#/chirp/base 网站导出的信道表格（.xlsx）转换成\n"
    "即时通 D500 能导入的 .dat 文件。泉盛用户可以先用该网站把手台里的信道导出成表格，\n"
    "再回到这里点【转换】。\n"
    "\n"
    "适配范围：只针对「即时通 D500」验证过，未对即时通其他机型做兼容，\n"
    "其他机型可以自行试一下能不能用。\n"
    "\n"
    "可以转换的信息：信道名称、接收频率、发送频率、带宽、发送功率、繁忙禁发、\n"
    "信道号、亚音（模拟亚音 / 数字亚音 / 反向数字亚音）。\n"
    "其余内容（频率步进、倒频、加密、信令、信道模式、DTMF、扫描列表等）不会写进去，\n"
    "需要在 D500 写频软件里手动设置。\n"
    "\n"
    "默认设置（表格里没填、或填了认不得的值时使用）：\n"
    "  · 全局参数与扫描区：用程序自带的默认值（取自一份写频软件生成、能正常导入的\n"
    "    .dat），不随表格变化\n"
    "  · 带宽：宽带（25 kHz）　发送功率：高　繁忙禁发：关　亚音：不保留\n"
    "  · 仅支持 200 条信道：表格里没有的信道号按空信道处理\n"
    "  · 第 200 条信道映射到 0 号槽\n"
    "  · 信道名称超过 8 个汉字（16 字节）会被截断；「#」或「接收频率」没填的行整行跳过\n"
    "\n"
    "亚音请规范填写：模拟亚音填频率（如 88.5），数字亚音填码（如 D023N、D263I，\n"
    "也可以直接写 023）；类型列要和码/频率对得上——两列都填时，码后面的 N/I 后缀优先，\n"
    "其次看类型列。填得不规范，转换结果可能不理想。\n";

void StartupLog() {
    std::wstring dir = k1d500::ExeDir();
    std::wstring text = k1d500::U8(kStartupHelp);
    text += L"────────────────────────────\n";
    text += L"程序目录 : " + dir + L"\n";

    auto list = k1d500::ListBackups(dir);
    bool none = list.empty();
    if (none) {
        text += L"备份文件 : 未找到 K5Channel*.xlsx —— 可先点【生成空白文件】\n";
    } else {
        text += L"备份文件 : 共 " + std::to_wstring(list.size()) + L" 个\n";
        text += L"默认备份 : " + DefaultBackupName(dir) + L"（按修改时间取最新）\n";
    }

    // 一次性设置, 避免在窗口显示前多次 EM_REPLACESEL 造成的重绘错位
    SetWindowTextW(g_hLog, ToCrlf(text).c_str());
    SendMessageW(g_hLog, EM_SETSEL, 0, 0);
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
    SetStatus(none ? L"未找到备份文件" : L"就绪");
}

void CreateTip() {
    g_hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                             WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, g_hMain, nullptr, nullptr, nullptr);
    if (!g_hTip) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_hMain;
    ti.uId = (UINT_PTR)g_hHint;
    ti.lpszText = const_cast<wchar_t*>(L"点击可恢复为默认：自动选择日期最新的备份");
    SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

    if (g_hRev) {
        TOOLINFOW t2{};
        t2.cbSize = sizeof(t2);
        t2.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        t2.hwnd = g_hMain;
        t2.uId = (UINT_PTR)g_hRev;
        t2.lpszText = const_cast<wchar_t*>(
            L"把程序目录里最新的 .dat 转回泉盛信道表格（.xlsx）；.dat 需是 D500 写频软件导出的");
        SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&t2);
    }
}

void OnCreate(HWND hwnd) {
    g_hMain = hwnd;
    g_dpi = GetDpi(hwnd);
    g_brBg = CreateSolidBrush(RGB(250, 250, 252));

    g_hTitle = CreateWindowExW(0, L"STATIC", L"泉盛信道  →  即时通 D500 信道转换",
                               WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, (HMENU)IDC_TITLE,
                               nullptr, nullptr);
    g_hSub = CreateWindowExW(0, L"STATIC",
                             L"读取 k5.vicicode.com 导出的信道表格",
                             WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, (HMENU)IDC_SUB,
                             nullptr, nullptr);

    g_hConvert = CreateWindowExW(0, L"BUTTON", L"转  换",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                                 hwnd, (HMENU)IDC_CONVERT, nullptr, nullptr);
    g_hPick = CreateWindowExW(0, L"BUTTON", L"选择文件…",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                              (HMENU)IDC_PICK, nullptr, nullptr);
    g_hBlank = CreateWindowExW(0, L"BUTTON", L"生成空白文件",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                               hwnd, (HMENU)IDC_BLANK, nullptr, nullptr);
    g_hOpen = CreateWindowExW(0, L"BUTTON", L"打开目录",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                              (HMENU)IDC_OPEN, nullptr, nullptr);
    g_hRev = CreateWindowExW(0, L"BUTTON", L"D500 → 泉盛",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                             (HMENU)IDC_REVERSE, nullptr, nullptr);

    g_hHint = CreateWindowExW(0, L"STATIC", L"默认转换最新版表格",
                              WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY, 0, 0, 0, 0, hwnd,
                              (HMENU)IDC_HINT, nullptr, nullptr);

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                             0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, nullptr, nullptr);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);

    CreateFonts();
    CreateTip();
    DragAcceptFiles(hwnd, TRUE);

    RECT rc;
    GetClientRect(hwnd, &rc);
    Layout(rc.right, rc.bottom);

    if (!LoadResourceBytes(IDR_BASE_BLOB, g_base))
        MessageBoxW(hwnd, L"内置基准数据缺失（资源未编译进程序）。", L"错误", MB_OK | MB_ICONERROR);
    LoadResourceBytes(IDR_BLANK_XLSX, g_blankXlsx);

    StartupLog();

    // 命令行带了文件 (把 xlsx 拖到 exe 图标上) -> 直接当作“已选择”
    if (!g_pendingSelect.empty()) {
        std::wstring p = g_pendingSelect;
        bool absolute = (!p.empty() && p[0] == L'\\') ||
                        (p.size() > 2 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'));
        if (!absolute) p = k1d500::Join(k1d500::ExeDir(), p);
        if (k1d500::FileExists(p)) {
            g_selected = p;
            SetHint(L"将要转换 " + k1d500::BaseName(p));
            AppendLog(L"");
            AppendLog(L"已选择: " + p);
            SetStatus(L"已选择文件");
        }
    }
    SetFocus(g_hConvert);
}

void OnDrop(HDROP hd) {
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    if (n == 0) {
        DragFinish(hd);
        return;
    }
    wchar_t path[MAX_PATH * 2] = L"";
    DragQueryFileW(hd, 0, path, MAX_PATH * 2);
    DragFinish(hd);
    std::wstring p = path;
    if (p.size() > 5 && k1d500::AsciiUpper(k1d500::WToU8(p.substr(p.size() - 5))) == ".XLSX") {
        g_selected = p;
        SetHint(L"将要转换 " + k1d500::BaseName(p));
        SetStatus(L"已选择文件");
        AppendLog(L"");
        AppendLog(L"已选择(拖入): " + p);
    } else {
        MessageBoxW(g_hMain, L"请拖入 K1 信道备份表格（*.xlsx）。", L"不支持的文件",
                    MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;

        case WM_SIZE:
            Layout(LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            RECT r = {0, 0, MinClientWidth(), MinClientHeight()};
            AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            RECT rc;
            GetClientRect(hwnd, &rc);
            Layout(rc.right, rc.bottom);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IDC_CONVERT && code == BN_CLICKED) DoConvert();
            else if (id == IDC_PICK && code == BN_CLICKED) DoPick();
            else if (id == IDC_BLANK && code == BN_CLICKED) DoBlank();
            else if (id == IDC_OPEN && code == BN_CLICKED) DoOpenDir();
            else if (id == IDC_REVERSE && code == BN_CLICKED) DoReverse();
            else if (id == IDC_HINT && code == STN_CLICKED) ResetSelection();
            return 0;
        }

        case WM_DROPFILES:
            OnDrop((HDROP)wp);
            return 0;

        case WM_SETCURSOR:
            if ((HWND)wp == g_hHint) {
                SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_HAND));
                return TRUE;
            }
            break;

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND h = (HWND)lp;
            SetBkColor(dc, RGB(250, 250, 252));
            if (h == g_hHint) {
                SetTextColor(dc, RGB(90, 96, 110));
                return (LRESULT)g_brBg;
            }
            if (h == g_hSub) {
                SetTextColor(dc, RGB(110, 116, 132));
                return (LRESULT)g_brBg;
            }
            if (h == g_hTitle) {
                SetTextColor(dc, RGB(28, 32, 44));
                return (LRESULT)g_brBg;
            }
            if (h == g_hStatus) {
                SetTextColor(dc, RGB(96, 102, 118));
                return (LRESULT)g_brBg;
            }
            SetTextColor(dc, RGB(30, 34, 44));
            return (LRESULT)g_brBg;
        }

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, RGB(24, 26, 32));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, g_brBg);
            // 状态栏上方细线
            int y = rc.bottom - S(34);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(226, 228, 236));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, S(16), y, nullptr);
            LineTo(dc, rc.right - S(16), y);
            SelectObject(dc, old);
            DeleteObject(pen);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    EnableDpiAwareness();

    // 命令行第 1 个非选项参数当作要转换的文件 (支持把 xlsx 拖到 exe 上)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                std::wstring a = argv[i];
                if (!a.empty() && a[0] != L'-') {
                    g_pendingSelect = a;
                    break;
                }
            }
            LocalFree(argv);
        }
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"K1toD500Window";
    if (!RegisterClassExW(&wc)) return 1;

    // 首屏尺寸按当前 DPI 换算: 直接用 900x830 物理像素的话, 125%/150% 缩放时
    // S() 放大后的按钮会超出窗口, 最右边那个按钮被切掉
    {
        typedef UINT(WINAPI * PFN_GetDpiForSystem)();
        static PFN_GetDpiForSystem fn =
            (PFN_GetDpiForSystem)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
        UINT sysDpi = fn ? fn() : 96;
        if (sysDpi >= 72) g_dpi = (int)sysDpi;
    }

    int cw = S(900);
    int chh = S(830);
    if (cw < MinClientWidth()) cw = MinClientWidth();
    if (chh < MinClientHeight()) chh = MinClientHeight();

    RECT rc = {0, 0, cw, chh};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    // 窗口别超出工作区 (小屏 / 高缩放时按可用空间收一下)
    RECT work = {0, 0, 0, 0};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        int aw = work.right - work.left;
        int ah = work.bottom - work.top;
        if (aw > 0 && ww > aw) ww = aw;
        if (ah > 0 && wh > ah) wh = ah;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"泉盛信道",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DestroyFonts();
    if (g_brBg) DeleteObject(g_brBg);
    return (int)m.wParam;
}
