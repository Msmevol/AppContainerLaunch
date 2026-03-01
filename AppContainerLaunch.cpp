// LaunchOpenCodeTUI.cpp
//
// Launch OpenCode TUI in an AppContainer sandbox.
//
// Mode A (daily use / single WorkRoot):
//   - OpenCode can read/write ONLY inside:
//       1) WorkRoot (-p / config: [Directories] WorkRoot or WorkingDirectory)
//       2) SandboxDataRoot (-s / config: [Directories] SandboxDataRoot)
//          (SandboxDataRoot must be under WorkRoot)
//     Projects should live under WorkRoot. You can optionally set -j (ProjectDir)
//     to start OpenCode in a specific project directory under WorkRoot.
//
//   - OpenCode can read/execute tools ONLY inside authorized tool directories (-a).
//
// Environment / side-effects discipline:
//   - Do NOT modify host env vars; child gets its own environment block.
//   - Host-level changes that are required for the sandbox to run are TEMPORARY and
//     will be restored after opencode exits:
//       * HKCU\Console\LowBoxConsoleEnabled (backup & restore)
//       * B: drive mapping for Bun/OpenTUI (DefineDosDevice; create & remove)
//       * ACL grants for AppContainer SID (grant & revoke)
//       * AppContainer profile (delete only if created by this run and not retained)
//
// Bun/OpenTUI fix:
//   - Some OpenCode Windows builds (Bun compiled) load OpenTUI native DLL from
//     "B:\~BUN\root\...". In an AppContainer, this can fail unless B: exists and is writable.
//   - We map B: -> <SandboxDataRoot>\bun-drive so B:\~BUN\root stays inside SandboxDataRoot.
//
// Cleanup strategy:
//   - If -w is specified, launcher waits for opencode and restores state itself.
//   - Otherwise, launcher spawns a hidden cleanup helper (same exe: --cleanup <ini>)
//     which waits for opencode PID and then restores state, so your terminal is not blocked.
//
// Build (VS Dev Cmd):
//   cl /EHsc /W4 LaunchOpenCodeTUI.cpp /link userenv.lib advapi32.lib onecoreuap.lib

#include <Windows.h>
#include <UserEnv.h>
#include <sddl.h>

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <cwctype>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "onecoreuap.lib")

// ============================================================================
// RAII SID wrapper
// ============================================================================

struct SidAttrWrap : public SID_AND_ATTRIBUTES {
    SidAttrWrap(SID_AND_ATTRIBUTES obj) : SID_AND_ATTRIBUTES(obj) {}

    SidAttrWrap(SidAttrWrap&& obj) noexcept {
        Sid = nullptr; Attributes = 0;
        std::swap(obj.Sid, Sid);
        std::swap(obj.Attributes, Attributes);
    }

    SidAttrWrap& operator=(SidAttrWrap&& obj) noexcept {
        if (this != &obj) {
            if (Sid) LocalFree(Sid);
            Sid = nullptr; Attributes = 0;
            std::swap(obj.Sid, Sid);
            std::swap(obj.Attributes, Attributes);
        }
        return *this;
    }

    ~SidAttrWrap() {
        if (Sid) { LocalFree(Sid); Sid = nullptr; }
    }

    SidAttrWrap(const SidAttrWrap&) = delete;
    SidAttrWrap& operator=(const SidAttrWrap&) = delete;
};

// ============================================================================
// Logging
// ============================================================================

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
static bool g_logLockInit = false;

void LogInitLockOnly()
{
    if (!g_logLockInit) {
        InitializeCriticalSection(&g_logLock);
        g_logLockInit = true;
    }
}

bool LogSwitchToFile(const std::wstring& logPath)
{
    if (!g_logLockInit) LogInitLockOnly();

    FILE* f = nullptr;
    _wfopen_s(&f, logPath.c_str(), L"a, ccs=UTF-8");
    if (!f) {
        size_t pos = logPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring parent = logPath.substr(0, pos);
            CreateDirectoryW(parent.c_str(), nullptr);
            _wfopen_s(&f, logPath.c_str(), L"a, ccs=UTF-8");
        }
    }

    if (!f) return false;

    EnterCriticalSection(&g_logLock);
    FILE* old = g_logFile;
    g_logFile = f;
    LeaveCriticalSection(&g_logLock);

    if (old) fclose(old);
    return true;
}

void LogClose()
{
    if (!g_logLockInit) return;

    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    DeleteCriticalSection(&g_logLock);
    g_logLockInit = false;
}

void Log(const WCHAR* fmt, ...)
{
    if (!g_logLockInit) LogInitLockOnly();

    WCHAR buf[2048] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf) - 1, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR timeBuf[64] = {};
    _snwprintf_s(timeBuf, _countof(timeBuf), _TRUNCATE,
                 L"[%04d-%02d-%02d %02d:%02d:%02d] ",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);

    // console output (best-effort)
    wprintf(L"%s%s", timeBuf, buf);

    // file output
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fwprintf(g_logFile, L"%s%s", timeBuf, buf);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
}

// ============================================================================
// Global configuration
// ============================================================================

std::wstring ExeToLaunchStr;                         // -i : opencode.exe path
std::wstring PackageMoniker;                         // -m : AppContainer name
std::wstring PackageDisplayName;                     // -d : display name
std::vector<SidAttrWrap> CapabilityList;             // capability list

bool WaitForExit = false;                            // -w : wait for exit (optional)
bool RetainProfile = false;                          // -r : retain profile after exit
bool LaunchAsLpac = false;                           // -l : LPAC mode

std::wstring WorkingDirectory;                       // -p : WorkRoot (Mode A)
std::wstring ProjectDirectory;                       // -j : project dir under WorkRoot (optional)

std::vector<std::wstring> AdditionalGrantPaths;      // -a : tool paths (RX)
bool SkipDefaultCapabilities = false;                // -n : skip default caps
std::wstring ExtraArgs;                              // -x : extra args for opencode
std::wstring SandboxDataRoot;                        // -s : sandbox data root (must be under WorkRoot)

std::map<std::wstring, std::wstring> CustomEnvVars;  // custom env vars from config
std::wstring ExtraCapabilitiesStr;                   // extra caps string from config

// Resolved sandbox subdirectory paths
std::wstring g_sandboxRoot;
std::wstring g_dirConfig;
std::wstring g_dirConfigOC;
std::wstring g_dirData;
std::wstring g_dirDataOC;
std::wstring g_dirTemp;
std::wstring g_dirHome;
std::wstring g_dirCache;
std::wstring g_openCodeJson;

std::wstring g_dirBunDrive;         // <sandbox>\bun-drive (mapped as B:)
std::wstring g_dirBunRoot;          // <sandbox>\bun-drive\~BUN\root
std::wstring g_dirBunTranspileCache;

// ============================================================================
// Default capabilities (daily): outbound network only
// ============================================================================

static const PCWSTR DefaultCaps[] = { L"internetClient" };
static const DWORD DefaultCapCount = _countof(DefaultCaps);

// ============================================================================
// ACL permission templates
// ============================================================================

static const PCWSTR kPermRW = L"(OI)(CI)(M)";
static const PCWSTR kPermRX = L"(OI)(CI)(RX)";

// NOTE: To minimize host modifications and make rollback feasible,
// we do NOT use /T (recursive) in icacls. We rely on inheritance.
// That means children with broken inheritance may still be inaccessible.
static const bool kUseRecursiveAclPropagation = false;

// ============================================================================
// Host-state rollback tracking
// ============================================================================

struct RegDwordBackup {
    bool queried = false;
    bool existed = false;
    DWORD oldValue = 0;
    bool changed = false;
};
static RegDwordBackup g_lowBoxBackup;

static bool g_profileCreatedThisRun = false;

static std::wstring g_appContainerSidStr;         // "S-1-15-2-..."
static std::vector<std::wstring> g_grantedPaths;  // paths we granted and should revoke

