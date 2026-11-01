# ResinDeck

[![CI](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml/badge.svg)](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml)

Um painel de controle open source para impressoras 3D de resina: veja seus arquivos fatiados em
3D, envie para a impressora pela rede e acompanhe a impressão, do seu próprio computador.

Hoje suporta impressoras **Creality HALOT**, direto do Linux, sem HALOT BOX e sem Wine nem Proton.
A ideia é centralizar num só lugar a comunicação em rede com impressoras 3D de resina, crescendo
para outras marcas e sistemas com a ajuda da comunidade.

[Read in English](README.md)

- **Interface web** (roda localmente, abre no navegador):
  - visualização 3D montada a partir das camadas do arquivo fatiado
  - visualizador camada por camada e a miniatura do fatiador
  - gerenciador de impressoras: várias impressoras, busca na rede, status ao vivo
  - enviar, enviar e imprimir, pausar/retomar, parar, com progresso por camada e tempo restante
- **Linha de comando** para scripts e tarefas rápidas.
- Lê arquivos `.cxdlpv4`, o formato que o HALOT BOX exporta para impressoras HALOT.

## Impressoras suportadas

| Impressora | Situação |
|---|---|
| Creality HALOT-R6 | testada: status, envio, imprimir (pausar e parar ainda sem confirmação) |
| Outros modelos Creality HALOT | mesmo protocolo até onde sabemos, sem teste; relatos são bem-vindos |
| Outras marcas | ainda não; veja o [CONTRIBUTING.md](CONTRIBUTING.md) para ajudar a adicionar |

> [!WARNING]
> Ferramenta não oficial, sem vínculo nem aprovação de nenhum fabricante de impressoras. Ela conversa com a impressora
> usando um protocolo descoberto por observação. Confira a cuba de resina e a plataforma antes de
> imprimir, como faria com o software oficial.

## Download

