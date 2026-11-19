/*
 * RamQuill  -  Limpador de memória RAM + desativador do SysMain (Windows 10/11)
 * Port em C puro (C17) com Win32 + NT Native API. Sem dependências externas.
 *
 * Arquitetura (resumo):
 *   - Privilégios .... advapi32: OpenProcessToken / LookupPrivilegeValue / AdjustTokenPrivileges
 *   - Limpeza ........ ntdll!NtSetSystemInformation (carregada via GetProcAddress):
 *                        SystemMemoryListInformation (80): EmptyWorkingSets, FlushModifiedList,
 *                                                          PurgeStandbyList, PurgeLowPriorityStandbyList
 *                        SystemRegistryReconciliationInformation (155)
 *                        SystemCombinePhysicalMemoryInformation  (130)
 *                      + SetSystemFileCacheSize (cache de arquivos) + flush dos volumes
 *                      + plano B por processo: EnumProcesses / EmptyWorkingSet (ignora processos críticos)
 *   - SysMain ........ Service Control Manager (OpenService / ChangeServiceConfig / ControlService)
 *   - Interface ...... Win32 + GDI, desenhada em um único DIB de 32 bits (anel/botões com
 *                      antialiasing por software). Sem controles filhos, sem GDI+, sem Nuklear:
 *                      menos DLLs, menos RAM. Timers só rodam quando há algo para animar.
 *
 * A ordem e a lista de "áreas" de memória seguem a técnica pública usada por utilitários como
 * o Mem Reduct (GPLv3). Este arquivo é uma implementação original: nenhum trecho de código
 * do Mem Reduct foi copiado.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <psapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* ------------------------------------------------------------------------- */
/* Tipos / constantes do NT que os headers do MinGW não trazem               */
/* ------------------------------------------------------------------------- */

typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)

typedef NTSTATUS (NTAPI *NtSetSystemInformation_t)(ULONG InfoClass, PVOID Info, ULONG Length);
typedef BOOL (WINAPI *SetSystemFileCacheSize_t)(SIZE_T Min, SIZE_T Max, DWORD Flags);

#define SYS_INFO_MEMORY_LIST        80   /* SystemMemoryListInformation               */
#define SYS_INFO_COMBINE_PHYS_MEM  130   /* SystemCombinePhysicalMemoryInformation    */
#define SYS_INFO_REGISTRY_RECONCILE 155  /* SystemRegistryReconciliationInformation   */

#define MEM_CMD_EMPTY_WORKING_SETS  2    /* MemoryEmptyWorkingSets                    */
#define MEM_CMD_FLUSH_MODIFIED      3    /* MemoryFlushModifiedList                   */
#define MEM_CMD_PURGE_STANDBY       4    /* MemoryPurgeStandbyList                    */
#define MEM_CMD_PURGE_LOW_STANDBY   5    /* MemoryPurgeLowPriorityStandbyList         */

typedef struct {                          /* MEMORY_COMBINE_INFORMATION_EX */
    HANDLE    Handle;
    ULONG_PTR PagesCombined;
    ULONG     Flags;
} MEMORY_COMBINE_INFORMATION_EX_;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_BORDER_COLOR_  34
#define DWMWA_CAPTION_COLOR_ 35
#define DWMWA_TEXT_COLOR_    36

/* Áreas de memória que podem ser limpas (máscara de bits) */
#define AREA_WORKING_SET   0x01u
#define AREA_FILE_CACHE    0x02u
#define AREA_STANDBY_LOW   0x04u
#define AREA_STANDBY       0x08u   /* "profunda": pode causar micro-travadas */
#define AREA_MODIFIED      0x10u   /* "profunda": grava páginas sujas no disco */
#define AREA_COMBINE       0x20u
#define AREA_REGISTRY      0x40u
#define AREA_VOLUME_FLUSH  0x80u
#define AREA_DEFAULT (AREA_WORKING_SET | AREA_FILE_CACHE | AREA_STANDBY_LOW | \
                      AREA_REGISTRY | AREA_COMBINE | AREA_VOLUME_FLUSH)
#define AREA_DEEP    (AREA_DEFAULT | AREA_MODIFIED | AREA_STANDBY)

/* ------------------------------------------------------------------------- */
/* Configurações gerais                                                      */
/* ------------------------------------------------------------------------- */

#define WND_CLASS        L"RamQuillWnd"
#define APP_NAME         L"RamQuill"
#define LOG_MAX          32
#define LOG_LEN          200
#define LOG_VISIBLE      7
#define AUTO_COOLDOWN_MS 60000ULL

#define WM_APP_LOG   (WM_APP + 1)
#define WM_APP_DONE  (WM_APP + 2)
#define WM_APP_SHOW  (WM_APP + 3)
#define WM_TRAYICON  (WM_APP + 4)

#define TIMER_ANIM 1
#define TIMER_TICK 2

#define IDM_OPEN  1001
#define IDM_CLEAN 1002
#define IDM_EXIT  1003

enum { JOB_CLEAN = 1, JOB_SYSMAIN = 2 };
typedef enum { SM_UNKNOWN, SM_RUNNING, SM_STOPPED, SM_DISABLED, SM_ABSENT } SmState;

typedef struct {
    ULONG  load;       /* % de uso (dwMemoryLoad)        */
    double totalMB;
    double availMB;
    double cacheMB;    /* cache do sistema (GetPerformanceInfo) */
} MemInfo;

/* ------------------------------------------------------------------------- */
/* Estado global                                                             */
/* ------------------------------------------------------------------------- */