// B: drive mapping tracking
static const wchar_t kBunDriveLetter = L'B';
static std::wstring g_bunDeviceName;    // "B:"
static std::wstring g_bunRawTarget;     // "\??\C:\...\bun-drive"
static bool g_bunMappingCreated = false;

// Cleanup helper file
static std::wstring g_cleanupIniPath;

// ============================================================================
// Forward declarations
// ============================================================================

void PrintUsage();

void FreeSidArray(PSID* sids, ULONG count);
std::wstring SidToString(PSID pSid);

bool EnsureDirectoryRecursive(const std::wstring& path);
std::wstring Utf8ToWide(const std::string& str);
std::wstring GetExeDirectory();

bool LoadConfigFile(const std::wstring& configPath);

bool ParseCapabilityList(WCHAR* psCapabilities);
bool AddDefaultCapabilities();

bool ParseArguments(int argc, WCHAR** argv);

bool DirectoryExists(const std::wstring& path);
bool FileExists(const std::wstring& path);

bool IsAbsolutePathW(const std::wstring& path);
std::wstring GetFullPathW(const std::wstring& path, const std::wstring& baseDir);
std::wstring NormalizeDirForCompare(const std::wstring& path);
bool IsSubPathOf(const std::wstring& childPath, const std::wstring& rootPath);

bool ContainsQuote(const std::wstring& s);

// ACL
bool GrantPath(const std::wstring& path, const std::wstring& sidStr, const std::wstring& perms);
bool RevokePath(const std::wstring& path, const std::wstring& sidStr);
void RememberGrantedPath(const std::wstring& path);

// Sandbox dirs
void PrepareSandboxDirectories();

// Environment block
void BuildSandboxEnvironmentBlock(std::vector<WCHAR>& outBlock);

// Host env changes (registry + B mapping)
bool EnableLowBoxConsoleTemp();
void RestoreLowBoxConsole();

std::wstring BuildRawTargetPathForDefineDosDevice(const std::wstring& dir);
bool QueryDosDeviceTargetSafe(const std::wstring& deviceName, std::wstring& outTarget, DWORD& outErr);
bool EnsureBunDriveMapping();
void RemoveBunDriveMappingIfCreated();

// AppContainer profile
DWORD CreateProfile(PSID* outSid);
DWORD DeleteProfileIfCreatedThisRun();

// Launch
DWORD LaunchProcess(PSID packageSid, PROCESS_INFORMATION& outPi);

// Cleanup helper
bool WriteCleanupIni(const std::wstring& iniPath,
                     DWORD opencodePid,
                     const std::wstring& exePathForHelper,
                     bool profileCreatedThisRun,
                     bool retainProfile,
                     const RegDwordBackup& lowBox,
                     bool bunMappingCreated,
                     const std::wstring& bunDeviceName,
                     const std::wstring& bunRawTarget,
                     const std::wstring& moniker,
                     const std::wstring& sidStr,
                     const std::vector<std::wstring>& grantedPaths,
                     const std::wstring& logPath);

int RunCleanupMode(const std::wstring& iniPath);
bool SpawnCleanupHelper(const std::wstring& helperExe, const std::wstring& iniPath);

// ============================================================================
// Utility functions
// ============================================================================

void FreeSidArray(PSID* sids, ULONG count)
{
    for (ULONG i = 0; i < count; i++)
        LocalFree(sids[i]);
    LocalFree(sids);
}

std::wstring SidToString(PSID pSid)
{
    LPWSTR s = nullptr;
    if (ConvertSidToStringSid(pSid, &s)) {
        std::wstring r(s);
        LocalFree(s);
        return r;
    }
    return L"";
}

bool EnsureDirectoryRecursive(const std::wstring& path)
{
    if (path.empty()) return false;

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;

    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 0) {
        std::wstring parent = path.substr(0, pos);
        if (parent.length() > 2 || (parent.length() == 2 && parent[1] != L':'))
            EnsureDirectoryRecursive(parent);
    }

    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring Utf8ToWide(const std::string& str)
{
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), nullptr, 0);
        if (len <= 0) return L"";
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), &result[0], len);
        return result;
    }
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], len);
    return result;
}

std::wstring GetExeDirectory()
{
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? p.substr(0, pos) : p;
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsAbsolutePathW(const std::wstring& path)
{
    if (path.empty()) return false;
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && ((path[0] == L'\\' && path[1] == L'\\') ||
                             (path[0] == L'/'  && path[1] == L'/'))) return true;
    if (!path.empty() && (path[0] == L'\\' || path[0] == L'/')) return true;
    return false;
}