Baixe a versão mais recente na [página de Releases](https://github.com/alexandre-pansan/ResinDeck/releases):

- **Linux:** `ResinDeck-<versão>-x86_64.AppImage`. Torne o arquivo executável (`chmod +x`, ou nas
  propriedades do gerenciador de arquivos) e abra.
- **Windows:** `ResinDeck-<versão>-windows-x64.exe`. O arquivo ainda não é assinado, então o
  SmartScreen do Windows pode avisar: clique em **Mais informações** → **Executar assim mesmo**.

O app roda em segundo plano, com um ícone na bandeja do sistema, e abre a interface no navegador.
Clicar no ícone abre a interface de novo; **Encerrar**, no topo da interface, fecha o ResinDeck. No
Linux, o ícone da bandeja não tem menu, e no GNOME ele precisa da extensão AppIndicator, que já vem
no Ubuntu.

## Instalação a partir do código

Precisa de Python 3.10+ e da impressora na mesma rede que o computador.

```sh
git clone https://github.com/alexandre-pansan/ResinDeck.git
cd ResinDeck
./start.sh
```

Na primeira execução, o `start.sh` cria um virtualenv em `.venv` (com o
[uv](https://docs.astral.sh/uv/), se você tiver, ou com `python3 -m venv`), instala as dependências
e abre a interface em <http://127.0.0.1:8765/>.

Para fazer na mão:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python resindeck.py ui
```

## Usando a interface

1. **Adicione a impressora**: clique em **+ adicionar**, digite o IP (ou use **Procurar na rede**)
   e a senha de rede mostrada na tela da impressora.
2. **Configure a pasta da biblioteca**: clique em **pastas** e informe a pasta onde ficam seus
   `.cxdlpv4`. Também dá para soltar arquivos na janela.
3. **Escolha um arquivo** para ver em 3D, navegar pelas camadas e conferir tempo de impressão,
   resina e exposição.
4. **Enviar** ou **Enviar e imprimir**. Durante a impressão, o cartão da impressora mostra a
   camada, o progresso e o tempo restante; **Acompanhar** mostra no 3D até onde a impressão chegou.

A interface está em inglês e português, escolhido pelo idioma do navegador (dá para trocar no
canto superior direito).

Tudo funciona sem internet, inclusive a visualização 3D.

### Acompanhar a impressão pelo celular

Com o celular no mesmo Wi-Fi, clique em **Celular** no topo e aponte a câmera para o QR code (ou
abra <http://resindeck.local:8765/>), depois digite o PIN de 6 dígitos que aparece ali. O celular vê a
interface completa: progresso, visualização 3D, pausar e parar.

- Só o computador que roda o ResinDeck entra sem PIN, por qualquer um dos seus endereços. A mesma
  janela lista os aparelhos conectados: desconecte um, ou clique em **novo PIN** para desconectar
  todos.
- O `resindeck.local` depende de mDNS: funciona no iPhone, Windows, macOS e Linux, mas não em
  alguns Androids: nesses, abra o endereço IP que aparece embaixo do QR code.
- No Windows, permita o ResinDeck quando o firewall perguntar na primeira execução.
- As configurações das impressoras e as pastas da biblioteca só podem ser mudadas no computador.
- Para deixar o ResinDeck acessível só neste computador, inicie com `RESINDECK_LAN=0`. Mais detalhes
  no [SECURITY.md](SECURITY.md) (em inglês).

## Linha de comando

```sh
resindeck version                       # versão do firmware, testa a conexão
resindeck status                        # estado da impressão
resindeck send peca.cxdlpv4             # só envia
resindeck send peca.cxdlpv4 --print     # envia e imprime (pede confirmação)
resindeck print peca.cxdlpv4            # imprime um arquivo que já está na impressora
resindeck pause                         # pausa / retoma
resindeck stop                          # cancela a impressão
resindeck ui                            # interface web (também é o padrão sem comando)
```

Rode como `.venv/bin/python resindeck.py ...`, ou crie um script `resindeck` no seu `PATH`.

## Configuração

| O quê | Onde |
|---|---|
| Configurações da interface (impressoras, senhas, pastas) | `~/.config/resindeck/settings.json`, permissão 600, editado pela interface |
| Impressora da linha de comando | `~/.config/resindeck/config` (veja `config.example`), ou as variáveis `RESINDECK_HOST` / `RESINDECK_PASSWORD` / `RESINDECK_DRIVER` |
| Cache da visualização 3D | `~/.cache/resindeck/` |

Na primeira execução, a interface importa a impressora da configuração da linha de comando.

A senha da impressora precisa ter de 1 a 8 caracteres. Dá para definir senhas maiores na
impressora, mas elas dão erro de token, inclusive no HALOT BOX.

## Problemas comuns

- **"Senha errada (TOKEN_ERROR)"**: a senha não bate com a da impressora. Confira na tela dela, ou
  defina uma nova lá.
- **"A impressora ainda está recebendo outro arquivo"**: um envio anterior ficou pela metade. Feche
  outros programas conectados à impressora (incluindo outro ResinDeck) ou reinicie a impressora.
- **Impressora aparece offline**: confira o IP e se a porta 18188 está acessível a partir do
  computador.

## Como funciona

Cada família de impressora é um driver em `drivers/`, então adicionar uma marca é escrever um único
módulo. Veja [Adding a printer brand](CONTRIBUTING.md#adding-a-printer-brand) (em inglês).

O protocolo da HALOT é um WebSocket na porta 18188 com comandos JSON e um token baseado em DES; os envios
vão em blocos comprimidos de 1 MiB. Tudo o que se sabe sobre ele está em
[docs/PROTOCOL.md](docs/PROTOCOL.md) (em inglês).

| Arquivo | O que faz |
|---|---|
| `resindeck.py` | linha de comando e ponto de entrada |
| `drivers/` | um módulo por família de impressora. `base.py` é o contrato, `halot.py` o driver das Creality HALOT |
| `formats/` | leitores de arquivos fatiados. `cxdlp.py` lê `.cxdlpv4`: cabeçalho, miniaturas, camadas, voxels do 3D |
| `server.py` | servidor web local (aiohttp): biblioteca, visualizações, conexões com as impressoras |
| `desktop.py` | ponto de entrada do app desktop: servidor em segundo plano e ícone na bandeja |
| `static/` | interface web (JavaScript puro, three.js no 3D, `i18n.js` com as traduções) |
| `packaging/` | ícone, receita do PyInstaller e script do AppImage |

## Contribuindo

Relatos de bugs, testes com outros modelos HALOT e traduções são muito bem-vindos. Veja o
[CONTRIBUTING.md](CONTRIBUTING.md) (em inglês).

## Créditos

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp), de mrtska (MIT): base do
  protocolo de rede e do algoritmo do token.
- [UVtools](https://github.com/sn4k3/UVtools), de Tiago Conceição: documentação do formato
  `.cxdlpv4`. O ResinDeck tem implementação própria em Python e não contém código do UVtools.
- [three.js](https://threejs.org/) (MIT) para a visualização 3D, incluído em `static/vendor/three/`.

## Licença

[MIT](LICENSE). "Creality" e "HALOT" são marcas de seus respectivos donos e aparecem aqui apenas
para descrever compatibilidade.

> E sim, isso aqui foi feito inteirinho usando o Claude (ou Claudinho, para os íntimos). A única parte feita por um humano é esta, que é mais ou menos assim: dabadee dabadaa.
> Sou totalmente a favor de usar IA para criar builds, apps, sonhos, modelos, logos e imagens, principalmente quando é preciso pesquisar a fundo ou quando a gente não entende direito como algo funciona. Fique à vontade para usar, só não esqueça de testar tudo e de ser gentil com a IA. Afinal, nunca se sabe quando a Skynet vai vir atrás da gente :D.
> Eu não entendo muito bem como essa comunicação funciona nem como fazer isso. O máximo que fiz foi apontar umas melhorias na interface, então não me julgue! Só estou aqui para facilitar a vida de quem usa Unix na hora de imprimir em 3D. (Afinal, rodar nativo é melhor do que ficar no WINEzado!)