static HINSTANCE g_hInst;
static HWND      g_hwnd;
static NtSetSystemInformation_t g_NtSet;
static BOOL      g_privProfile, g_privQuota;

static volatile LONG g_busy;           /* 1 = limpeza em andamento (thread) */
static BOOL    g_auto, g_deep;
static int     g_threshold = 85;
static ULONGLONG g_lastAuto;
static wchar_t g_iniPath[MAX_PATH];

static MemInfo g_mem;
static SmState g_sm = SM_UNKNOWN;
static float   g_target, g_shown;      /* % alvo e % exibido (animado)      */
static BOOL    g_animOn, g_tracking, g_needRedraw;
static int     g_hover = -1;
static ULONG   g_lastTrayLoad = 0xFFFFFFFF;

static wchar_t g_log[LOG_MAX][LOG_LEN];
static int     g_logN;

/* Escala de DPI e layout */
static float g_scale = 1.0f;
static int   g_W, g_H;
static RECT  g_rBtn[3], g_rTog[2];

/* Back buffer (DIB 32 bits, top-down) */
static HDC      g_memDC;
static HBITMAP  g_dib, g_oldBmp;
static uint32_t *g_px;

static HFONT g_fTitle, g_fSub, g_fBig, g_fUI, g_fSmall, g_fLog;
static NOTIFYICONDATAW g_nid;
static HICON g_icon;
static BOOL  g_iconOwned;
static UINT  g_msgTaskbarCreated;

/* Cores 0xRRGGBB */
#define C_BG      0x05070du
#define C_PANEL   0x0b1220u
#define C_BLUE    0x00b4ffu
#define C_BLUE_DK 0x0a5c99u
#define C_TRACK   0x0d1a2bu
#define C_TEXT    0xd8ecffu
#define C_MUTED   0x6f8aa6u
#define C_WARN    0xff4d6du
#define C_ARC_A   0x0a5cffu
#define C_ARC_B   0x00e0ffu
#define C_RING_IN 0x0a2640u

static int   S(int v)    { return (int)(v * g_scale + 0.5f); }
static float SF(float v) { return v * g_scale; }

/* ------------------------------------------------------------------------- */
/* Log (qualquer thread -> janela)                                           */
/* ------------------------------------------------------------------------- */

static void LogF(const wchar_t *fmt, ...)
{
    wchar_t body[LOG_LEN], line[LOG_LEN + 16];
    va_list ap;
    SYSTEMTIME t;

    va_start(ap, fmt);
    _vsnwprintf(body, LOG_LEN, fmt, ap);
    va_end(ap);
    body[LOG_LEN - 1] = 0;

    GetLocalTime(&t);
    _snwprintf(line, LOG_LEN + 16, L"[%02u:%02u:%02u] %ls",
               (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond, body);
    line[LOG_LEN + 15] = 0;

    wchar_t *dup = _wcsdup(line);
    if (dup && (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)dup)))
        free(dup);
}

/* ------------------------------------------------------------------------- */
/* Privilégios e elevação (advapi32)                                         */
/* ------------------------------------------------------------------------- */

static BOOL IsElevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION te;
    DWORD len = 0;
    BOOL ok = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof te, &len))
            ok = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return ok;
}

/* Habilita um privilégio no token do processo. Retorna FALSE se não foi concedido
 * (AdjustTokenPrivileges retorna TRUE mesmo com ERROR_NOT_ALL_ASSIGNED). */
static BOOL EnablePrivilege(LPCWSTR name)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    DWORD err;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return FALSE;

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, NULL, NULL);
    err = GetLastError();
    CloseHandle(tok);
    return err == ERROR_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Memória                                                                   */
/* ------------------------------------------------------------------------- */

static MemInfo ReadMem(void)
{
    MemInfo m = {0};
    MEMORYSTATUSEX st;
    PERFORMANCE_INFORMATION pi;

    st.dwLength = sizeof st;
    if (GlobalMemoryStatusEx(&st)) {
        m.load    = st.dwMemoryLoad;
        m.totalMB = (double)st.ullTotalPhys / 1048576.0;
        m.availMB = (double)st.ullAvailPhys / 1048576.0;
    }
    pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi))
        m.cacheMB = (double)pi.SystemCache * (double)pi.PageSize / 1048576.0;
    return m;
}