std::wstring GetFullPathW(const std::wstring& path, const std::wstring& baseDir)
{
    if (path.empty()) return L"";

    std::wstring combined = path;
    if (!baseDir.empty() && !IsAbsolutePathW(path)) {
        if (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/'))
            combined = baseDir + path;
        else
            combined = baseDir + L"\\" + path;
    }

    DWORD needed = GetFullPathNameW(combined.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return L"";

    std::wstring out(needed, L'\0');
    DWORD written = GetFullPathNameW(combined.c_str(), needed, &out[0], nullptr);
    if (written == 0 || written >= needed) return L"";

    out.resize(written);
    for (auto& ch : out) if (ch == L'/') ch = L'\\';
    return out;
}

std::wstring NormalizeDirForCompare(const std::wstring& path)
{
    std::wstring full = GetFullPathW(path, L"");
    if (full.empty()) return L"";

    for (auto& ch : full) if (ch == L'/') ch = L'\\';

    while (full.size() > 3 && full.back() == L'\\')
        full.pop_back();

    if (!full.empty() && full.back() != L'\\')
        full.push_back(L'\\');

    return full;
}

bool IsSubPathOf(const std::wstring& childPath, const std::wstring& rootPath)
{
    std::wstring child = NormalizeDirForCompare(childPath);
    std::wstring root  = NormalizeDirForCompare(rootPath);

    if (child.empty() || root.empty()) return false;
    if (child.size() < root.size()) return false;

    return _wcsnicmp(child.c_str(), root.c_str(), root.size()) == 0;
}

bool ContainsQuote(const std::wstring& s)
{
    return s.find(L'"') != std::wstring::npos;
}

// ============================================================================
// INI config parser (same as your version)
// ============================================================================

bool LoadConfigFile(const std::wstring& configPath)
{
    char narrowPath[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, 0, configPath.c_str(), -1,
                        narrowPath, sizeof(narrowPath), nullptr, nullptr);

    std::ifstream file(narrowPath, std::ios::in);
    if (!file.is_open()) {
        Log(L"[INFO] Config file not found: %s (using command-line args)\n",
            configPath.c_str());
        return false;
    }

    Log(L"[INFO] Loading config: %s\n", configPath.c_str());

    std::string line;
    std::string currentSection;

    while (std::getline(file, line))
    {
        // Strip UTF-8 BOM
        if (line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
        {
            line = line.substr(3);
        }

        // Trim
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(s, e - s + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line[0] == '[') {
            size_t endBracket = line.find(']');
            if (endBracket != std::string::npos)
                currentSection = line.substr(1, endBracket - 1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        size_t ks = key.find_first_not_of(" \t");
        size_t ke = key.find_last_not_of(" \t");
        if (ks != std::string::npos && ke != std::string::npos)
            key = key.substr(ks, ke - ks + 1);
        else
            key = "";

        size_t vs = val.find_first_not_of(" \t");
        size_t ve = val.find_last_not_of(" \t");
        if (vs != std::string::npos && ve != std::string::npos)
            val = val.substr(vs, ve - vs + 1);
        else
            val = "";

        if (key.empty()) continue;

        std::wstring wkey = Utf8ToWide(key);
        std::wstring wval = Utf8ToWide(val);

        if (currentSection == "General")
        {
            if (key == "Moniker" && PackageMoniker.empty())
                PackageMoniker = wval;
            else if (key == "DisplayName" && PackageDisplayName.empty())
                PackageDisplayName = wval;
            else if (key == "WaitForExit" && !WaitForExit)
                WaitForExit = (val == "true" || val == "1");
            else if (key == "RetainProfile" && !RetainProfile)
                RetainProfile = (val == "true" || val == "1");
            else if (key == "LPAC" && !LaunchAsLpac)
                LaunchAsLpac = (val == "true" || val == "1");
        }
        else if (currentSection == "Executable")
        {
            if (key == "ExePath" && ExeToLaunchStr.empty())
                ExeToLaunchStr = wval;
            else if (key == "ExtraArgs" && ExtraArgs.empty())
                ExtraArgs = wval;
            else if ((key == "ProjectDir" || key == "ProjectDirectory") && ProjectDirectory.empty())
                ProjectDirectory = wval;
        }
        else if (currentSection == "Directories")
        {
            if ((key == "WorkRoot" || key == "WorkingDirectory") && WorkingDirectory.empty())
                WorkingDirectory = wval;
            else if ((key == "ProjectDir" || key == "ProjectDirectory") && ProjectDirectory.empty())
                ProjectDirectory = wval;
            else if (key == "SandboxDataRoot" && SandboxDataRoot.empty())
                SandboxDataRoot = wval;
        }
        else if (currentSection == "Capabilities")
        {
            if (key == "SkipDefaults" && !SkipDefaultCapabilities)
                SkipDefaultCapabilities = (val == "true" || val == "1");
            else if (key == "ExtraCapabilities" && !wval.empty())
                ExtraCapabilitiesStr = wval;
        }
        else if (currentSection == "GrantPaths")
        {
            if (!wval.empty())
                AdditionalGrantPaths.push_back(wval);
        }
        else if (currentSection == "Environment")
        {
            if (!wval.empty())
                CustomEnvVars[wkey] = wval;
        }
    }

    file.close();
    Log(L"[INFO] Config file loaded successfully\n");
    return true;
}

// ============================================================================
// Usage
// ============================================================================

void PrintUsage()
{
    Log(L"LaunchOpenCodeTUI.exe - Launch OpenCode TUI in AppContainer sandbox (Mode A)\n\n");
    Log(L"Usage: LaunchOpenCodeTUI.exe -m <name> -i <exe_path> -p <work_root> [options]\n\n");
    Log(L"Required:\n");
    Log(L"  -i <path>     Path to opencode.exe\n");
    Log(L"  -m <name>     AppContainer Moniker\n");
    Log(L"  -p <dir>      WorkRoot (only authorized RW root)\n\n");
    Log(L"Optional:\n");
    Log(L"  -j <dir>      Project directory (must be under WorkRoot)\n");
    Log(L"  -a <path>     Tool path to grant Read+Execute (can be used multiple times)\n");
    Log(L"  -c <caps>     Extra capabilities (semicolon-separated)\n");
    Log(L"  -x <args>     Extra arguments passed to opencode\n");
    Log(L"  -d <name>     Display name\n");
    Log(L"  -s <dir>      Sandbox data root (default: <WorkRoot>\\.opencode-sandbox)\n");
    Log(L"  -n            Skip default capabilities (default: internetClient)\n");
    Log(L"  -w            Wait for opencode to exit (then cleanup happens in this process)\n");
    Log(L"  -r            Retain AppContainer profile after exit (only affects profile)\n");
    Log(L"  -l            LPAC mode (not recommended for TUI)\n\n");
    Log(L"Note: without -w, a hidden cleanup helper is spawned to restore host state after opencode exits.\n");
}

// ============================================================================
// Capability parsing
// ============================================================================

bool ParseCapabilityList(WCHAR* psCapabilities)
{
    WCHAR* ctx = nullptr;
    WCHAR* psCap = wcstok_s(psCapabilities, L";", &ctx);

    while (psCap != nullptr)
    {
        bool parsed = false;

        {
            SID_AND_ATTRIBUTES sa = {};
            sa.Attributes = SE_GROUP_ENABLED;
            if (ConvertStringSidToSid(psCap, &sa.Sid)) {
                CapabilityList.push_back(sa);
                parsed = true;
            }
        }

        if (!parsed) {
            PSID* grpSids = nullptr; DWORD grpLen = 0;
            PSID* capSids = nullptr; DWORD capLen = 0;
            if (DeriveCapabilitySidsFromName(psCap, &grpSids, &grpLen,
                                             &capSids, &capLen))
            {
                for (DWORD i = 0; i < capLen; ++i)
                    CapabilityList.push_back(SID_AND_ATTRIBUTES{ capSids[i], SE_GROUP_ENABLED });

                FreeSidArray(grpSids, grpLen);
                LocalFree(capSids);
                parsed = true;
            }
        }

        if (!parsed) {
            Log(L"[ERROR] Cannot parse capability: %s\n", psCap);
            return false;
        }

        psCap = wcstok_s(nullptr, L";", &ctx);
    }
    return true;
}

bool AddDefaultCapabilities()
{
    for (DWORD i = 0; i < DefaultCapCount; i++)
    {
        PSID* grpSids = nullptr; DWORD grpLen = 0;
        PSID* capSids = nullptr; DWORD capLen = 0;

        if (!DeriveCapabilitySidsFromName(DefaultCaps[i], &grpSids, &grpLen,
                                          &capSids, &capLen))
        {
            Log(L"[ERROR] Cannot derive capability: %s (err=%d)\n",
                DefaultCaps[i], GetLastError());
            return false;
        }

        for (DWORD j = 0; j < capLen; ++j)
            CapabilityList.push_back(SID_AND_ATTRIBUTES{ capSids[j], SE_GROUP_ENABLED });

        FreeSidArray(grpSids, grpLen);
        LocalFree(capSids);

        Log(L"  + %s\n", DefaultCaps[i]);
    }
    return true;
}

// ============================================================================
// Argument parsing
// ============================================================================

bool ParseArguments(int argc, WCHAR** argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] != '-' && argv[i][0] != '/')
            continue;

        // We use single-letter flags. Long flags reserved.
        if (argv[i][1] == L'-')
            continue;

        wchar_t opt = (wchar_t)towlower(argv[i][1]);

        switch (opt)
        {
        case 'i':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            ExeToLaunchStr = argv[++i];
            break;
        case 'm':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            PackageMoniker = argv[++i];
            break;
        case 'c':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            {
                std::wstring caps = argv[++i];
                std::vector<WCHAR> buf(caps.begin(), caps.end());
                buf.push_back(L'\0');
                if (!ParseCapabilityList(buf.data()))
                    return false;
            }
            break;
        case 'd':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            PackageDisplayName = argv[++i];
            break;
        case 'p':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            WorkingDirectory = argv[++i];
            break;
        case 'j':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            ProjectDirectory = argv[++i];
            break;
        case 'a':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            AdditionalGrantPaths.push_back(argv[++i]);
            break;
        case 'x':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            ExtraArgs = argv[++i];
            break;
        case 's':
            if (i + 1 >= argc) { PrintUsage(); return false; }
            SandboxDataRoot = argv[++i];
            break;
        case 'n':
            SkipDefaultCapabilities = true;
            break;
        case 'w':
            WaitForExit = true;
            break;
        case 'r':
            RetainProfile = true;
            break;
        case 'l':
            LaunchAsLpac = true;
            break;
        case 'h':
        case '?':
            PrintUsage();
            return false;
        default:
            Log(L"[WARN] Unknown argument: %s\n", argv[i]);
            break;
        }
    }
    return true;
}

// ============================================================================
// ACL helpers
// ============================================================================

void RememberGrantedPath(const std::wstring& path)
{
    // store full normalized path, no trailing "\"
    std::wstring full = GetFullPathW(path, L"");
    if (full.empty()) full = path;

    while (full.size() > 3 && full.back() == L'\\')
        full.pop_back();

    auto it = std::find_if(g_grantedPaths.begin(), g_grantedPaths.end(),
        [&](const std::wstring& p) { return _wcsicmp(p.c_str(), full.c_str()) == 0; });

    if (it == g_grantedPaths.end())
        g_grantedPaths.push_back(full);
}

bool GrantPath(const std::wstring& path, const std::wstring& sidStr, const std::wstring& perms)
{
    if (path.empty() || sidStr.empty() || perms.empty())
        return false;

    if (ContainsQuote(path)) {
        Log(L"[ERROR] Path contains quotes (blocked for safety): %s\n", path.c_str());
        return false;
    }

    // Minimal side-effects: no /T recursion (inheritance is used).
    std::wstring cmd = L"icacls \"" + path + L"\" /grant *"
                     + sidStr + L":" + perms + L" /C /Q";
    if (kUseRecursiveAclPropagation)
        cmd += L" /T";

    int ret = _wsystem(cmd.c_str());
    if (ret != 0) {
        Log(L"  [WARN] ACL grant failed (ret=%d): %s\n", ret, path.c_str());
        return false;
    }

    Log(L"  [OK] GRANT %s -> %s\n", path.c_str(), perms.c_str());
    RememberGrantedPath(path);
    return true;
}

bool RevokePath(const std::wstring& path, const std::wstring& sidStr)
{
    if (path.empty() || sidStr.empty())
        return false;

    if (ContainsQuote(path)) {
        Log(L"[ERROR] Path contains quotes (blocked for safety): %s\n", path.c_str());
        return false;
    }

    std::wstring cmd = L"icacls \"" + path + L"\" /remove *"
                     + sidStr + L" /C /Q";
    if (kUseRecursiveAclPropagation)
        cmd += L" /T";

    int ret = _wsystem(cmd.c_str());
    if (ret != 0) {
        Log(L"  [WARN] ACL revoke failed (ret=%d): %s\n", ret, path.c_str());
        return false;
    }

    Log(L"  [OK] REVOKE %s\n", path.c_str());
    return true;
}

// ============================================================================
// Prepare sandbox directories
// ============================================================================

void PrepareSandboxDirectories()
{
    g_sandboxRoot = SandboxDataRoot;

    g_dirConfig   = g_sandboxRoot + L"\\config";
    g_dirConfigOC = g_sandboxRoot + L"\\config\\opencode";
    g_dirData     = g_sandboxRoot + L"\\data";
    g_dirDataOC   = g_sandboxRoot + L"\\data\\opencode";
    g_dirTemp     = g_sandboxRoot + L"\\temp";
    g_dirHome     = g_sandboxRoot + L"\\home";
    g_dirCache    = g_sandboxRoot + L"\\cache";
    g_openCodeJson = g_sandboxRoot + L"\\opencode.json";

    g_dirBunDrive = g_sandboxRoot + L"\\bun-drive";
    g_dirBunRoot  = g_dirBunDrive + L"\\~BUN\\root";
    g_dirBunTranspileCache = g_dirCache + L"\\bun-transpiler-cache";

    EnsureDirectoryRecursive(g_sandboxRoot);
    EnsureDirectoryRecursive(g_dirConfigOC);
    EnsureDirectoryRecursive(g_dirDataOC);
    EnsureDirectoryRecursive(g_dirTemp);
    EnsureDirectoryRecursive(g_dirHome);
    EnsureDirectoryRecursive(g_dirCache);

    EnsureDirectoryRecursive(g_dirBunRoot);
    EnsureDirectoryRecursive(g_dirBunTranspileCache);

    Log(L"[INFO] Sandbox directories prepared:\n");
    Log(L"  root  -> %s\n", g_sandboxRoot.c_str());
    Log(L"  home  -> %s\n", g_dirHome.c_str());
    Log(L"  temp  -> %s\n", g_dirTemp.c_str());
    Log(L"  cache -> %s\n", g_dirCache.c_str());
    Log(L"  bun-drive -> %s\n", g_dirBunDrive.c_str());
}

// ============================================================================
// Build sandbox environment variable block (child-only)
// ============================================================================

void BuildSandboxEnvironmentBlock(std::vector<WCHAR>& outBlock)
{
    std::map<std::wstring, std::wstring> envMap;

    LPWCH envStrings = GetEnvironmentStringsW();
    if (envStrings) {
        LPCWSTR ptr = envStrings;
        while (*ptr) {
            std::wstring entry(ptr);
            size_t startSearch = (entry[0] == L'=') ? 1 : 0;
            size_t eqPos = entry.find(L'=', startSearch);
            if (eqPos != std::wstring::npos) {
                std::wstring envKey = entry.substr(0, eqPos);
                std::wstring envVal = entry.substr(eqPos + 1);
                envMap[envKey] = envVal;
            }
            ptr += entry.length() + 1;
        }
        FreeEnvironmentStringsW(envStrings);
    }

    envMap[L"HOME"]        = g_dirHome;
    envMap[L"USERPROFILE"] = g_dirHome;

    if (g_dirHome.length() >= 2 && g_dirHome[1] == L':') {
        envMap[L"HOMEDRIVE"] = g_dirHome.substr(0, 2);
        envMap[L"HOMEPATH"]  = g_dirHome.substr(2);
    }

    envMap[L"TEMP"]   = g_dirTemp;
    envMap[L"TMP"]    = g_dirTemp;
    envMap[L"TMPDIR"] = g_dirTemp;

    envMap[L"XDG_CONFIG_HOME"] = g_dirConfig;
    envMap[L"XDG_DATA_HOME"]   = g_dirData;
    envMap[L"XDG_CACHE_HOME"]  = g_dirCache;

    envMap[L"APPDATA"]      = g_dirConfig;
    envMap[L"LOCALAPPDATA"] = g_dirData;

    envMap[L"OPENCODE_CONFIG_DIR"] = g_dirConfigOC;
    envMap[L"OPENCODE_DATA_DIR"]   = g_dirDataOC;

    // If you want a single-file config, keep this.
    envMap[L"OPENCODE_CONFIG"] = g_openCodeJson;

    // Bun cache (kept inside sandbox)
    envMap[L"BUN_RUNTIME_TRANSPILER_CACHE_PATH"] = g_dirBunTranspileCache;

    for (const auto& kv : CustomEnvVars) {
        envMap[kv.first] = kv.second;
        bool isSensitive =
            (kv.first.find(L"KEY") != std::wstring::npos ||
             kv.first.find(L"SECRET") != std::wstring::npos ||
             kv.first.find(L"TOKEN") != std::wstring::npos ||
             kv.first.find(L"PASSWORD") != std::wstring::npos);
        Log(L"  [ENV] %s = %s\n", kv.first.c_str(),
            isSensitive ? L"***" : kv.second.c_str());
    }

    outBlock.clear();
    for (const auto& kv : envMap) {
        std::wstring entry = kv.first + L"=" + kv.second;
        outBlock.insert(outBlock.end(), entry.begin(), entry.end());
        outBlock.push_back(L'\0');
    }
    outBlock.push_back(L'\0');
}

// ============================================================================
// Registry: LowBoxConsoleEnabled (backup & restore)
// ============================================================================

bool EnableLowBoxConsoleTemp()
{
    g_lowBoxBackup = RegDwordBackup{};

    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);

    LSTATUS ls = RegGetValueW(HKEY_CURRENT_USER,
                              L"Console",
                              L"LowBoxConsoleEnabled",
                              RRF_RT_REG_DWORD,
                              &type,
                              &value,
                              &size);

    g_lowBoxBackup.queried = true;

    if (ls == ERROR_SUCCESS) {
        g_lowBoxBackup.existed = true;
        g_lowBoxBackup.oldValue = value;
        if (value == 1) {
            Log(L"[INFO] LowBoxConsoleEnabled already 1 (no change)\n");
            return true;
        }
    } else if (ls == ERROR_FILE_NOT_FOUND) {
        g_lowBoxBackup.existed = false;
    } else {
        Log(L"[WARN] RegGetValue LowBoxConsoleEnabled failed (err=%ld)\n", ls);
        g_lowBoxBackup.existed = false;
    }

    DWORD one = 1;
    ls = RegSetKeyValueW(HKEY_CURRENT_USER,
                         L"Console",
                         L"LowBoxConsoleEnabled",
                         REG_DWORD,
                         &one,
                         sizeof(one));

    if (ls != ERROR_SUCCESS) {
        Log(L"[WARN] Failed to set LowBoxConsoleEnabled (err=%ld)\n", ls);
        return false;
    }

    g_lowBoxBackup.changed = true;
    Log(L"[INFO] LowBoxConsoleEnabled set to 1 (temporary)\n");
    return true;
}

void RestoreLowBoxConsole()
{
    if (!g_lowBoxBackup.changed)
        return;

    if (g_lowBoxBackup.existed) {
        DWORD v = g_lowBoxBackup.oldValue;
        LSTATUS ls = RegSetKeyValueW(HKEY_CURRENT_USER,
                                     L"Console",
                                     L"LowBoxConsoleEnabled",
                                     REG_DWORD,
                                     &v,
                                     sizeof(v));
        if (ls == ERROR_SUCCESS)
            Log(L"[INFO] Restored LowBoxConsoleEnabled to %u\n", v);
        else
            Log(L"[WARN] Failed to restore LowBoxConsoleEnabled (err=%ld)\n", ls);
    } else {
        LSTATUS ls = RegDeleteKeyValueW(HKEY_CURRENT_USER,
                                        L"Console",
                                        L"LowBoxConsoleEnabled");
        if (ls == ERROR_SUCCESS || ls == ERROR_FILE_NOT_FOUND)
            Log(L"[INFO] Deleted LowBoxConsoleEnabled (restored to absent)\n");
        else
            Log(L"[WARN] Failed to delete LowBoxConsoleEnabled (err=%ld)\n", ls);
    }
}

// ============================================================================
// Bun/OpenTUI B: mapping (backup & restore)
// ============================================================================

std::wstring BuildRawTargetPathForDefineDosDevice(const std::wstring& dir)
{
    std::wstring p = GetFullPathW(dir, L"");
    if (p.empty()) p = dir;
    for (auto& ch : p) if (ch == L'/') ch = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return std::wstring(L"\\??\\") + p;
}

bool QueryDosDeviceTargetSafe(const std::wstring& deviceName, std::wstring& outTarget, DWORD& outErr)
{
    outTarget.clear();
    outErr = ERROR_SUCCESS;

    std::vector<WCHAR> buf(32768, L'\0');
    DWORD len = QueryDosDeviceW(deviceName.c_str(), buf.data(), (DWORD)buf.size());
    if (len == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            outErr = ERROR_FILE_NOT_FOUND;
            return true;
        }
        outErr = err;
        return false;
    }

    outTarget = std::wstring(buf.data()); // first string
    return true;
}

