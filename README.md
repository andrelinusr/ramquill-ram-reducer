# RamQuill

Limpador de memória RAM e desativador do serviço SysMain para **Windows 10/11**, escrito em **C puro (C17)** com a API nativa do Windows. Sem frameworks, sem runtime extra e sem instalação: um único `.exe` com interface escura em preto e azul.

> Pensado para máquinas com pouca RAM (por exemplo, notebooks com 4 GB), onde um alívio rápido de memória ou desativar o SysMain faz diferença.

---

## O que é

O RamQuill pede ao Windows para devolver a memória que os programas guardam sem estar usando e esvazia alguns caches do sistema. **Ele não fecha nenhum programa nem processo.** Também tem um botão para parar e desativar o serviço **SysMain** (o antigo Superfetch).

A interface mostra o uso de RAM em um **anel de progresso circular animado**, com a porcentagem no centro, e um registro das operações logo abaixo.

## Recursos

- Anel de progresso circular com animação suave, brilho e pontas arredondadas, desenhado por software com antialiasing.
- **Limpar RAM** em um clique, com duas intensidades: padrão e profunda.
- **Desativar SysMain** pelo Gerenciador de Serviços do Windows (sem chamar `sc.exe`).
- **Auto-limpeza** quando o uso de RAM passa de um limite (padrão: 85%), com intervalo mínimo de 60 segundos entre execuções.
- Ícone na **bandeja do sistema**: minimizar envia o app para lá, com menu para abrir, limpar ou sair.
- Registro das etapas, mostrando o código `NTSTATUS` quando algo falha.
- Instância única, escala automática de DPI e barra de título escura.
- Configurações salvas em `%APPDATA%\RamQuill\RamQuill.ini`.

## Como funciona

A limpeza roda em uma thread separada para a janela não travar. As etapas seguem esta ordem, e a máscara de opções define quais são executadas:

| # | Etapa | Como |
|---|-------|------|
| 1 | Working set de todos os processos | `NtSetSystemInformation` → `SystemMemoryListInformation` (`MemoryEmptyWorkingSets`) |
| 2 | Cache de arquivos do sistema | `SetSystemFileCacheSize(-1, -1, 0)` |
| 3 | Cache de escrita dos volumes fixos | `CreateFileW("\\.\X:")` + `FlushFileBuffers` |
| 4 | Lista de páginas modificadas *(profunda)* | `MemoryFlushModifiedList` |
| 5 | Lista standby *(profunda)* | `MemoryPurgeStandbyList` |
| 6 | Standby de baixa prioridade | `MemoryPurgeLowPriorityStandbyList` |
| 7 | Cache do registro | `SystemRegistryReconciliationInformation` (classe 155) |
| 8 | Combinação de páginas idênticas | `SystemCombinePhysicalMemoryInformation` (classe 130) |

**Detalhes importantes**

- `NtSetSystemInformation` é carregada em tempo de execução com `GetProcAddress` a partir da `ntdll.dll`.
- Antes de limpar, o app habilita `SeProfileSingleProcessPrivilege` e `SeIncreaseQuotaPrivilege` no próprio token (`OpenProcessToken`, `LookupPrivilegeValueW`, `AdjustTokenPrivileges`).
- Se o passo 1 falhar, entra um **plano B** que percorre os processos com `EnumProcesses` e aplica `EmptyWorkingSet` em cada um, **ignorando processos críticos** (`csrss`, `smss`, `wininit`, `services`, `lsass`, `winlogon`, `System`, `Registry` e processos sem nome).
- Os passos 4 e 5 ficam só no modo **profundo**, porque podem causar uma lentidão passageira logo após a limpeza (o Windows precisa reler dados do disco).
- A ordem foi escolhida para que o que foi gravado no disco (passos 2 a 4) já possa ser descartado na mesma passada (passos 5 e 6).
- O **SysMain** é controlado pelo Service Control Manager: `ChangeServiceConfigW` define a inicialização como *Desativado* e `ControlService` envia o comando de parada.