static NTSTATUS NtSetSys(ULONG cls, PVOID buf, ULONG len)
{
    return g_NtSet ? g_NtSet(cls, buf, len) : STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS MemCmd(ULONG cmd)
{
    return NtSetSys(SYS_INFO_MEMORY_LIST, &cmd, sizeof cmd);
}

static const wchar_t *NtText(NTSTATUS s)
{
    switch ((ULONG)s) {
    case 0xC0000061: return L"privilégio não concedido";
    case 0xC0000022: return L"acesso negado";
    case 0xC0000002:
    case 0xC00000BB: return L"não suportado nesta versão do Windows";
    case 0xC000000D: return L"parâmetro inválido";
    default:         return L"erro";
    }
}

static void Step(const wchar_t *name, NTSTATUS st)
{
    if (NT_SUCCESS(st))
        LogF(L"[ok] %ls", name);
    else
        LogF(L"[falhou] %ls: %ls (0x%08lX)", name, NtText(st), (unsigned long)st);
}

/* --- Plano B: reduzir processo a processo, ignorando os críticos --- */

static const wchar_t *const k_critical[] = {
    L"csrss.exe", L"smss.exe", L"wininit.exe", L"services.exe",
    L"lsass.exe", L"winlogon.exe", L"System", L"Registry"
};

static BOOL IsCriticalProcess(DWORD pid, HANDLE h)
{
    wchar_t path[MAX_PATH];
    const wchar_t *base;
    size_t i;

    if (pid <= 4) return TRUE;
    if (!GetProcessImageFileNameW(h, path, MAX_PATH)) return TRUE; /* sem nome: não mexe */
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    for (i = 0; i < sizeof k_critical / sizeof k_critical[0]; i++)
        if (_wcsicmp(base, k_critical[i]) == 0) return TRUE;
    return FALSE;
}

static int TrimEachProcess(void)
{
    DWORD cap = 1024, bytes = 0, count, i;
    DWORD *pids = NULL;
    int n = 0;

    EnablePrivilege(SE_DEBUG_NAME);

    for (;;) {                              /* cresce o buffer até caber tudo */
        DWORD *np = (DWORD *)realloc(pids, cap * sizeof(DWORD));
        if (!np) { free(pids); return 0; }
        pids = np;
        if (!EnumProcesses(pids, cap * (DWORD)sizeof(DWORD), &bytes)) { free(pids); return 0; }
        if (bytes < cap * sizeof(DWORD)) break;
        cap *= 2;
    }

    count = bytes / (DWORD)sizeof(DWORD);
    for (i = 0; i < count; i++) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, pids[i]);
        if (!h) continue;
        if (!IsCriticalProcess(pids[i], h) && EmptyWorkingSet(h)) n++;
        CloseHandle(h);
    }
    free(pids);
    return n;
}

/* --- Flush do cache de escrita dos volumes fixos --- */
static int FlushFixedVolumes(void)
{
    DWORD drives = GetLogicalDrives();
    int i, n = 0;

    for (i = 0; i < 26; i++) {
        wchar_t root[4], dev[8];
        HANDLE h;
        if (!(drives & (1u << i))) continue;
        root[0] = (wchar_t)(L'A' + i); root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        _snwprintf(dev, 8, L"\\\\.\\%lc:", (wint_t)(L'A' + i));
        dev[7] = 0;
        h = CreateFileW(dev, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (FlushFileBuffers(h)) n++;
            CloseHandle(h);
        }
    }
    return n;
}

/* --- Limpeza principal. Ordem: working set -> cache de arquivos -> volumes ->
 *     modified list -> standby -> standby prioridade 0 -> registro -> combinar páginas.
 *     (modified antes de standby: o que foi gravado no disco já pode ser descartado) --- */
static void DoClean(DWORD mask)
{
    MemInfo before = ReadMem(), after;

    LogF(L"Iniciando limpeza %ls...", (mask & AREA_STANDBY) ? L"profunda" : L"padrão");
    if (!g_privProfile) LogF(L"[aviso] SeProfileSingleProcessPrivilege não habilitado");
    if (!g_privQuota)   LogF(L"[aviso] SeIncreaseQuotaPrivilege não habilitado");

    if (mask & AREA_WORKING_SET) {
        NTSTATUS st = MemCmd(MEM_CMD_EMPTY_WORKING_SETS);
        Step(L"Working set de todos os processos", st);
        if (!NT_SUCCESS(st))
            LogF(L"Plano B: %d processos reduzidos individualmente", TrimEachProcess());
    }

    if (mask & AREA_FILE_CACHE) {
        SetSystemFileCacheSize_t fn = (SetSystemFileCacheSize_t)(void (*)(void))
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetSystemFileCacheSize");
        if (fn && fn((SIZE_T)-1, (SIZE_T)-1, 0))
            LogF(L"[ok] Cache de arquivos do sistema");
        else
            LogF(L"[falhou] Cache de arquivos do sistema (erro %lu)", (unsigned long)GetLastError());
    }

    if (mask & AREA_VOLUME_FLUSH)
        LogF(L"[ok] Cache de escrita dos volumes (%d volume(s))", FlushFixedVolumes());

    if (mask & AREA_MODIFIED)
        Step(L"Lista de páginas modificadas", MemCmd(MEM_CMD_FLUSH_MODIFIED));

    if (mask & AREA_STANDBY)
        Step(L"Lista standby", MemCmd(MEM_CMD_PURGE_STANDBY));

    if (mask & AREA_STANDBY_LOW)
        Step(L"Standby de baixa prioridade", MemCmd(MEM_CMD_PURGE_LOW_STANDBY));

    if (mask & AREA_REGISTRY)
        Step(L"Cache do registro", NtSetSys(SYS_INFO_REGISTRY_RECONCILE, NULL, 0));

    if (mask & AREA_COMBINE) {
        MEMORY_COMBINE_INFORMATION_EX_ ci = {0};
        Step(L"Combinação de páginas", NtSetSys(SYS_INFO_COMBINE_PHYS_MEM, &ci, sizeof ci));
    }

    after = ReadMem();
    LogF(L"Concluído: livre %.0f -> %.0f MB (%+.0f MB), uso %lu%% -> %lu%%",
         before.availMB, after.availMB, after.availMB - before.availMB,
         (unsigned long)before.load, (unsigned long)after.load);
}

/* ------------------------------------------------------------------------- */
/* SysMain (Service Control Manager)                                         */
/* ------------------------------------------------------------------------- */