static std::wstring NormalizeNtPathForCompare(const std::wstring& s)
{
    std::wstring t = s;
    for (auto& ch : t) {
        if (ch == L'/') ch = L'\\';
        ch = (wchar_t)towlower(ch);
    }
    while (!t.empty() && t.back() == L'\\')
        t.pop_back();
    return t;
}

bool EnsureBunDriveMapping()
{
    g_bunDeviceName = std::wstring(1, kBunDriveLetter) + L":";
    g_bunRawTarget = BuildRawTargetPathForDefineDosDevice(g_dirBunDrive);

    if (!DirectoryExists(g_dirBunDrive)) {
        Log(L"[ERROR] Bun drive target missing: %s\n", g_dirBunDrive.c_str());
        return false;
    }

    std::wstring existing;
    DWORD qerr = ERROR_SUCCESS;
    if (!QueryDosDeviceTargetSafe(g_bunDeviceName, existing, qerr)) {
        Log(L"[ERROR] QueryDosDevice(%s) failed: %d\n", g_bunDeviceName.c_str(), qerr);
        return false;
    }

    if (qerr == ERROR_SUCCESS) {
        // Already exists; must match exactly our target, otherwise it's unsafe.
        if (NormalizeNtPathForCompare(existing) == NormalizeNtPathForCompare(g_bunRawTarget)) {
            Log(L"[INFO] %s already mapped to sandbox target (no change)\n", g_bunDeviceName.c_str());
            g_bunMappingCreated = false;
            return true;
        }

        Log(L"[ERROR] %s already in use and points elsewhere:\n", g_bunDeviceName.c_str());
        Log(L"        Existing: %s\n", existing.c_str());
        Log(L"        Expected: %s\n", g_bunRawTarget.c_str());
        return false;
    }

    // Not found -> create mapping
    DWORD flags = DDD_NO_BROADCAST_SYSTEM | DDD_RAW_TARGET_PATH;
    if (!DefineDosDeviceW(flags, g_bunDeviceName.c_str(), g_bunRawTarget.c_str())) {
        DWORD err = GetLastError();
        Log(L"[ERROR] DefineDosDevice(%s -> %s) failed: %d\n",
            g_bunDeviceName.c_str(), g_bunRawTarget.c_str(), err);
        return false;
    }

    g_bunMappingCreated = true;
    Log(L"[INFO] Mapped %s -> %s (temporary)\n", g_bunDeviceName.c_str(), g_dirBunDrive.c_str());

    // ensure B:\~BUN\root exists through mapping
    std::wstring bunRootOnDrive = std::wstring(1, kBunDriveLetter) + L":\\~BUN\\root";
    EnsureDirectoryRecursive(bunRootOnDrive);

    return true;
}