**Nota sobre os números:** o uso de RAM vem de `GlobalMemoryStatusEx`, que conta a lista standby como memória disponível. Por isso o anel pode cair logo após a limpeza mesmo que parte daquelas páginas ainda esteja fisicamente na RAM, só em outra lista.

## Como usar

1. Abra o `RamQuill.exe`. O Windows vai pedir permissão de **Administrador**, que é necessária para mexer na memória do sistema e nos serviços.
2. Use os botões:
   - **LIMPAR RAM**: executa a limpeza.
   - **DESATIVAR SYSMAIN**: para o serviço e impede que ele inicie com o Windows.
   - **FAZER TUDO**: desativa o SysMain e limpa a RAM.
3. Chaves opcionais:
   - **Limpar automaticamente quando a RAM passar de 85%**: liga a auto-limpeza.
   - **Limpeza profunda**: inclui as listas standby e modified (libera mais, com possível lentidão passageira).
4. Minimize para mandar o app para a bandeja. Clique no ícone para reabrir ou use o botão direito para o menu. O botão de fechar encerra o programa.

### Arquivo de configuração

`%APPDATA%\RamQuill\RamQuill.ini`:

```ini
[RamQuill]
AutoClean=0
DeepClean=0
Threshold=85
```

`Threshold` aceita valores de 50 a 99.

## Compilar

**Requisitos:** Windows 10/11 64 bits e um compilador **GCC do MinGW-w64** (por exemplo, o [WinLibs](https://winlibs.com) em ZIP, ou o MSYS2).

### Jeito rápido

Deixe `main.c`, `ramquill.manifest` e `compilar.bat` na mesma pasta (e, se quiser um ícone próprio, um `ramquill.ico`) e dê dois cliques no `compilar.bat`. Ele procura o gcc sozinho e, se não achar, pergunta o caminho da pasta `bin`.

### Manualmente

```bat
echo 1 24 "ramquill.manifest" > ramquill.rc
windres ramquill.rc -O coff -o ramquill_res.o

gcc -std=gnu17 -O2 -s -municode -mwindows -static -Wall -Wextra ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    main.c ramquill_res.o -o RamQuill.exe ^
    -lgdi32 -luser32 -ladvapi32 -lpsapi -ldwmapi -lshell32 -lm
```

Para colocar um ícone no executável, adicione `1 ICON "ramquill.ico"` ao `ramquill.rc` antes de rodar o `windres`.

## DLLs usadas

Todas já vêm no Windows: `kernel32`, `user32`, `gdi32`, `advapi32`, `shell32`, `dwmapi` e a runtime C do sistema. A `ntdll` é acessada em tempo de execução (`GetProcAddress`). `libgcc` e `winpthread` são embutidas pelo `-static`.

## Estrutura do projeto

```
main.c               código-fonte completo
ramquill.manifest    pede Administrador e declara compatibilidade e DPI
compilar.bat         script de compilação para Windows
ramquill.ico         (opcional) ícone do aplicativo
```

## Avisos

- O RamQuill **não encerra processos**, não injeta código, não grava na memória de outros programas e não usa rede.
- Limpar a RAM dá **alívio temporário**: os programas voltam a ocupar memória conforme são usados, e logo após a limpeza o sistema pode ficar um pouco mais lento até os caches se reencherem. Evite a limpeza profunda em máquinas virtuais, bancos de dados com cache grande e trabalho de áudio ou vídeo em tempo real.
- Algumas etapas usam **chamadas não documentadas** do Windows. Elas podem mudar ou não existir em certas versões; quando uma falha, o registro mostra o `NTSTATUS`.
- **Desfazer o SysMain:** rode `sc config SysMain start= auto` e `sc start SysMain` em um terminal de Administrador, ou use o `services.msc`.
- Antivírus podem alertar sobre programas novos que mexem em memória e serviços (falso positivo comum). O código-fonte está aberto para conferência.
- Use por sua conta e risco.

## Créditos e licença

Código original, escrito a partir da técnica pública de limpeza de listas de memória do Windows, também usada por utilitários como o [Mem Reduct](https://github.com/henrypp/memreduct) (GPLv3). Nenhum trecho do código do Mem Reduct foi copiado.