static SmState SysMainQuery(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS_PROCESS ssp;
    DWORD need = 0, sz = 0;
    BOOL running = FALSE, disabled = FALSE;
    SmState st;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return SM_UNKNOWN;

    svc = OpenServiceW(scm, L"SysMain", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        return e == ERROR_SERVICE_DOES_NOT_EXIST ? SM_ABSENT : SM_UNKNOWN;
    }

    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof ssp, &need))
        running = ssp.dwCurrentState != SERVICE_STOPPED;

    QueryServiceConfigW(svc, NULL, 0, &sz);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && sz) {
        QUERY_SERVICE_CONFIGW *cfg = (QUERY_SERVICE_CONFIGW *)malloc(sz);
        if (cfg) {
            if (QueryServiceConfigW(svc, cfg, sz, &sz))
                disabled = cfg->dwStartType == SERVICE_DISABLED;
            free(cfg);
        }
    }

    st = running ? SM_RUNNING : (disabled ? SM_DISABLED : SM_STOPPED);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return st;
}

static void DoDisableSysMain(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    SERVICE_STATUS_PROCESS ssp;
    DWORD need, e;
    int i;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { LogF(L"[falhou] SysMain: não abriu o gerenciador de serviços (erro %lu)",
                     (unsigned long)GetLastError()); return; }

    svc = OpenServiceW(scm, L"SysMain",
                       SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (!svc) {
        e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) LogF(L"SysMain não existe neste Windows. Nada a fazer.");
        else LogF(L"[falhou] SysMain: OpenService (erro %lu)", (unsigned long)e);
        CloseServiceHandle(scm);
        return;
    }

    /* 1) desativa a inicialização; 2) para o serviço se estiver rodando */
    if (ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL))
        LogF(L"[ok] SysMain configurado como Desativado");
    else
        LogF(L"[falhou] SysMain: ChangeServiceConfig (erro %lu)", (unsigned long)GetLastError());

    if (ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
        for (i = 0; i < 50; i++) {                       /* espera até ~10 s */
            if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof ssp, &need)
                || ssp.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
        }
        LogF(L"[ok] SysMain parado");
    } else {
        e = GetLastError();
        if (e == ERROR_SERVICE_NOT_ACTIVE) LogF(L"SysMain já estava parado");
        else LogF(L"[falhou] SysMain: parar serviço (erro %lu)", (unsigned long)e);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

/* ------------------------------------------------------------------------- */
/* Thread de trabalho                                                        */
/* ------------------------------------------------------------------------- */

static DWORD WINAPI JobThread(LPVOID p)
{
    UINT_PTR a = (UINT_PTR)p;
    int   job  = (int)(a & 0xFF);
    DWORD mask = (DWORD)(a >> 8);

    if (job & JOB_SYSMAIN) DoDisableSysMain();
    if (job & JOB_CLEAN)   DoClean(mask);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void Redraw(void);

static void StartJob(int job)
{
    HANDLE th;
    UINT_PTR arg;

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;   /* já ocupado */
    arg = (UINT_PTR)job | ((UINT_PTR)(g_deep ? AREA_DEEP : AREA_DEFAULT) << 8);
    th = CreateThread(NULL, 0, JobThread, (LPVOID)arg, 0, NULL);
    if (!th) {
        InterlockedExchange(&g_busy, 0);
        LogF(L"[falhou] não foi possível criar a thread (erro %lu)", (unsigned long)GetLastError());
        return;
    }
    CloseHandle(th);                                              /* sem vazamento de handle */
    Redraw();
}

/* ------------------------------------------------------------------------- */
/* Configurações (INI em %APPDATA%\RamQuill)                                 */
/* ------------------------------------------------------------------------- */

static void InitIni(void)
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    g_iniPath[0] = 0;
    if (n && n < MAX_PATH - 32) {
        wcscat(dir, L"\\RamQuill");
        CreateDirectoryW(dir, NULL);
        _snwprintf(g_iniPath, MAX_PATH, L"%ls\\RamQuill.ini", dir);
        g_iniPath[MAX_PATH - 1] = 0;
    }
}

static void LoadCfg(void)
{
    if (!g_iniPath[0]) return;
    g_auto = GetPrivateProfileIntW(L"RamQuill", L"AutoClean", 0, g_iniPath) != 0;
    g_deep = GetPrivateProfileIntW(L"RamQuill", L"DeepClean", 0, g_iniPath) != 0;
    g_threshold = (int)GetPrivateProfileIntW(L"RamQuill", L"Threshold", 85, g_iniPath);
    if (g_threshold < 50) g_threshold = 50;
    if (g_threshold > 99) g_threshold = 99;
}

static void SaveCfg(void)
{
    wchar_t v[16];
    if (!g_iniPath[0]) return;
    WritePrivateProfileStringW(L"RamQuill", L"AutoClean", g_auto ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"RamQuill", L"DeepClean", g_deep ? L"1" : L"0", g_iniPath);
    _snwprintf(v, 16, L"%d", g_threshold);
    WritePrivateProfileStringW(L"RamQuill", L"Threshold", v, g_iniPath);
}

/* ------------------------------------------------------------------------- */
/* Rasterizador 2D com antialiasing (anel, retângulos arredondados)          */
/* ------------------------------------------------------------------------- */

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline void put(int x, int y, uint32_t col, float a)
{
    uint32_t *p, d;
    int dr, dg, db, sr, sg, sb, r, g, b;

    if (a <= 0.004f || x < 0 || y < 0 || x >= g_W || y >= g_H) return;
    p = &g_px[(size_t)y * (size_t)g_W + (size_t)x];
    d = *p;
    dr = (int)((d >> 16) & 255); dg = (int)((d >> 8) & 255); db = (int)(d & 255);
    sr = (int)((col >> 16) & 255); sg = (int)((col >> 8) & 255); sb = (int)(col & 255);
    r = (int)((float)dr + (float)(sr - dr) * a + 0.5f);
    g = (int)((float)dg + (float)(sg - dg) * a + 0.5f);
    b = (int)((float)db + (float)(sb - db) * a + 0.5f);
    *p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t lerpc(uint32_t a, uint32_t b, float t)
{
    int ar = (int)((a >> 16) & 255), ag = (int)((a >> 8) & 255), ab = (int)(a & 255);
    int br = (int)((b >> 16) & 255), bg = (int)((b >> 8) & 255), bb = (int)(b & 255);
    int r = (int)((float)ar + (float)(br - ar) * t + 0.5f);
    int g = (int)((float)ag + (float)(bg - ag) * t + 0.5f);
    int bl = (int)((float)ab + (float)(bb - ab) * t + 0.5f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* Retângulo arredondado (SDF) com borda suave. r = raio dos cantos. */
static void rrect(float x, float y, float w, float h, float r, uint32_t col, float alpha)
{
    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
    int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float hx = w * 0.5f - r, hy = h * 0.5f - r;
    int px, py;

    if (r < 0.0f) r = 0.0f;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > g_H) y1 = g_H;

    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            float qx = fabsf((float)px + 0.5f - cx) - hx;
            float qy = fabsf((float)py + 0.5f - cy) - hy;
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
            put(px, py, col, clamp01(0.5f - d) * alpha);
        }
    }
}

static void rrect_b(float x, float y, float w, float h, float r,
                    uint32_t fill, uint32_t border, float bw)
{
    rrect(x, y, w, h, r, border, 1.0f);
    rrect(x + bw, y + bw, w - 2.0f * bw, h - 2.0f * bw, r - bw, fill, 1.0f);
}

/* Anel de progresso circular: trilho + brilho (glow) + arco em gradiente com pontas redondas.
 * prog = 0..1, começa no topo e gira no sentido horário. */
static void DrawRing(float cx, float cy, float R, float W, float prog)
{
    const float TWO_PI = 6.2831853f;
    const float half = W * 0.5f, glowR = SF(16.0f), Ri = R - SF(26.0f);
    const float ext = R + half + glowR + 2.0f;
    float endAng, c1x, c1y;
    int x0 = (int)(cx - ext), x1 = (int)(cx + ext) + 1;
    int y0 = (int)(cy - ext), y1 = (int)(cy + ext) + 1;
    int x, y;
    BOOL has = prog > 0.002f;

    if (prog > 1.0f) prog = 1.0f;
    endAng = prog * TWO_PI;
    c1x = cx + R * sinf(endAng);
    c1y = cy - R * cosf(endAng);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > g_H) y1 = g_H;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float off = fabsf(d - R);
            float cov, ci;

            ci = clamp01(0.5f + SF(0.6f) - fabsf(d - Ri));     /* anel interno decorativo */
            if (ci > 0.0f) put(x, y, C_RING_IN, ci);

            if (off > half + glowR) continue;

            cov = clamp01(half - off + 0.5f);                  /* trilho */
            if (cov > 0.0f) put(x, y, C_TRACK, cov);
            if (!has) continue;

            {
                float a = atan2f(dx, -dy), d0, d1, dShape, glow, covA, u;
                float tx1 = (float)x + 0.5f - c1x, ty1 = (float)y + 0.5f - c1y;
                float tx0 = dx, ty0 = (float)y + 0.5f - (cy - R);
                BOOL inArc;

                if (a < 0.0f) a += TWO_PI;
                inArc = a <= endAng;
                d0 = sqrtf(tx0 * tx0 + ty0 * ty0);
                d1 = sqrtf(tx1 * tx1 + ty1 * ty1);

                /* distância até a forma do arco (para o brilho) */
                if (inArc) dShape = fmaxf(off - half, 0.0f);
                else       dShape = fmaxf(fminf(d0, d1) - half, 0.0f);
                if (dShape > glowR) continue;

                glow = 0.55f * expf(-dShape / SF(7.0f));
                put(x, y, C_BLUE, glow * (1.0f - clamp01(cov)) * 0.9f + glow * 0.15f);

                /* cobertura do arco = faixa angular + duas pontas redondas */
                covA = inArc ? cov : 0.0f;
                covA = fmaxf(covA, clamp01(half - d0 + 0.5f));
                covA = fmaxf(covA, clamp01(half - d1 + 0.5f));
                if (covA > 0.0f) {
                    if (inArc) u = a / endAng;
                    else       u = (d1 < d0) ? 1.0f : 0.0f;
                    put(x, y, lerpc(C_ARC_A, C_ARC_B, clamp01(u)), covA);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Texto (GDI sobre o mesmo DIB)                                             */
/* ------------------------------------------------------------------------- */

static COLORREF CR(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

static void Txt(HFONT f, uint32_t col, const wchar_t *s, int l, int t, int r, int b, UINT fl)
{
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    SelectObject(g_memDC, f);
    SetTextColor(g_memDC, CR(col));
    DrawTextW(g_memDC, s, -1, &rc, fl | DT_NOPREFIX);
}

static HFONT MakeFont(const wchar_t *face, int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

/* ------------------------------------------------------------------------- */
/* Layout e renderização                                                     */
/* ------------------------------------------------------------------------- */

static void BuildLayout(void)
{
    static const int bw = 132, gap = 8, bx = 24;
    int i;
    for (i = 0; i < 3; i++) {
        g_rBtn[i].left   = S(bx + i * (bw + gap));
        g_rBtn[i].right  = g_rBtn[i].left + S(bw);
        g_rBtn[i].top    = S(404);
        g_rBtn[i].bottom = S(446);
    }
    for (i = 0; i < 2; i++) {
        g_rTog[i].left   = S(24);
        g_rTog[i].right  = S(436);
        g_rTog[i].top    = S(460 + i * 30);
        g_rTog[i].bottom = S(488 + i * 30);
    }
}

static int HitTest(int x, int y)
{
    POINT p;
    int i;
    p.x = x; p.y = y;
    for (i = 0; i < 3; i++) if (PtInRect(&g_rBtn[i], p)) return i;
    for (i = 0; i < 2; i++) if (PtInRect(&g_rTog[i], p)) return 3 + i;
    return -1;
}

static void DrawToggle(const RECT *r, BOOL on, BOOL hover)
{
    float pw = SF(40), ph = SF(20);
    float px = (float)r->left, py = (float)r->top + ((float)(r->bottom - r->top) - ph) * 0.5f;
    float kr = ph * 0.5f - SF(3);
    float kx = on ? px + pw - ph * 0.5f : px + ph * 0.5f;

    rrect_b(px, py, pw, ph, ph * 0.5f, on ? C_BLUE_DK : C_PANEL,
            hover ? C_BLUE : C_BLUE_DK, SF(1.3f));
    rrect(kx - kr, py + ph * 0.5f - kr, kr * 2.0f, kr * 2.0f, kr, on ? C_BLUE : C_MUTED, 1.0f);
}

static void Render(void)
{
    static const wchar_t *const btn[3] = { L"LIMPAR RAM", L"DESATIVAR SYSMAIN", L"FAZER TUDO" };
    wchar_t buf[160];
    size_t i, n;
    BOOL busy = g_busy != 0;
    float cx, cy;
    int k;

    if (!g_px) return;

    n = (size_t)g_W * (size_t)g_H;
    for (i = 0; i < n; i++) g_px[i] = C_BG;

    cx = (float)g_W * 0.5f;
    cy = (float)S(200);
    DrawRing(cx, cy, SF(100.0f), SF(16.0f), g_shown / 100.0f);

    /* botões */
    for (k = 0; k < 3; k++) {
        BOOL hov = (g_hover == k) && !busy;
        rrect_b((float)g_rBtn[k].left, (float)g_rBtn[k].top,
                (float)(g_rBtn[k].right - g_rBtn[k].left),
                (float)(g_rBtn[k].bottom - g_rBtn[k].top),
                SF(10.0f), hov ? C_BLUE : C_PANEL, busy ? C_BLUE_DK : C_BLUE, SF(1.4f));
    }

    /* chaves (toggles) */
    DrawToggle(&g_rTog[0], g_auto, g_hover == 3);
    DrawToggle(&g_rTog[1], g_deep, g_hover == 4);

    /* painel do log */
    rrect_b((float)S(24), (float)S(546), (float)S(412), (float)S(140),
            SF(12.0f), C_PANEL, C_BLUE_DK, SF(1.2f));

    /* ---- texto ---- */
    SetBkMode(g_memDC, TRANSPARENT);

    Txt(g_fTitle, C_BLUE, APP_NAME, 0, S(12), g_W, S(52), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    Txt(g_fSub, C_MUTED, L"OTIMIZADOR DE MEMÓRIA", 0, S(50), g_W, S(70),
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    _snwprintf(buf, 160, L"%.0f%%", (double)g_shown);
    Txt(g_fBig, C_TEXT, buf, (int)cx - S(90), (int)cy - S(44), (int)cx + S(90), (int)cy + S(24),
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    Txt(g_fSub, C_MUTED, L"USO DE RAM", (int)cx - S(90), (int)cy + S(24), (int)cx + S(90), (int)cy + S(46),
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    _snwprintf(buf, 160, L"%.2f GB usados de %.2f GB",
               (g_mem.totalMB - g_mem.availMB) / 1024.0, g_mem.totalMB / 1024.0);
    Txt(g_fUI, C_TEXT, buf, 0, S(322), g_W, S(344), DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    _snwprintf(buf, 160, L"Disponível: %.0f MB   \u2022   Cache do sistema: %.0f MB",
               g_mem.availMB, g_mem.cacheMB);
    Txt(g_fSmall, C_MUTED, buf, 0, S(344), g_W, S(364), DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    {
        const wchar_t *t; uint32_t c;
        switch (g_sm) {
        case SM_RUNNING:  t = L"\u25CF  SYSMAIN: ATIVO";                          c = C_WARN;  break;
        case SM_DISABLED: t = L"\u25CF  SYSMAIN: DESATIVADO";                     c = C_BLUE;  break;
        case SM_STOPPED:  t = L"\u25CF  SYSMAIN: PARADO (pode voltar ao reiniciar)"; c = C_MUTED; break;
        case SM_ABSENT:   t = L"\u25CF  SYSMAIN: NÃO ENCONTRADO";                 c = C_MUTED; break;
        default:          t = L"\u25CF  SYSMAIN: ?";                              c = C_MUTED; break;
        }
        Txt(g_fUI, c, t, 0, S(372), g_W, S(396), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    for (k = 0; k < 3; k++) {
        BOOL hov = (g_hover == k) && !busy;
        Txt(g_fUI, busy ? C_MUTED : (hov ? C_BG : C_BLUE), btn[k],
            g_rBtn[k].left, g_rBtn[k].top, g_rBtn[k].right, g_rBtn[k].bottom,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    _snwprintf(buf, 160, L"Limpar automaticamente quando a RAM passar de %d%%", g_threshold);
    Txt(g_fSmall, C_TEXT, buf, g_rTog[0].left + S(52), g_rTog[0].top, g_rTog[0].right, g_rTog[0].bottom,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    Txt(g_fSmall, C_TEXT, L"Limpeza profunda (standby + modified list)*",
        g_rTog[1].left + S(52), g_rTog[1].top, g_rTog[1].right, g_rTog[1].bottom,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    Txt(g_fSub, C_MUTED, L"* pode causar pequena lentidão logo após a limpeza",
        g_rTog[1].left + S(52), S(520), g_rTog[1].right, S(538), DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    {
        int start = g_logN > LOG_VISIBLE ? g_logN - LOG_VISIBLE : 0, line;
        for (line = start; line < g_logN; line++) {
            const wchar_t *s = g_log[line % LOG_MAX];
            int ty = S(556) + (line - start) * S(17);
            Txt(g_fLog, wcsstr(s, L"[falhou]") ? C_WARN : C_TEXT, s,
                S(34), ty, S(428), ty + S(17), DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    GdiFlush();   /* garante que o GDI terminou antes do próximo frame mexer nos pixels */
}

static void Blit(HDC dc)
{
    BitBlt(dc, 0, 0, g_W, g_H, g_memDC, 0, 0, SRCCOPY);
}

/* Redesenha e envia direto para a janela. Se estiver oculta/minimizada, adia. */
static void Redraw(void)
{
    HDC dc;
    if (!g_hwnd || !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) { g_needRedraw = TRUE; return; }
    g_needRedraw = FALSE;
    Render();
    dc = GetDC(g_hwnd);
    if (dc) { Blit(dc); ReleaseDC(g_hwnd, dc); }
}

/* ------------------------------------------------------------------------- */
/* Animação e ciclo de atualização                                           */
/* ------------------------------------------------------------------------- */

static void StartAnim(void)
{
    if (!g_animOn) { g_animOn = TRUE; SetTimer(g_hwnd, TIMER_ANIM, 16, NULL); }
}

static void TrimSelf(void)
{
    EmptyWorkingSet(GetCurrentProcess());   /* o próprio app devolve a RAM que não usa */
}

static void TrayUpdate(void)
{
    if (g_mem.load == g_lastTrayLoad) return;
    g_lastTrayLoad = g_mem.load;
    _snwprintf(g_nid.szTip, 128, L"RamQuill - RAM %lu%%", (unsigned long)g_mem.load);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

static void Tick(void)
{
    static int n = 0;
    MemInfo m = ReadMem();
    BOOL changed = (m.load != g_mem.load) || ((long)m.availMB != (long)g_mem.availMB)
                   || ((long)m.cacheMB != (long)g_mem.cacheMB);

    g_mem = m;
    g_target = (float)m.load;
    if (fabsf(g_target - g_shown) > 0.08f) StartAnim();

    if (n % 5 == 0) {                                   /* SysMain: a cada 5 s */
        SmState s = SysMainQuery();
        if (s != g_sm) { g_sm = s; changed = TRUE; }
    }
    if (n == 10) TrimSelf();
    n++;

    if (changed || g_needRedraw) Redraw();
    TrayUpdate();

    if (g_auto && m.load >= (ULONG)g_threshold && !g_busy
        && GetTickCount64() - g_lastAuto >= AUTO_COOLDOWN_MS) {
        g_lastAuto = GetTickCount64();
        LogF(L"Auto-limpeza: RAM em %lu%%", (unsigned long)m.load);
        StartJob(JOB_CLEAN);
    }
}

/* ------------------------------------------------------------------------- */
/* Bandeja do sistema                                                        */
/* ------------------------------------------------------------------------- */

static void TrayAdd(void)
{
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_lastTrayLoad = 0xFFFFFFFF;
}

static void ShowFromTray(void)
{
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    Redraw();
}

static void TrayMenu(void)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_OPEN,  L"Abrir RamQuill");
    AppendMenuW(m, MF_STRING, IDM_CLEAN, L"Limpar RAM agora");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT,  L"Sair");
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------------- */
/* Janela                                                                    */
/* ------------------------------------------------------------------------- */

static void ApplyDwm(HWND h)
{
    BOOL dark = TRUE;
    COLORREF cap = CR(C_BG), brd = CR(C_BLUE_DK), txt = CR(C_BLUE);
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR_, &cap, sizeof cap);   /* Windows 11 */
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR_, &brd, sizeof brd);
    DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR_, &txt, sizeof txt);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        BITMAPINFO bi;
        HDC sdc;

        g_hwnd = h;
        ApplyDwm(h);

        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g_W;
        bi.bmiHeader.biHeight = -g_H;               /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        sdc = GetDC(h);
        g_memDC = CreateCompatibleDC(sdc);
        g_dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
        ReleaseDC(h, sdc);
        if (!g_memDC || !g_dib || !g_px) return -1;
        g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);

        g_fTitle = MakeFont(L"Bahnschrift", 26, FW_BOLD);
        g_fSub   = MakeFont(L"Segoe UI", 11, FW_NORMAL);
        g_fBig   = MakeFont(L"Bahnschrift", 46, FW_BOLD);
        g_fUI    = MakeFont(L"Segoe UI", 12, FW_SEMIBOLD);
        g_fSmall = MakeFont(L"Segoe UI", 12, FW_NORMAL);
        g_fLog   = MakeFont(L"Consolas", 12, FW_NORMAL);

        BuildLayout();
        TrayAdd();
        Tick();
        SetTimer(h, TIMER_TICK, 1000, NULL);
        LogF(L"RamQuill iniciado. Privilégios: perfil=%ls, cota=%ls",
             g_privProfile ? L"ok" : L"não", g_privQuota ? L"ok" : L"não");
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_needRedraw) { Render(); g_needRedraw = FALSE; }
        Blit(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            float d = g_target - g_shown;
            g_shown += d * 0.14f;
            if (fabsf(d) < 0.08f) {
                g_shown = g_target;
                KillTimer(h, TIMER_ANIM);
                g_animOn = FALSE;
            }
            Redraw();
        } else if (wp == TIMER_TICK) {
            Tick();
        }
        return 0;

    case WM_MOUSEMOVE: {
        int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!g_tracking) {
            TRACKMOUSEEVENT t;
            t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = h; t.dwHoverTime = 0;
            TrackMouseEvent(&t);
            g_tracking = TRUE;
        }
        if (hit != g_hover) { g_hover = hit; Redraw(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (g_hover != -1) { g_hover = -1; Redraw(); }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover >= 0) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP: {
        int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        switch (hit) {
        case 0: StartJob(JOB_CLEAN); break;
        case 1: StartJob(JOB_SYSMAIN); break;
        case 2: StartJob(JOB_SYSMAIN | JOB_CLEAN); break;
        case 3: g_auto = !g_auto; SaveCfg(); Redraw(); break;
        case 4: g_deep = !g_deep; SaveCfg(); Redraw(); break;
        default: break;
        }
        return 0;
    }

    case WM_APP_LOG: {
        wchar_t *s = (wchar_t *)lp;
        if (s) {
            wcsncpy(g_log[g_logN % LOG_MAX], s, LOG_LEN - 1);
            g_log[g_logN % LOG_MAX][LOG_LEN - 1] = 0;
            g_logN++;
            free(s);
            Redraw();
        }
        return 0;
    }

    case WM_APP_DONE:
        InterlockedExchange(&g_busy, 0);
        Tick();
        Redraw();
        return 0;

    case WM_APP_SHOW:
        ShowFromTray();
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP) ShowFromTray();
        else if (lp == WM_RBUTTONUP) TrayMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:  ShowFromTray(); break;
        case IDM_CLEAN: StartJob(JOB_CLEAN); break;
        case IDM_EXIT:  DestroyWindow(h); break;
        default: break;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {      /* minimizar = ir para a bandeja */
            ShowWindow(h, SW_HIDE);
            TrimSelf();
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(h, TIMER_TICK);
        KillTimer(h, TIMER_ANIM);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_oldBmp && g_memDC) SelectObject(g_memDC, g_oldBmp);
        if (g_dib) DeleteObject(g_dib);
        if (g_memDC) DeleteDC(g_memDC);
        if (g_fTitle) DeleteObject(g_fTitle);
        if (g_fSub)   DeleteObject(g_fSub);
        if (g_fBig)   DeleteObject(g_fBig);
        if (g_fUI)    DeleteObject(g_fUI);
        if (g_fSmall) DeleteObject(g_fSmall);
        if (g_fLog)   DeleteObject(g_fLog);
        g_dib = NULL; g_memDC = NULL; g_px = NULL;
        g_hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == g_msgTaskbarCreated && msg != 0) {   /* Explorer reiniciou: recria o ícone */
            TrayAdd();
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------------- */
/* Ponto de entrada                                                          */
/* ------------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    RECT rc;
    HANDLE mtx;
    HDC sdc;
    HWND hw;
    MSG m;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    (void)hPrev; (void)cmd; (void)show;

    /* 1) precisa de Administrador (o manifesto já pede; isto cobre execução sem manifesto) */
    if (!IsElevated()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if ((INT_PTR)ShellExecuteW(NULL, L"runas", exe, NULL, NULL, SW_SHOWNORMAL) <= 32)
            MessageBoxW(NULL, L"O RamQuill precisa de permissão de Administrador para limpar a memória.",
                        APP_NAME, MB_ICONWARNING);
        return 0;
    }

    /* 2) instância única */
    mtx = CreateMutexW(NULL, TRUE, L"RamQuill_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(WND_CLASS, NULL);
        if (w) PostMessageW(w, WM_APP_SHOW, 0, 0);
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    g_hInst = hInst;

    /* 3) NtSetSystemInformation (ntdll já está carregada; GetModuleHandle não cria handle) */
    g_NtSet = (NtSetSystemInformation_t)(void (*)(void))
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation");

    /* 4) privilégios necessários para as limpezas profundas */
    g_privProfile = EnablePrivilege(SE_PROF_SINGLE_PROCESS_NAME);
    g_privQuota   = EnablePrivilege(SE_INCREASE_QUOTA_NAME);

    InitIni();
    LoadCfg();

    /* 5) escala de DPI */
    sdc = GetDC(NULL);
    g_scale = (float)GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(NULL, sdc);
    g_W = S(460);
    g_H = S(700);

    /* 6) ícone (recurso 1, se existir) */
    g_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (g_icon) g_iconOwned = TRUE;
    else g_icon = LoadIconW(NULL, IDI_APPLICATION);

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = g_icon;
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Falha ao registrar a janela.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    rc.left = 0; rc.top = 0; rc.right = g_W; rc.bottom = g_H;
    AdjustWindowRect(&rc, style, FALSE);
    hw = CreateWindowExW(0, WND_CLASS, APP_NAME, style,
                         (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                         (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
                         rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
    if (!hw) {
        MessageBoxW(NULL, L"Falha ao criar a janela.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hw, SW_SHOWNORMAL);
    UpdateWindow(hw);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_iconOwned && g_icon) DestroyIcon(g_icon);
    if (wc.hIcon) DestroyIcon(wc.hIcon);   /* DestroyIcon em ícone compartilhado do sistema é inofensivo */
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return (int)m.wParam;
}