void RemoveBunDriveMappingIfCreated()
{
    if (!g_bunMappingCreated)
        return;

    DWORD flags = DDD_REMOVE_DEFINITION
                | DDD_EXACT_MATCH_ON_REMOVE
                | DDD_NO_BROADCAST_SYSTEM
                | DDD_RAW_TARGET_PATH;

    if (!DefineDosDeviceW(flags, g_bunDeviceName.c_str(), g_bunRawTarget.c_str())) {
        DWORD err = GetLastError();
        Log(L"[WARN] Failed to remove %s mapping (err=%d)\n", g_bunDeviceName.c_str(), err);
    } else {
        Log(L"[INFO] Removed %s mapping\n", g_bunDeviceName.c_str());
    }
}

// ============================================================================
// AppContainer profile management
// ============================================================================

DWORD CreateProfile(PSID* outSid)
{
    g_profileCreatedThisRun = false;

    PSID packageSid = nullptr;
    PCWSTR display = PackageDisplayName.empty()
                   ? PackageMoniker.c_str()
                   : PackageDisplayName.c_str();

    SID_AND_ATTRIBUTES* caps = CapabilityList.empty() ? nullptr : CapabilityList.data();
    DWORD capCount = static_cast<DWORD>(CapabilityList.size());

    HRESULT hr = CreateAppContainerProfile(
        PackageMoniker.c_str(), display, display,
        caps, capCount,
        &packageSid);

    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            Log(L"[INFO] Profile already exists, retrieving SID...\n");
            hr = DeriveAppContainerSidFromAppContainerName(PackageMoniker.c_str(), &packageSid);
            if (FAILED(hr)) {
                Log(L"[ERROR] Failed to get SID: 0x%08X\n", hr);
                return HRESULT_CODE(hr);
            }
        } else {
            Log(L"[ERROR] CreateAppContainerProfile failed: 0x%08X\n", hr);
            return HRESULT_CODE(hr);
        }
    } else {
        g_profileCreatedThisRun = true;
        Log(L"[INFO] Created profile: %s\n", PackageMoniker.c_str());
    }

    *outSid = packageSid;
    return ERROR_SUCCESS;
}

