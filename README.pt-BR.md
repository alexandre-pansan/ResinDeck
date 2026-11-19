# ResinDeck

[![CI](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml/badge.svg)](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml)

Um painel de controle open source para impressoras 3D de resina: veja seus arquivos fatiados em
3D, envie para a impressora pela rede e acompanhe a impressão, do seu próprio computador.

Hoje suporta impressoras **Creality HALOT**, direto do Linux, sem HALOT BOX e sem Wine nem Proton.
A ideia é centralizar num só lugar a comunicação em rede com impressoras 3D de resina, crescendo
para outras marcas e sistemas com a ajuda da comunidade.

[Read in English](README.md)

- **Programa para Linux e Windows:**
  - visualização 3D montada a partir das camadas do arquivo fatiado, desenhada pela placa de vídeo, suave e
    detalhada até o pixel, com corte em qualquer camada
  - visualizador camada por camada e a miniatura do fatiador, tempo de impressão, resina e exposição
  - gerenciador de impressoras: várias impressoras, busca na rede, status ao vivo com camada, tempo
    restante, hora de início e término previsto
  - enviar, enviar e imprimir, pausar/retomar, parar, com o progresso do envio
- **Acesso pelo celular e pelo navegador** na sua rede, com QR code e PIN, para acompanhar e controlar a
  impressão.
- **Linha de comando** (`resindeck-cli`) para scripts e tarefas rápidas.
- Lê arquivos `.cxdlpv4`, o formato que o HALOT BOX e o Chitubox exportam para impressoras HALOT.

## Impressoras suportadas

| Impressora | Situação |
|---|---|
| Creality HALOT-R6 | testada: status, envio, imprimir, pausar, retomar, parar |
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
- **Windows:** `ResinDeck-<versão>-windows-x64-setup.exe` instala com atalho no menu Iniciar, ou
  `ResinDeck-<versão>-windows-x64.zip` roda sem instalar (descompacte e abra o `resindeck.exe`). Os arquivos
  ainda não são assinados, então o SmartScreen do Windows pode avisar: clique em **Mais informações** →
  **Executar assim mesmo**. Permita o ResinDeck quando o firewall perguntar, para o celular conseguir
  conectar.

## Compilar a partir do código

No Linux:

```sh
git clone https://github.com/alexandre-pansan/ResinDeck.git
cd ResinDeck
./build.sh setup   # só na primeira vez: CMake, Ninja e Qt, sem precisar de admin
./build.sh run     # compila e abre o ResinDeck
```

