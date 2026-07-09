# Taskbar Dock Mode (taskbar sobreposta estilo macOS)

Um mod para o [Windhawk](https://windhawk.net/) no **Windows 11** que faz a barra de tarefas se comportar como o Dock do macOS: ela fica permanentemente desenhada **por cima de todas as janelas** e **deixa de reservar espaço** na Work Area do monitor — preservando 100% do visual nativo do Windows (Mica, Acrylic, cantos arredondados e animações não são tocados).

> For the English version of this document, see **README.md**.

## O que ele faz

- A **Work Area** de cada monitor passa a ser a resolução completa do monitor.
- **Janelas maximizadas** usam a tela inteira, incluindo a faixa onde a taskbar fica.
- A taskbar (`Shell_TrayWnd` e `Shell_SecondaryTrayWnd`, em monitores secundários) é reforçada como `HWND_TOPMOST`, permanecendo sempre visível acima das outras janelas.
- Tudo na taskbar continua funcionando: Iniciar, pesquisa, widgets, bandeja do sistema, relógio, notificações.
- Funciona com **múltiplos monitores**, **escalas de DPI diferentes** e barras posicionadas em **qualquer lado** (inferior, superior, esquerda, direita).

## O que ele NÃO altera

- Nenhuma mudança visual: aparência, transparência, Mica/Acrylic, cantos arredondados e animações permanecem exatamente como o Windows renderiza.
- Nenhum componente do Explorer é substituído. Nenhum aplicativo externo em segundo plano. Tudo vive dentro deste único mod do Windhawk.

## Como funciona (resumo técnico)

Em vez de reescrever o cálculo interno de geometria da taskbar, o mod usa o mecanismo que o próprio Windows oferece para "Work Area = tela cheia":

1. Ativa programaticamente o auto-hide da taskbar via `SHAppBarMessage(ABM_SETSTATE, ABS_AUTOHIDE)` — uma API pública e documentada. Com o auto-hide ligado, o Windows entrega nativamente a área completa do monitor como Work Area em **todos** os monitores, com qualquer DPI e com a barra em qualquer borda. Janelas maximizadas usam 100% da tela.
2. Em seguida, bloqueia o ato de esconder: hooks em `TrayUI::_Hide()` (`taskbar.dll`, lado Win32) e em `winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded` (`Taskbar.View.dll`, lado XAML — cobre também as taskbars de monitores secundários) mantêm todas as barras permanentemente expandidas e visíveis.

O resultado é uma taskbar em auto-hide que nunca se esconde: sem espaço reservado, sempre por cima, com visual totalmente nativo. Dois `WinEventHook`s orientados a evento (sem polling) reforçam o `HWND_TOPMOST` e reaplicam o auto-hide caso algo (ex.: o app Configurações) o desligue, e uma thread de curta duração na inicialização espera a `Shell_TrayWnd` existir antes de aplicar o estado — cobrindo também reinícios do Explorer, já que o Windhawk reinjeta o mod em qualquer novo `explorer.exe`.

**Efeito colateral visível (e reversível):** enquanto o mod estiver ativo, o toggle "Ocultar automaticamente a barra de tarefas" nas Configurações do Windows aparecerá LIGADO — é o mecanismo em uso. O estado original é salvo e restaurado ao desativar o mod, o que faz o próprio Windows recalcular a Work Area nativa.

## Instalação

1. Instale o [Windhawk](https://windhawk.net/), caso ainda não tenha.
2. Abra o Windhawk → clique no botão de **perfil/menu** → **New mod** (ou, em versões mais novas, "Create a new mod" na área de desenvolvimento de mods).
3. Substitua o modelo pelo conteúdo completo de `taskbar-dock-mode.wh.cpp`.
4. Salve (Ctrl+S). O Windhawk compila o mod e o ativa. No primeiro carregamento ele pode baixar símbolos de depuração de `taskbar.dll` do servidor público de símbolos da Microsoft — isso é normal e acontece uma vez por build do Windows.

## Desinstalação / restauração

Desative ou remova o mod no Windhawk. O hook é removido automaticamente e o mod dispara uma notificação de mudança de exibição para forçar o recálculo da Work Area. Se as janelas maximizadas não voltarem imediatamente ao tamanho original, reinicie o Explorer (`explorer.exe`) uma vez para garantir a restauração completa.

## Limitações conhecidas

- **Aplicativos em fullscreen exclusivo** (a maioria dos jogos em modo exclusivo DirectX/OpenGL, alguns players de vídeo) usam um caminho de composição que contorna o gerenciador de janelas normal. O próprio Windows suspende janelas topmost nesse modo, então esses aplicativos podem cobrir a taskbar. Isso está fora do alcance de qualquer hook em `explorer.exe`.
- Aplicativos legados que consultam `SHAppBarMessage(ABM_GETTASKBARPOS)` para não sobrepor a taskbar podem desenhar por baixo dela — o que, na prática, é justamente o comportamento do Dock do macOS.
- `TrayUI::_Hide` e `ViewCoordinator::ShouldTaskbarBeExpanded` são **símbolos não documentados da Microsoft**. As assinaturas podem mudar entre builds do Windows. Se o mod falhar ao carregar após uma atualização do Windows, verifique o log do Windhawk; a ferramenta [Windhawk Symbol Helper](https://github.com/ramensoftware/windhawk-symbol-helper) mostra os nomes atuais dos símbolos em `taskbar.dll` e `Taskbar.View.dll` para você atualizar as strings nos arrays de hook.

## Arquivos desta pasta

| Arquivo | Descrição |
|---|---|
| `taskbar-dock-mode.wh.cpp` | Código-fonte do mod do Windhawk (C++) |
| `README.md` | Versão em inglês deste documento |
| `LEIA-ME.md` | Este documento (português) |

## Licença

GNU General Public License v3.0.