DWORD DeleteProfileIfCreatedThisRun()
{
    if (RetainProfile || !g_profileCreatedThisRun)
        return ERROR_SUCCESS;

    HRESULT hr = DeleteAppContainerProfile(PackageMoniker.c_str());
    if (FAILED(hr)) {
        Log(L"[WARN] Failed to delete profile: 0x%08X\n", hr);
        return HRESULT_CODE(hr);
    }
    Log(L"[INFO] Profile deleted (created by this run)\n");
    return ERROR_SUCCESS;
}

// ============================================================================
// Launch AppContainer process (returns PROCESS_INFORMATION to caller)
// ============================================================================

DWORD LaunchProcess(PSID packageSid, PROCESS_INFORMATION& outPi)
{
    outPi = PROCESS_INFORMATION{};

    DWORD result = ERROR_SUCCESS;
    DWORD attrCount = 1;
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    SIZE_T attrListSize = 0;

    SECURITY_CAPABILITIES secCaps = {};
    DWORD lpacPolicy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;

    STARTUPINFOEX si = {};
    PROCESS_INFORMATION pi = {};

    if (LaunchAsLpac) attrCount++;

    InitializeProcThreadAttributeList(nullptr, attrCount, 0, &attrListSize);
    attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrListSize);
    if (!attrList)
        return ERROR_OUTOFMEMORY;

    if (!InitializeProcThreadAttributeList(attrList, attrCount, 0, &attrListSize)) {
        result = GetLastError();
        Log(L"[ERROR] InitializeProcThreadAttributeList failed: %d\n", result);
        HeapFree(GetProcessHeap(), 0, attrList);
        return result;
    }

    secCaps.CapabilityCount = static_cast<DWORD>(CapabilityList.size());
    secCaps.Capabilities = secCaps.CapabilityCount ? CapabilityList.data() : nullptr;
    secCaps.AppContainerSid = packageSid;

    if (!UpdateProcThreadAttribute(attrList, 0,
        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
        &secCaps, sizeof(secCaps), nullptr, nullptr))
    {
        result = GetLastError();
        Log(L"[ERROR] Failed to set SECURITY_CAPABILITIES: %d\n", result);
        DeleteProcThreadAttributeList(attrList);
        HeapFree(GetProcessHeap(), 0, attrList);
        return result;
    }

    if (LaunchAsLpac) {
        if (!UpdateProcThreadAttribute(attrList, 0,
            PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
            &lpacPolicy, sizeof(lpacPolicy), nullptr, nullptr))
        {
            result = GetLastError();
            Log(L"[ERROR] Failed to set LPAC policy: %d\n", result);
            DeleteProcThreadAttributeList(attrList);
            HeapFree(GetProcessHeap(), 0, attrList);
            return result;
        }
    }

    std::wstring cmdLine = L"\"" + ExeToLaunchStr + L"\"";
    if (!ExtraArgs.empty()) {
        cmdLine += L" ";
        cmdLine += ExtraArgs;
    }

    std::vector<WCHAR> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT
                | CREATE_NEW_CONSOLE
                | CREATE_UNICODE_ENVIRONMENT;

    LPCWSTR workDir = nullptr;
    if (!ProjectDirectory.empty()) workDir = ProjectDirectory.c_str();
    else workDir = WorkingDirectory.c_str();

    std::vector<WCHAR> envBlock;
    BuildSandboxEnvironmentBlock(envBlock);

    Log(L"[INFO] Launch cmd: %s\n", cmdLine.c_str());
    Log(L"[INFO] WorkDir  : %s\n", workDir ? workDir : L"(null)");

    if (!CreateProcessW(nullptr,
                        cmdBuf.data(),
                        nullptr, nullptr,
                        FALSE,
                        flags,
                        envBlock.data(),
                        workDir,
                        (LPSTARTUPINFOW)&si,
                        &pi))
    {
        result = GetLastError();
        Log(L"[ERROR] CreateProcess failed: %d\n", result);
    } else {
        outPi = pi; // caller will close handles
        Log(L"[SUCCESS] OpenCode launched, PID=%lu\n", pi.dwProcessId);
    }

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);
    return result;
}

// ============================================================================
// Cleanup helper INI IO
// ============================================================================

static bool IniWriteDword(const std::wstring& ini, const wchar_t* section, const wchar_t* key, DWORD v)
{
    wchar_t buf[32] = {};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%lu", (unsigned long)v);
    return WritePrivateProfileStringW(section, key, buf, ini.c_str()) != 0;
}

static bool IniWriteString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring& v)
{
    return WritePrivateProfileStringW(section, key, v.c_str(), ini.c_str()) != 0;
}

bool WriteCleanupIni(const std::wstring& iniPath,
                     DWORD opencodePid,
                     const std::wstring& exePathForHelper,
                     bool profileCreatedThisRun,
                     bool retainProfile,
                     const RegDwordBackup& lowBox,
                     bool bunMappingCreated,
                     const std::wstring& bunDeviceName,
                     const std::wstring& bunRawTarget,
                     const std::wstring& moniker,
                     const std::wstring& sidStr,
                     const std::vector<std::wstring>& grantedPaths,
                     const std::wstring& logPath)
{
    EnsureDirectoryRecursive(GetFullPathW(iniPath, L"").substr(0, GetFullPathW(iniPath, L"").find_last_of(L"\\/")));

    const wchar_t* sec = L"Cleanup";

    bool ok = true;
    ok &= IniWriteDword(iniPath, sec, L"Pid", opencodePid);
    ok &= IniWriteString(iniPath, sec, L"HelperExe", exePathForHelper);
    ok &= IniWriteString(iniPath, sec, L"Moniker", moniker);
    ok &= IniWriteString(iniPath, sec, L"Sid", sidStr);
    ok &= IniWriteString(iniPath, sec, L"LogPath", logPath);

    ok &= IniWriteDword(iniPath, sec, L"ProfileCreatedThisRun", profileCreatedThisRun ? 1 : 0);
    ok &= IniWriteDword(iniPath, sec, L"RetainProfile", retainProfile ? 1 : 0);

    ok &= IniWriteDword(iniPath, sec, L"LowBoxChanged", lowBox.changed ? 1 : 0);
    ok &= IniWriteDword(iniPath, sec, L"LowBoxExisted", lowBox.existed ? 1 : 0);
    ok &= IniWriteDword(iniPath, sec, L"LowBoxOldValue", lowBox.oldValue);

    ok &= IniWriteDword(iniPath, sec, L"BunMappingCreated", bunMappingCreated ? 1 : 0);
    ok &= IniWriteString(iniPath, sec, L"BunDeviceName", bunDeviceName);
    ok &= IniWriteString(iniPath, sec, L"BunRawTarget", bunRawTarget);

    ok &= IniWriteDword(iniPath, sec, L"PathCount", (DWORD)grantedPaths.size());
    for (size_t i = 0; i < grantedPaths.size(); i++) {
        wchar_t key[64] = {};
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"Path%zu", i);
        ok &= IniWriteString(iniPath, sec, key, grantedPaths[i]);
    }

    return ok;
}

static DWORD IniReadDword(const std::wstring& ini, const wchar_t* section, const wchar_t* key, DWORD defV)
{
    return (DWORD)GetPrivateProfileIntW(section, key, (int)defV, ini.c_str());
}

static std::wstring IniReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
{
    std::vector<WCHAR> buf(32768, L'\0');
    GetPrivateProfileStringW(section, key, L"", buf.data(), (DWORD)buf.size(), ini.c_str());
    return std::wstring(buf.data());
}