O Qt precisa dos arquivos de desenvolvimento do OpenGL: `sudo apt install libgl-dev libegl-dev` no Debian e
no Ubuntu. Outros sistemas e detalhes no [CONTRIBUTING.md](CONTRIBUTING.md#building) (em inglês).

## Usando o ResinDeck

1. **Adicione a impressora**: clique em **+ adicionar**, digite o IP (ou use **Procurar na rede**)
   e a senha de rede mostrada na tela da impressora.
2. **Configure a pasta da biblioteca**: clique em **pastas** e escolha a pasta onde ficam seus
   `.cxdlpv4`. Também dá para soltar arquivos na janela, ou abrir um com o ResinDeck pelo gerenciador de
   arquivos.
3. **Escolha um arquivo** para ver em 3D, navegar pelas camadas e conferir tempo de impressão,
   resina e exposição. Arraste para girar o modelo, arraste com o botão direito para mover, use a rodinha
   para o zoom e clique duas vezes para voltar à vista inicial.
4. **Enviar** ou **Enviar e imprimir**. Durante a impressão, o cartão da impressora mostra a camada, o
   tempo restante, a hora em que começou e quando deve terminar.

A janela está em inglês e português, escolhido pelo idioma do sistema (dá para trocar no topo).
Tudo funciona sem internet, inclusive a visualização 3D.

### Acompanhar a impressão pelo celular

Com o ResinDeck aberto, celulares e outros computadores no mesmo Wi-Fi acompanham a impressão pelo navegador.
Clique em **Celular** no topo e aponte a câmera para o QR code (ou abra <http://resindeck.local:8765/>),
depois digite o PIN de 6 dígitos que aparece ali. O celular vê o progresso e o 3D, e pode pausar e parar.

- Só o computador que roda o ResinDeck entra sem PIN, por qualquer um dos seus endereços. A mesma
  janela lista os aparelhos conectados: desconecte um, ou clique em **novo PIN** para desconectar
  todos.
- O `resindeck.local` depende de mDNS: funciona no iPhone, Windows, macOS e Linux, mas não em
  alguns Androids: nesses, abra o endereço IP que aparece embaixo do QR code.
- As configurações das impressoras e as pastas da biblioteca só podem ser mudadas no computador.
- Para deixar o ResinDeck acessível só neste computador, inicie com `RESINDECK_LAN=0`. Mais detalhes
  no [SECURITY.md](SECURITY.md) (em inglês).

## Linha de comando

```sh
resindeck-cli version                       # versão do firmware, testa a conexão
resindeck-cli status                        # estado da impressão
resindeck-cli send peca.cxdlpv4             # só envia
resindeck-cli send peca.cxdlpv4 --print     # envia e imprime (pede confirmação)
resindeck-cli print peca.cxdlpv4            # imprime um arquivo que já está na impressora
resindeck-cli pause                         # pausa / retoma
resindeck-cli stop                          # cancela a impressão
resindeck-cli serve                         # a interface do celular e do navegador, sem a janela
```

Ele fica ao lado do `resindeck` no AppImage (use `--appimage-extract` para tirá-lo de lá) e na pasta do
Windows, e em `build/release/app/cli/` quando compilado a partir do código.

## Configuração

| O quê | Onde |
|---|---|
| Impressoras, senhas, pastas da biblioteca, celulares pareados | `~/.config/resindeck/settings.json`, permissão 600, editado pela janela |
| Impressora da linha de comando | `~/.config/resindeck/config` (veja `config.example`), ou as variáveis `RESINDECK_HOST` / `RESINDECK_PASSWORD` / `RESINDECK_DRIVER` |
| Cache da visualização 3D | `~/.cache/resindeck/` |

Na primeira execução, o ResinDeck importa a impressora da configuração da linha de comando.

Não é preciso fixar o IP da impressora. O ResinDeck guarda o endereço MAC dela, e quando ela para de
responder, procura esse MAC na rede e atualiza o IP sozinho. Isso funciona com a impressora e o
computador na mesma rede, o caso normal em casa.

A senha da impressora precisa ter de 1 a 8 caracteres. Dá para definir senhas maiores na
impressora, mas elas dão erro de token, inclusive no HALOT BOX.

## Problemas comuns

- **"Senha errada (TOKEN_ERROR)"**: a senha não bate com a da impressora. Confira na tela dela, ou
  defina uma nova lá.
- **"A impressora ainda está recebendo outro arquivo"**: um envio anterior ficou pela metade. Feche
  outros programas conectados à impressora (incluindo outro ResinDeck) ou reinicie a impressora.
- **"A porta 8765 está em uso"**: outro ResinDeck já está aberto. A janela funciona, mas o celular conecta
  no outro.
- **Impressora aparece offline**: confira o IP e se a porta 18188 está acessível a partir do
  computador.

## Como funciona

O ResinDeck é escrito em C++ com Qt. Cada família de impressora é um driver, então adicionar uma marca é
escrever uma única classe; veja [Adding a printer brand](CONTRIBUTING.md#adding-a-printer-brand) (em
inglês). O 3D extrai uma superfície das próprias imagens das camadas, usando o antialiasing delas para
posicionar as bordas entre os pixels.

O protocolo da HALOT é um WebSocket na porta 18188 com comandos JSON e um token baseado em DES; os envios
vão em blocos comprimidos de 1 MiB. Tudo o que se sabe sobre ele está em
[docs/PROTOCOL.md](docs/PROTOCOL.md) (em inglês).

| Pasta | O que faz |
|---|---|
| `core/` | impressoras e drivers, formatos de arquivo, malhas 3D, o servidor web do celular, rede, com testes |
| `app/desktop/` | a janela (Qt Widgets) e o 3D na placa de vídeo |
| `app/cli/` | a linha de comando |
| `static/` | a interface do celular e do navegador (JavaScript puro, three.js, `i18n.js` com as traduções) |
| `packaging/` | ícone e os pacotes do Linux e do Windows |

A versão 0.x era em Python; ela fica guardada na branch `legacy`.

## Contribuindo

Relatos de bugs, testes com outros modelos HALOT e traduções são muito bem-vindos. Veja o
[CONTRIBUTING.md](CONTRIBUTING.md) (em inglês).

## Próximos passos

O ResinDeck vai virar um fatiador de resina completo: abrir modelos, ocar, suportes, fatiar e imprimir num
lugar só. As fases estão no [docs/ROADMAP.md](docs/ROADMAP.md) (em inglês).

## Créditos

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp), de mrtska (MIT): base do
  protocolo de rede e do algoritmo do token.
- [UVtools](https://github.com/sn4k3/UVtools), de Tiago Conceição: documentação do formato
  `.cxdlpv4`. O ResinDeck tem implementação própria e não contém código do UVtools.
- [Qt](https://www.qt.io/) (LGPL 3) para a janela, a rede e o 3D.
- [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library), do Project Nayuki (MIT),
  em `third_party/qrcodegen/`.
- [three.js](https://threejs.org/) (MIT) para o 3D da interface do celular e do navegador, incluído em
  `static/vendor/three/`.

## Licença

[MIT](LICENSE). "Creality" e "HALOT" são marcas de seus respectivos donos e aparecem aqui apenas
para descrever compatibilidade.

> E sim, isso aqui foi feito inteirinho usando o Claude (ou Claudinho, para os íntimos). A única parte feita por um humano é esta, que é mais ou menos assim: dabadee dabadaa.
> Sou totalmente a favor de usar IA para criar builds, apps, sonhos, modelos, logos e imagens, principalmente quando é preciso pesquisar a fundo ou quando a gente não entende direito como algo funciona. Fique à vontade para usar, só não esqueça de testar tudo e de ser gentil com a IA. Afinal, nunca se sabe quando a Skynet vai vir atrás da gente :D.
> Eu não entendo muito bem como essa comunicação funciona nem como fazer isso. O máximo que fiz foi apontar umas melhorias na interface, então não me julgue! Só estou aqui para facilitar a vida de quem usa Unix na hora de imprimir em 3D. (Afinal, rodar nativo é melhor do que ficar no WINEzado!)
