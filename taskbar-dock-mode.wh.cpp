// ==WindhawkMod==
// @id              taskbar-dock-mode
// @name            Taskbar Dock Mode (macOS-style overlay taskbar)
// @description     Faz a taskbar do Windows 11 se comportar como o Dock do macOS: sempre por cima, sem reservar espaço na Work Area
// @version         1.1.0
// @author          (personalizar)
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.

// ==WindhawkModReadme==
/*
# Taskbar Dock Mode

Faz a taskbar do Windows 11 parar de reservar espaço na Work Area do monitor,
mantendo-a sempre desenhada por cima ("topmost") das janelas, de forma
semelhante ao Dock do macOS.

## Como funciona (v1.1)

Em vez de tentar reescrever o cálculo interno de geometria da taskbar, o mod
usa o mecanismo que o próprio Windows oferece para "Work Area = tela cheia":

1. Ativa programaticamente o auto-hide da taskbar via
   `SHAppBarMessage(ABM_SETSTATE, ABS_AUTOHIDE)` — uma API pública e
   documentada. Com auto-hide ativo, o Windows entrega nativamente a área de
   trabalho completa em todos os monitores, com qualquer DPI e com a barra em
   qualquer borda. Janelas maximizadas passam a usar 100% da tela.
2. Bloqueia o ato de esconder: hooks em `TrayUI::_Hide()` (lado Win32) e em
   `winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded`
   (lado XAML, cobre também taskbars secundárias) mantêm a barra sempre
   expandida e visível.

Resultado: a barra nunca reserva espaço, nunca se esconde e fica sempre por
cima — comportamento de Dock — com o visual 100% nativo (Mica, Acrylic,
cantos e animações intactos; o mod não desenha nada).

## Efeito colateral visível (e reversível)

Enquanto o mod estiver ativo, o toggle "Ocultar automaticamente a barra de
tarefas" nas Configurações do Windows aparecerá LIGADO — é o mecanismo que o
mod usa. O estado original é salvo e restaurado ao desativar o mod.

## O que este mod NÃO consegue contornar

- **Aplicativos em modo fullscreen exclusivo** (a maioria dos jogos em modo
  exclusivo DirectX/OpenGL) usam um caminho de composição que contorna o
  gerenciador de janelas; podem cobrir a taskbar. Fora do alcance de hooks
  em explorer.exe.

## Limitações conhecidas

`TrayUI::_Hide` e `ViewCoordinator::ShouldTaskbarBeExpanded` são símbolos
internos e não documentados; podem mudar entre builds. Se o mod falhar após
uma atualização do Windows, verifique o log do Windhawk e confirme os nomes
atuais com o Windhawk Symbol Helper (módulos `taskbar.dll` e
`Taskbar.View.dll`).
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <shellapi.h>

#include <atomic>
#include <unordered_set>

// -----------------------------------------------------------------------
// Estado global do mod
// -----------------------------------------------------------------------

std::unordered_set<HWND> g_taskbarWindows;

HWINEVENTHOOK g_hForegroundEventHook = nullptr;
HWINEVENTHOOK g_hObjectCreateEventHook = nullptr;

// Estado original do auto-hide, salvo antes de o mod alterá-lo, para
// restauração fiel na desinstalação.
std::atomic<bool> g_originalStateSaved{false};
UINT g_originalAppBarState = 0;

// Controle da thread de aplicação adiada (ver ScheduleApplyDockState).
std::atomic<bool> g_applyThreadActive{false};
std::atomic<bool> g_modUnloading{false};

// Ponteiro para SHAppBarMessage resolvido dinamicamente (shell32.dll).
// Resolver em tempo de execução evita depender de flags de link do
// compilador do Windhawk.
using SHAppBarMessage_t = UINT_PTR(WINAPI*)(DWORD, PAPPBARDATA);
SHAppBarMessage_t g_pSHAppBarMessage = nullptr;

enum class WinVersion {
    Unsupported,
    Win10,
    Win11,
};

WinVersion g_winVersion = WinVersion::Unsupported;

// -----------------------------------------------------------------------
// Detecção de versão do Windows
// -----------------------------------------------------------------------
// RtlGetVersion (ntdll) em vez de GetVersionEx: a segunda é afetada por
// manifestos de compatibilidade e pode mentir sobre a build real.
WinVersion DetectWinVersion() {
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOEXW*);

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) {
        Wh_Log(L"Falha ao obter handle de ntdll.dll");
        return WinVersion::Unsupported;
    }

    auto pRtlGetVersion =
        (RtlGetVersion_t)GetProcAddress(hNtdll, "RtlGetVersion");
    if (!pRtlGetVersion) {
        Wh_Log(L"RtlGetVersion não encontrada");
        return WinVersion::Unsupported;
    }

    OSVERSIONINFOEXW osInfo{};
    osInfo.dwOSVersionInfoSize = sizeof(osInfo);
    if (pRtlGetVersion(&osInfo) != 0) {
        Wh_Log(L"RtlGetVersion falhou");
        return WinVersion::Unsupported;
    }

    Wh_Log(L"Build detectada: %u", osInfo.dwBuildNumber);

    if (osInfo.dwBuildNumber >= 22000) {
        return WinVersion::Win11;
    }
    if (osInfo.dwBuildNumber >= 10240) {
        return WinVersion::Win10;
    }
    return WinVersion::Unsupported;
}

// -----------------------------------------------------------------------
// Hook 1 (Win32): TrayUI::_Hide — impede a taskbar de se esconder
// -----------------------------------------------------------------------
// Com o auto-hide ativo, o único caminho para a barra sair da tela é a
// chamada interna a TrayUI::_Hide (disparada por timer quando o mouse sai
// da barra, ou quando uma janela fullscreen aparece). Transformá-la em
// no-op mantém a barra permanentemente no estado "mostrada". Como nunca há
// transição para "escondida", nenhuma animação nova é introduzida.
using TrayUI__Hide_t = void(WINAPI*)(void* pThis);
TrayUI__Hide_t TrayUI__Hide_Original;

void WINAPI TrayUI__Hide_Hook(void* pThis) {
    // No-op intencional: nunca esconder.
}

// -----------------------------------------------------------------------
// Hook 2 (XAML): ViewCoordinator::ShouldTaskbarBeExpanded — sempre true
// -----------------------------------------------------------------------
// No Windows 11, o estado expandido/recolhido de CADA taskbar (inclusive as
// de monitores secundários) é coordenado no lado XAML por
// winrt::Taskbar::implementation::ViewCoordinator. O primeiro parâmetro
// (unsigned __int64) identifica a view/monitor. Forçar o retorno "true"
// mantém todas as barras expandidas. Chamamos a original mesmo assim para
// preservar qualquer efeito colateral interno de contabilização de estado.
using ViewCoordinator_ShouldTaskbarBeExpanded_t =
    bool(WINAPI*)(void* pThis, unsigned __int64 viewId, bool param);
ViewCoordinator_ShouldTaskbarBeExpanded_t
    ViewCoordinator_ShouldTaskbarBeExpanded_Original;

bool WINAPI
ViewCoordinator_ShouldTaskbarBeExpanded_Hook(void* pThis,
                                             unsigned __int64 viewId,
                                             bool param) {
    ViewCoordinator_ShouldTaskbarBeExpanded_Original(pThis, viewId, param);
    return true;
}

// -----------------------------------------------------------------------
// Controle do estado de appbar (auto-hide) via API pública
// -----------------------------------------------------------------------

HWND FindPrimaryTaskbar() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

UINT GetAppBarState() {
    if (!g_pSHAppBarMessage) {
        return 0;
    }
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    return (UINT)g_pSHAppBarMessage(ABM_GETSTATE, &abd);
}

bool SetAppBarState(UINT state) {
    if (!g_pSHAppBarMessage) {
        return false;
    }
    HWND hTaskbar = FindPrimaryTaskbar();
    if (!hTaskbar) {
        return false;
    }
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hTaskbar;
    abd.lParam = state;
    g_pSHAppBarMessage(ABM_SETSTATE, &abd);
    return true;
}

// Aplica o "modo Dock": salva o estado original (uma única vez) e liga o
// auto-hide. A sequência OFF -> ON tem um propósito: se o auto-hide já
// estava ligado e a barra estava ESCONDIDA no momento em que o mod carrega,
// nunca haverá uma chamada a _Hide para bloquearmos — a barra ficaria
// invisível para sempre. Desligar o auto-hide primeiro força o Windows a
// mostrar a barra; religá-lo em seguida libera a Work Area de novo, e a
// partir daí o hook em _Hide impede qualquer nova ocultação.
bool ApplyDockState() {
    HWND hTaskbar = FindPrimaryTaskbar();
    if (!hTaskbar) {
        return false;
    }

    UINT current = GetAppBarState();

    bool expected = false;
    if (g_originalStateSaved.compare_exchange_strong(expected, true)) {
        g_originalAppBarState = current;
        Wh_Log(L"Estado original do appbar salvo: 0x%X", current);
    }

    if (current & ABS_AUTOHIDE) {
        SetAppBarState(current & ~ABS_AUTOHIDE);
    }
    bool ok = SetAppBarState(GetAppBarState() | ABS_AUTOHIDE);

    Wh_Log(L"Modo Dock %s (auto-hide ativado; ocultação bloqueada por hook)",
           ok ? L"aplicado" : L"FALHOU ao aplicar");
    return ok;
}

// Restaura fielmente o estado de auto-hide que o usuário tinha antes do mod.
void RestoreOriginalState() {
    if (!g_originalStateSaved.load()) {
        return;
    }
    UINT desired = g_originalAppBarState;
    UINT current = GetAppBarState();
    if ((current & ABS_AUTOHIDE) != (desired & ABS_AUTOHIDE)) {
        SetAppBarState(desired);
    }
    Wh_Log(L"Estado original do appbar restaurado: 0x%X", desired);
}

// Corpo da thread de aplicação adiada. Função livre com convenção WINAPI
// (__stdcall): LPTHREAD_START_ROUTINE, assim como WNDENUMPROC, não aceita
// lambdas do C++.
DWORD WINAPI ApplyDockStateThreadProc(LPVOID) {
    // Até ~30s de tentativas (60 x 500ms), suficiente até para boots
    // lentos; em geral resolve na primeira ou segunda iteração.
    for (int i = 0; i < 60 && !g_modUnloading.load(); i++) {
        if (ApplyDockState()) {
            break;
        }
        Sleep(500);
    }
    g_applyThreadActive.store(false);
    return 0;
}

// O mod é injetado ANTES de o Explorer criar a Shell_TrayWnd (o log de
// inicialização mostra a janela surgindo ~1s depois do Wh_ModInit). Como
// ABM_SETSTATE precisa da janela existindo — e chamá-la de dentro do
// callback de criação da própria janela arriscaria reentrância durante a
// inicialização da taskbar — usamos uma thread de curta duração que espera
// a janela aparecer e então aplica o estado. A thread termina sozinha; não
// há polling permanente.
void ScheduleApplyDockState() {
    bool expected = false;
    if (!g_applyThreadActive.compare_exchange_strong(expected, true)) {
        return;  // já existe uma tentativa em andamento
    }

    HANDLE hThread =
        CreateThread(nullptr, 0, ApplyDockStateThreadProc, nullptr, 0,
                     nullptr);

    if (hThread) {
        CloseHandle(hThread);
    } else {
        g_applyThreadActive.store(false);
        Wh_Log(L"AVISO: falha ao criar thread de aplicação (%u); tentando "
               L"aplicar de forma síncrona",
               GetLastError());
        ApplyDockState();
    }
}

// -----------------------------------------------------------------------
// Descoberta das janelas da taskbar + reforço de topmost
// -----------------------------------------------------------------------
// Barras em auto-hide já são topmost por design, mas o reforço explícito
// cobre casos em que outro aplicativo topmost tenta ficar acima da barra.
bool IsTaskbarClassName(PCWSTR className) {
    return _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

void EnforceTopmost(HWND hWnd) {
    if (!IsWindow(hWnd)) {
        return;
    }
    if (!SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        Wh_Log(L"SetWindowPos(HWND_TOPMOST) falhou para %08X, erro=%u",
               (DWORD)(DWORD_PTR)hWnd, GetLastError());
    }
}

// Callback de EnumWindows. Função livre com convenção CALLBACK
// (__stdcall): WNDENUMPROC não aceita lambdas do C++.
BOOL CALLBACK EnumTaskbarWindowsProc(HWND hWnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::unordered_set<HWND>*>(lParam);

    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hWnd, &dwProcessId);
    if (dwProcessId != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR className[64];
    if (!GetClassNameW(hWnd, className, ARRAYSIZE(className))) {
        return TRUE;
    }

    if (IsTaskbarClassName(className)) {
        windows->insert(hWnd);
        EnforceTopmost(hWnd);
    }

    return TRUE;
}

void EnumerateAndEnforceAllTaskbars() {
    EnumWindows(EnumTaskbarWindowsProc,
                reinterpret_cast<LPARAM>(&g_taskbarWindows));
}

// -----------------------------------------------------------------------
// WinEventHook 1: EVENT_SYSTEM_FOREGROUND
// -----------------------------------------------------------------------
// Orientado a evento (não é polling): executa só quando a janela em
// primeiro plano muda. Faz duas coisas baratas: (a) reforça o topmost das
// barras conhecidas; (b) verifica se algo desligou o auto-hide (ex.: o
// usuário mexeu nas Configurações) e o reaplica, mantendo a Work Area em
// tela cheia de forma consistente enquanto o mod estiver ativo.
void CALLBACK ForegroundEventProc(HWINEVENTHOOK hWinEventHook,
                                  DWORD event,
                                  HWND hWnd,
                                  LONG idObject,
                                  LONG idChild,
                                  DWORD dwEventThread,
                                  DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW) {
        return;
    }
    for (HWND hTaskbarWnd : g_taskbarWindows) {
        EnforceTopmost(hTaskbarWnd);
    }
    if (g_originalStateSaved.load() && !(GetAppBarState() & ABS_AUTOHIDE)) {
        Wh_Log(L"Auto-hide foi desativado externamente; reaplicando");
        SetAppBarState(GetAppBarState() | ABS_AUTOHIDE);
    }
}

// -----------------------------------------------------------------------
// WinEventHook 2: EVENT_OBJECT_CREATE (restrito ao processo do Explorer)
// -----------------------------------------------------------------------
// Detecta novas janelas de taskbar: reinício do Explorer (nova
// Shell_TrayWnd) ou monitor recém-conectado (Shell_SecondaryTrayWnd).
// Além do reforço de topmost, agenda a (re)aplicação do modo Dock, pois é
// aqui que sabemos que a barra primária passou a existir.
void CALLBACK ObjectCreateEventProc(HWINEVENTHOOK hWinEventHook,
                                    DWORD event,
                                    HWND hWnd,
                                    LONG idObject,
                                    LONG idChild,
                                    DWORD dwEventThread,
                                    DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW || !hWnd) {
        return;
    }

    WCHAR className[64];
    if (!GetClassNameW(hWnd, className, ARRAYSIZE(className))) {
        return;
    }

    if (IsTaskbarClassName(className)) {
        Wh_Log(L"Nova janela de taskbar detectada: %08X (%s)",
               (DWORD)(DWORD_PTR)hWnd, className);
        g_taskbarWindows.insert(hWnd);
        EnforceTopmost(hWnd);

        if (_wcsicmp(className, L"Shell_TrayWnd") == 0) {
            ScheduleApplyDockState();
        }
    }
}

// -----------------------------------------------------------------------
// Ciclo de vida do mod
// -----------------------------------------------------------------------

BOOL Wh_ModInit() {
    Wh_Log(L"Taskbar Dock Mode: iniciando");

    g_winVersion = DetectWinVersion();
    if (g_winVersion == WinVersion::Unsupported) {
        Wh_Log(L"Versão do Windows não suportada/detectável");
        return FALSE;
    }

    // SHAppBarMessage vive em shell32.dll; resolver dinamicamente evita
    // depender de flags de link (-lshell32) no compilador do Windhawk.
    HMODULE hShell32 = LoadLibraryW(L"shell32.dll");
    if (hShell32) {
        g_pSHAppBarMessage =
            (SHAppBarMessage_t)GetProcAddress(hShell32, "SHAppBarMessage");
    }
    if (!g_pSHAppBarMessage) {
        Wh_Log(L"ERRO: SHAppBarMessage não pôde ser resolvida");
        return FALSE;
    }

    // TrayUI vive em taskbar.dll no Win11; no Win10/ExplorerPatcher, no
    // módulo principal do explorer.exe. ViewCoordinator (lado XAML) vive em
    // Taskbar.View.dll no Win11 — tentamos hookar nos dois módulos e
    // exigimos que pelo menos um dos mecanismos de bloqueio de ocultação
    // tenha sido instalado.
    HMODULE hTaskbarModule;
    if (g_winVersion == WinVersion::Win11) {
        hTaskbarModule = LoadLibraryW(L"taskbar.dll");
        if (!hTaskbarModule) {
            Wh_Log(L"taskbar.dll não encontrada, usando módulo principal");
            hTaskbarModule = GetModuleHandleW(nullptr);
        }
    } else {
        hTaskbarModule = GetModuleHandleW(nullptr);
    }
    if (!hTaskbarModule) {
        Wh_Log(L"Não foi possível resolver o módulo da TrayUI");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK trayHooks[] = {
        {
            {LR"(public: void __cdecl TrayUI::_Hide(void))"},
            (void**)&TrayUI__Hide_Original,
            (void*)TrayUI__Hide_Hook,
            true,  // opcional (validado em conjunto abaixo)
        },
        {
            {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded(unsigned __int64,bool))"},
            (void**)&ViewCoordinator_ShouldTaskbarBeExpanded_Original,
            (void*)ViewCoordinator_ShouldTaskbarBeExpanded_Hook,
            true,  // opcional: pode estar em Taskbar.View.dll, não aqui
        },
    };

    if (!WindhawkUtils::HookSymbols(hTaskbarModule, trayHooks,
                                    ARRAYSIZE(trayHooks))) {
        Wh_Log(L"HookSymbols falhou para o módulo da TrayUI");
        return FALSE;
    }

    // Se ViewCoordinator não estava no módulo da TrayUI, procurar em
    // Taskbar.View.dll (localização usual no Win11).
    if (g_winVersion == WinVersion::Win11 &&
        !ViewCoordinator_ShouldTaskbarBeExpanded_Original) {
        HMODULE hTaskbarView = LoadLibraryW(L"Taskbar.View.dll");
        if (hTaskbarView) {
            WindhawkUtils::SYMBOL_HOOK viewHooks[] = {
                {
                    {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded(unsigned __int64,bool))"},
                    (void**)&ViewCoordinator_ShouldTaskbarBeExpanded_Original,
                    (void*)ViewCoordinator_ShouldTaskbarBeExpanded_Hook,
                    true,
                },
            };
            if (!WindhawkUtils::HookSymbols(hTaskbarView, viewHooks,
                                            ARRAYSIZE(viewHooks))) {
                Wh_Log(L"HookSymbols falhou para Taskbar.View.dll");
            }
        } else {
            Wh_Log(L"Taskbar.View.dll não pôde ser carregada");
        }
    }

    if (!TrayUI__Hide_Original &&
        !ViewCoordinator_ShouldTaskbarBeExpanded_Original) {
        Wh_Log(L"ERRO: nenhum mecanismo de bloqueio de ocultação pôde ser "
               L"instalado (TrayUI::_Hide e "
               L"ViewCoordinator::ShouldTaskbarBeExpanded não encontrados). "
               L"Verifique os nomes atuais com o Windhawk Symbol Helper.");
        return FALSE;
    }

    Wh_Log(L"Bloqueio de ocultação instalado: TrayUI::_Hide=%s, "
           L"ViewCoordinator::ShouldTaskbarBeExpanded=%s",
           TrayUI__Hide_Original ? L"sim" : L"não",
           ViewCoordinator_ShouldTaskbarBeExpanded_Original ? L"sim" : L"não");

    EnumerateAndEnforceAllTaskbars();

    g_hForegroundEventHook =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                        nullptr, ForegroundEventProc,
                        /*idProcess=*/0, /*idThread=*/0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_hForegroundEventHook) {
        Wh_Log(L"AVISO: SetWinEventHook (foreground) falhou");
    }

    g_hObjectCreateEventHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr,
        ObjectCreateEventProc, GetCurrentProcessId(), /*idThread=*/0,
        WINEVENT_OUTOFCONTEXT);
    if (!g_hObjectCreateEventHook) {
        Wh_Log(L"AVISO: SetWinEventHook (object create) falhou; a aplicação "
               L"do modo Dock após reinícios do Explorer dependerá apenas da "
               L"thread de inicialização");
    }

    Wh_Log(L"Taskbar Dock Mode: inicializado com sucesso");
    return TRUE;
}

void Wh_ModAfterInit() {
    // Se a Shell_TrayWnd já existe (mod ativado com o Explorer rodando),
    // aplica imediatamente; senão, a thread espera a janela aparecer — e o
    // EVENT_OBJECT_CREATE cobre reinícios futuros do Explorer.
    ScheduleApplyDockState();
    EnumerateAndEnforceAllTaskbars();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"Taskbar Dock Mode: desinstalando");
    g_modUnloading.store(true);

    if (g_hForegroundEventHook) {
        UnhookWinEvent(g_hForegroundEventHook);
        g_hForegroundEventHook = nullptr;
    }
    if (g_hObjectCreateEventHook) {
        UnhookWinEvent(g_hObjectCreateEventHook);
        g_hObjectCreateEventHook = nullptr;
    }

    g_taskbarWindows.clear();
}

void Wh_ModUninit() {
    // Os hooks de símbolo já foram removidos pelo motor do Windhawk; a
    // taskbar volta a poder se esconder normalmente. Restaurar o estado de
    // auto-hide original faz o próprio Windows recalcular e reaplicar a
    // Work Area nativa — restauração completa sem gambiarras.
    RestoreOriginalState();
    Wh_Log(L"Taskbar Dock Mode: desinstalado");
}