bool SpawnCleanupHelper(const std::wstring& helperExe, const std::wstring& iniPath)
{
    std::wstring cmd = L"\"" + helperExe + L"\" --cleanup \"" + iniPath + L"\"";
    std::vector<WCHAR> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

    if (!CreateProcessW(nullptr,
                        buf.data(),
                        nullptr, nullptr,
                        FALSE,
                        flags,
                        nullptr,
                        nullptr,
                        &si,
                        &pi))
    {
        DWORD err = GetLastError();
        Log(L"[ERROR] Failed to spawn cleanup helper (err=%d)\n", err);
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    Log(L"[INFO] Cleanup helper spawned\n");
    return true;
}

int RunCleanupMode(const std::wstring& iniPath)
{
    LogInitLockOnly(); // in cleanup mode, we might switch log later

    const wchar_t* sec = L"Cleanup";

    DWORD pid = IniReadDword(iniPath, sec, L"Pid", 0);
    std::wstring moniker = IniReadString(iniPath, sec, L"Moniker");
    std::wstring sidStr  = IniReadString(iniPath, sec, L"Sid");
    std::wstring logPath = IniReadString(iniPath, sec, L"LogPath");

    if (!logPath.empty())
        LogSwitchToFile(logPath);

    Log(L"[CLEANUP] Starting cleanup for PID=%lu\n", pid);

    HANDLE hProc = nullptr;
    if (pid != 0) {
        hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            Log(L"[CLEANUP] Waiting for opencode to exit...\n");
            WaitForSingleObject(hProc, INFINITE);
            CloseHandle(hProc);
        } else {
            Log(L"[CLEANUP] OpenProcess failed (err=%d). Continuing cleanup anyway.\n", GetLastError());
        }
    }

    // Restore B mapping (if created)
    bool bunCreated = IniReadDword(iniPath, sec, L"BunMappingCreated", 0) != 0;
    std::wstring bunDev = IniReadString(iniPath, sec, L"BunDeviceName");
    std::wstring bunRaw = IniReadString(iniPath, sec, L"BunRawTarget");

    if (bunCreated && !bunDev.empty() && !bunRaw.empty()) {
        DWORD flags = DDD_REMOVE_DEFINITION
                    | DDD_EXACT_MATCH_ON_REMOVE
                    | DDD_NO_BROADCAST_SYSTEM
                    | DDD_RAW_TARGET_PATH;

        if (!DefineDosDeviceW(flags, bunDev.c_str(), bunRaw.c_str())) {
            Log(L"[CLEANUP][WARN] Failed removing %s mapping (err=%d)\n", bunDev.c_str(), GetLastError());
        } else {
            Log(L"[CLEANUP] Removed %s mapping\n", bunDev.c_str());
        }
    }

    // Restore registry LowBoxConsoleEnabled (if changed)
    bool lowChanged = IniReadDword(iniPath, sec, L"LowBoxChanged", 0) != 0;
    bool lowExisted = IniReadDword(iniPath, sec, L"LowBoxExisted", 0) != 0;
    DWORD lowOldVal = IniReadDword(iniPath, sec, L"LowBoxOldValue", 0);

    if (lowChanged) {
        if (lowExisted) {
            DWORD v = lowOldVal;
            LSTATUS ls = RegSetKeyValueW(HKEY_CURRENT_USER,
                                         L"Console",
                                         L"LowBoxConsoleEnabled",
                                         REG_DWORD, &v, sizeof(v));
            if (ls == ERROR_SUCCESS)
                Log(L"[CLEANUP] Restored LowBoxConsoleEnabled=%lu\n", v);
            else
                Log(L"[CLEANUP][WARN] Failed restoring LowBoxConsoleEnabled (err=%ld)\n", ls);
        } else {
            LSTATUS ls = RegDeleteKeyValueW(HKEY_CURRENT_USER,
                                            L"Console",
                                            L"LowBoxConsoleEnabled");
            if (ls == ERROR_SUCCESS || ls == ERROR_FILE_NOT_FOUND)
                Log(L"[CLEANUP] Deleted LowBoxConsoleEnabled\n");
            else
                Log(L"[CLEANUP][WARN] Failed deleting LowBoxConsoleEnabled (err=%ld)\n", ls);
        }
    }

    // Revoke ACLs (remove AppContainer SID from granted paths)
    DWORD pathCount = IniReadDword(iniPath, sec, L"PathCount", 0);
    if (!sidStr.empty() && pathCount > 0) {
        Log(L"[CLEANUP] Revoking ACL grants (%lu paths)...\n", pathCount);
        for (DWORD i = 0; i < pathCount; i++) {
            wchar_t key[64] = {};
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"Path%lu", (unsigned long)i);
            std::wstring p = IniReadString(iniPath, sec, key);
            if (!p.empty()) {
                // best-effort revoke
                std::wstring cmd = L"icacls \"" + p + L"\" /remove *" + sidStr + L" /C /Q";
                if (kUseRecursiveAclPropagation)
                    cmd += L" /T";
                _wsystem(cmd.c_str());
            }
        }
        Log(L"[CLEANUP] ACL revoke completed (best-effort)\n");
    }

    // Delete profile if created by this run and not retained
    bool profileCreated = IniReadDword(iniPath, sec, L"ProfileCreatedThisRun", 0) != 0;
    bool retainProfile = IniReadDword(iniPath, sec, L"RetainProfile", 0) != 0;

    if (profileCreated && !retainProfile && !moniker.empty()) {
        HRESULT hr = DeleteAppContainerProfile(moniker.c_str());
        if (FAILED(hr))
            Log(L"[CLEANUP][WARN] Failed deleting profile (0x%08X)\n", hr);
        else
            Log(L"[CLEANUP] Profile deleted\n");
    }

    // Remove cleanup ini itself
    DeleteFileW(iniPath.c_str());
    Log(L"[CLEANUP] Done\n");

    LogClose();
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int wmain(int argc, WCHAR** argv)
{
    // Special cleanup mode: LaunchOpenCodeTUI.exe --cleanup "<iniPath>"
    if (argc >= 3 && wcscmp(argv[1], L"--cleanup") == 0) {
        return RunCleanupMode(argv[2]);
    }

    LogInitLockOnly();

    Log(L"==========================================\n");
    Log(L" OpenCode TUI - AppContainer Launcher\n");
    Log(L" Mode A (WorkRoot whitelist)\n");
    Log(L"==========================================\n\n");

    // Load config file first (lower priority than CLI)
    std::wstring configPath = GetExeDirectory() + L"\\config.ini";
    LoadConfigFile(configPath);

    // Parse CLI args
    if (!ParseArguments(argc, argv)) {
        LogClose();
        return 1;
    }

    // Validate required params
    if (PackageMoniker.empty() || ExeToLaunchStr.empty() || WorkingDirectory.empty()) {
        PrintUsage();
        Log(L"\n[ERROR] -m, -i, -p are required.\n");
        LogClose();
        return 1;
    }

    // Canonicalize paths
    ExeToLaunchStr = GetFullPathW(ExeToLaunchStr, GetExeDirectory());
    WorkingDirectory = GetFullPathW(WorkingDirectory, L"");

    if (ExeToLaunchStr.empty() || !FileExists(ExeToLaunchStr)) {
        Log(L"[ERROR] opencode.exe not found: %s\n", ExeToLaunchStr.c_str());
        LogClose();
        return 1;
    }

    if (WorkingDirectory.empty() || !DirectoryExists(WorkingDirectory)) {
        Log(L"[ERROR] WorkRoot does not exist: %s\n", WorkingDirectory.c_str());
        LogClose();
        return 1;
    }

    if (!ProjectDirectory.empty()) {
        ProjectDirectory = GetFullPathW(ProjectDirectory, WorkingDirectory);
        if (ProjectDirectory.empty() || !DirectoryExists(ProjectDirectory)) {
            Log(L"[ERROR] ProjectDir does not exist: %s\n", ProjectDirectory.c_str());
            LogClose();
            return 1;
        }
        if (!IsSubPathOf(ProjectDirectory, WorkingDirectory)) {
            Log(L"[ERROR] ProjectDir must be under WorkRoot.\n");
            Log(L"        WorkRoot : %s\n", WorkingDirectory.c_str());
            Log(L"        Project  : %s\n", ProjectDirectory.c_str());
            LogClose();
            return 1;
        }
    }

    if (SandboxDataRoot.empty())
        SandboxDataRoot = WorkingDirectory + L"\\.opencode-sandbox";
    SandboxDataRoot = GetFullPathW(SandboxDataRoot, WorkingDirectory);

    if (SandboxDataRoot.empty() || !IsSubPathOf(SandboxDataRoot, WorkingDirectory)) {
        Log(L"[ERROR] SandboxDataRoot must be under WorkRoot.\n");
        Log(L"        WorkRoot : %s\n", WorkingDirectory.c_str());
        Log(L"        Sandbox  : %s\n", SandboxDataRoot.c_str());
        LogClose();
        return 1;
    }

    // Canonicalize tool paths
    for (auto& p : AdditionalGrantPaths)
        p = GetFullPathW(p, L"");

    // Prepare sandbox dirs
    PrepareSandboxDirectories();

    // Switch log to sandbox log file (avoid writing logs outside WorkRoot)
    std::wstring sandboxLogPath = g_sandboxRoot + L"\\launcher.log";
    if (LogSwitchToFile(sandboxLogPath))
        Log(L"[INFO] Log file: %s\n", sandboxLogPath.c_str());

    Log(L"[CONFIG] Moniker:    %s\n", PackageMoniker.c_str());
    Log(L"[CONFIG] ExePath:    %s\n", ExeToLaunchStr.c_str());
    Log(L"[CONFIG] WorkRoot:   %s\n", WorkingDirectory.c_str());
    Log(L"[CONFIG] ProjectDir: %s\n", ProjectDirectory.empty() ? L"(not set)" : ProjectDirectory.c_str());
    Log(L"[CONFIG] SandboxDir: %s\n\n", SandboxDataRoot.c_str());

    // Capabilities
    if (!SkipDefaultCapabilities) {
        Log(L"[INFO] Adding default capabilities:\n");
        if (!AddDefaultCapabilities()) {
            LogClose();
            return 1;
        }
    }
    if (!ExtraCapabilitiesStr.empty()) {
        std::vector<WCHAR> capBuf(ExtraCapabilitiesStr.begin(), ExtraCapabilitiesStr.end());
        capBuf.push_back(L'\0');
        ParseCapabilityList(capBuf.data());
    }
    Log(L"[INFO] Total capabilities: %zu\n\n", CapabilityList.size());

    // Create AppContainer profile (track if created)
    PSID appContainerSid = nullptr;
    DWORD r = CreateProfile(&appContainerSid);
    if (r != ERROR_SUCCESS) {
        LogClose();
        return (int)r;
    }
    g_appContainerSidStr = SidToString(appContainerSid);
    Log(L"[INFO] AppContainer SID: %s\n", g_appContainerSidStr.c_str());

    // Apply minimal ACL grants (remember paths for later revoke)
    Log(L"[INFO] Granting filesystem access (temporary; will revoke on exit)...\n");

    // opencode.exe dir -> RX
    {
        size_t pos = ExeToLaunchStr.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring exeDir = ExeToLaunchStr.substr(0, pos);
            GrantPath(exeDir, g_appContainerSidStr, kPermRX);
        }
    }

    // WorkRoot -> RW
    GrantPath(WorkingDirectory, g_appContainerSidStr, kPermRW);

    // Sandbox root -> RW
    GrantPath(g_sandboxRoot, g_appContainerSidStr, kPermRW);
    GrantPath(g_dirConfig,   g_appContainerSidStr, kPermRW);
    GrantPath(g_dirData,     g_appContainerSidStr, kPermRW);
    GrantPath(g_dirTemp,     g_appContainerSidStr, kPermRW);
    GrantPath(g_dirHome,     g_appContainerSidStr, kPermRW);
    GrantPath(g_dirCache,    g_appContainerSidStr, kPermRW);
    GrantPath(g_dirBunDrive, g_appContainerSidStr, kPermRW);
    GrantPath(g_dirBunRoot,  g_appContainerSidStr, kPermRW);

    // Tools -> RX
    for (const auto& tp : AdditionalGrantPaths)
        if (!tp.empty())
            GrantPath(tp, g_appContainerSidStr, kPermRX);

    // TEMPORARY host changes needed for AppContainer console + Bun/OpenTUI
    EnableLowBoxConsoleTemp();

    if (!EnsureBunDriveMapping()) {
        Log(L"[ERROR] Bun/OpenTUI drive mapping failed. Rolling back...\n");
        RestoreLowBoxConsole();

        // revoke ACL
        for (const auto& p : g_grantedPaths)
            RevokePath(p, g_appContainerSidStr);

        RemoveBunDriveMappingIfCreated();
        DeleteProfileIfCreatedThisRun();

        if (appContainerSid) FreeSid(appContainerSid);
        LogClose();
        return 1;
    }

    // Launch opencode
    PROCESS_INFORMATION pi = {};
    DWORD launchErr = LaunchProcess(appContainerSid, pi);
    if (appContainerSid) FreeSid(appContainerSid);

    if (launchErr != ERROR_SUCCESS) {
        Log(L"[ERROR] Launch failed. Rolling back...\n");

        // rollback in-process since no child running
        RemoveBunDriveMappingIfCreated();
        RestoreLowBoxConsole();

        for (const auto& p : g_grantedPaths)
            RevokePath(p, g_appContainerSidStr);

        DeleteProfileIfCreatedThisRun();

        LogClose();
        return (int)launchErr;
    }

    // If -w: wait here, then cleanup in the same process.
    // Otherwise: spawn cleanup helper so terminal is not blocked.
    DWORD pid = pi.dwProcessId;

    if (WaitForExit) {
        Log(L"[INFO] Waiting for opencode to exit (in-process cleanup enabled)...\n");
        WaitForSingleObject(pi.hProcess, INFINITE);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // rollback
        RemoveBunDriveMappingIfCreated();
        RestoreLowBoxConsole();

        for (const auto& p : g_grantedPaths)
            RevokePath(p, g_appContainerSidStr);

        DeleteProfileIfCreatedThisRun();

        Log(L"[INFO] Cleanup complete. Exiting.\n");
        LogClose();
        return 0;
    }

    // Not waiting: close handles now, spawn helper for cleanup
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Write cleanup.ini inside sandbox, then spawn helper
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);

    g_cleanupIniPath = g_sandboxRoot + L"\\cleanup.ini";

    bool wrote = WriteCleanupIni(g_cleanupIniPath,
                                pid,
                                selfPath,
                                g_profileCreatedThisRun,
                                RetainProfile,
                                g_lowBoxBackup,
                                g_bunMappingCreated,
                                g_bunDeviceName,
                                g_bunRawTarget,
                                PackageMoniker,
                                g_appContainerSidStr,
                                g_grantedPaths,
                                sandboxLogPath);

    if (!wrote) {
        Log(L"[ERROR] Failed to write cleanup.ini; fallback to in-process wait.\n");
        // fallback safety: wait + cleanup
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }

        RemoveBunDriveMappingIfCreated();
        RestoreLowBoxConsole();

        for (const auto& p : g_grantedPaths)
            RevokePath(p, g_appContainerSidStr);

        DeleteProfileIfCreatedThisRun();

        LogClose();
        return 0;
    }

    if (!SpawnCleanupHelper(selfPath, g_cleanupIniPath)) {
        Log(L"[ERROR] Failed to spawn cleanup helper; fallback to in-process wait.\n");
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }

        RemoveBunDriveMappingIfCreated();
        RestoreLowBoxConsole();

        for (const auto& p : g_grantedPaths)
            RevokePath(p, g_appContainerSidStr);

        DeleteProfileIfCreatedThisRun();
    }

    Log(L"[INFO] Launcher exiting. Cleanup will happen automatically after opencode exits.\n");
    LogClose();
    return 0;
}
