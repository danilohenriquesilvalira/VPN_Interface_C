# RLS Automacao VPN — Guia Completo de Configuracao e Actualizacao

## Repositorio GitHub

| Campo        | Valor                                                          |
|--------------|----------------------------------------------------------------|
| URL          | https://github.com/danilohenriquesilvalira/VPN_Interface_C    |
| Branch main  | `main`                                                         |
| Releases     | https://github.com/danilohenriquesilvalira/VPN_Interface_C/releases |

---

## Token GitHub (OAuth)

O token esta guardado no **Windows Credential Manager** com o nome `git:https://github.com`.

Para recuperar via linha de comando:
```powershell
cmdkey /list | findstr github
```

Para usar o token em scripts:
```powershell
# Recupera o token do Credential Manager:
$cred = Get-StoredCredential -Target "git:https://github.com"
$env:GH_TOKEN = $cred.GetNetworkCredential().Password
```

> **Nota:** Se o token expirar, gera um novo em https://github.com/settings/tokens
> com os scopes: `repo`, `workflow`, `write:packages`

---

## Credenciais VPN (hardcoded em vpn.h)

| Campo         | Valor              |
|---------------|--------------------|
| Servidor      | 10.201.114.222     |
| Porta         | 443                |
| Hub VPN       | DEFAULT            |
| Utilizador    | luiz               |
| Password      | luiz1234           |
| Nome da conta | RLS_Automacao      |
| Nome da NIC   | RLS_Automacao      |

Para alterar as credenciais, edita o ficheiro `src/vpn.h`:
```c
#define DEFAULT_HOST  "10.201.114.222"
#define DEFAULT_PORT  443
#define DEFAULT_HUB   "DEFAULT"
#define DEFAULT_USER  "luiz"
#define DEFAULT_PASS  "luiz1234"
#define ACCOUNT_NAME  "RLS_Automacao"
#define NIC_NAME      "RLS_Automacao"
```

---

## Como Publicar uma Nova Versao (rapido)

### 1. Faz as alteracoes no codigo

Edita os ficheiros em `src/` conforme necessario.

### 2. Faz commit e push

```bash
cd C:\Users\Admin\Desktop\analise_rede\VPN_Interface_C

git add -A
git commit -m "descricao da alteracao"
git push origin main
```

### 3. Cria uma nova tag para publicar release

```bash
git tag v1.0.2
git push origin v1.0.2
```

O GitHub Actions vai automaticamente:
- Descarregar o SoftEther VPN Client mais recente
- Compilar o `rls_vpn.exe` com o SoftEther embutido
- Criar um Release no GitHub com o `.exe` para download

### 4. Verifica o build

Vai a: https://github.com/danilohenriquesilvalira/VPN_Interface_C/actions

Se o build falhar, clica no job para ver os logs de erro.

---

## Estrutura do Projecto

```
VPN_Interface_C/
├── src/
│   ├── main.c          # GUI Win32 (janela, botoes, logica de UI)
│   ├── vpn.c           # Logica VPN (wrapper do vpncmd.exe)
│   ├── vpn.h           # Tipos, constantes e declaracoes
│   ├── resource.rc     # Recursos Windows (icone, manifest, instalador embutido)
│   ├── app.manifest    # UAC (requireAdministrator) + DPI
│   └── icon.ico        # Icone da aplicacao
├── .github/
│   └── workflows/
│       └── build.yml   # CI/CD: compila + cria release automaticamente
├── Makefile            # Build local com MinGW
├── SETUP.md            # Este ficheiro
└── installer.nsi       # Script NSIS (referencia, nao usado no CI)
```

---

## Como o Exe Funciona (fluxo automatico)

1. **Ao abrir**: verifica se o SoftEther VPN Client esta instalado
2. **Se nao instalado**: extrai o instalador embutido no proprio `.exe` para `%TEMP%`
3. **Instala silenciosamente**: `se_client.exe /SILENT /NORESTART`
4. **Configura automaticamente**: cria a placa de rede virtual `RLS_Automacao` e a conta VPN
5. **Pronto**: o utilizador so precisa de clicar "Ligar"

---

## Build Local (sem GitHub Actions)

Necessario: MinGW-w64 instalado e no PATH.

```bash
# Instalar MinGW via MSYS2
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils

# Copiar o instalador SoftEther para a raiz do projecto
copy C:\caminho\para\softether_installer.exe se_client.exe

# Compilar
cd src
windres resource.rc -O coff -o resource.o
cd ..
gcc -Wall -O2 -Isrc -mwindows -o rls_vpn.exe src/main.c src/vpn.c src/resource.o -lcomctl32 -lgdi32 -lshcore
```

---

## Troubleshooting

| Problema | Solucao |
|----------|---------|
| "Falha ao instalar SoftEther" | Executa como **Administrador** |
| "Servico VPN nao iniciou" | Reinicia o Windows e tenta de novo |
| "Cannot connect to VPN Client" | O servico `SevpnClient` nao esta a correr — vai a Servicos do Windows e inicia-o |
| Build falha no GitHub Actions | Verifica os logs em Actions; pode ser que o SoftEther mudou o nome do ficheiro de release |
| Token expirado (401) | Gera novo token em https://github.com/settings/tokens e actualiza no Credential Manager |
