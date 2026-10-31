# ResinDeck

Um painel de controle open source para impressoras 3D de resina: veja seus arquivos fatiados em
3D, envie para a impressora pela rede e acompanhe a impressão, do seu próprio computador.

Hoje suporta impressoras **Creality HALOT**, direto do Linux, sem HALOT BOX e sem Wine nem Proton.
A ideia é crescer para outras marcas e sistemas com a ajuda da comunidade.

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

## Instalação

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

O servidor só escuta em `127.0.0.1`, então não fica acessível para outras máquinas. A visualização
3D carrega o [three.js](https://threejs.org/) de uma CDN; sem internet, as abas Camadas e Miniatura
continuam funcionando.

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
| Impressora da linha de comando | `~/.config/resindeck/config` (veja `config.example`), ou as variáveis `RESINDECK_HOST` / `RESINDECK_PASSWORD` |
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

O protocolo da HALOT é um WebSocket na porta 18188 com comandos JSON e um token baseado em DES; os envios
vão em blocos comprimidos de 1 MiB. Tudo o que se sabe sobre ele está em
[docs/PROTOCOL.md](docs/PROTOCOL.md) (em inglês).

| Arquivo | O que faz |
|---|---|
| `resindeck.py` | linha de comando e ponto de entrada |
| `halot.py` | driver das Creality HALOT (protocolo de rede) |
| `server.py` | servidor web local (aiohttp): biblioteca, visualizações, conexões com as impressoras |
| `cxdlp.py` | leitor de `.cxdlpv4`: cabeçalho, miniaturas, imagens das camadas, voxels para o 3D |
| `static/` | interface web (JavaScript puro, three.js no 3D, `i18n.js` com as traduções) |

## Contribuindo

Relatos de bugs, testes com outros modelos HALOT e traduções são muito bem-vindos. Veja o
[CONTRIBUTING.md](CONTRIBUTING.md) (em inglês).

## Créditos

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp), de mrtska (MIT): base do
  protocolo de rede e do algoritmo do token.
- [UVtools](https://github.com/sn4k3/UVtools), de Tiago Conceição: documentação do formato
  `.cxdlpv4`. O ResinDeck tem implementação própria em Python e não contém código do UVtools.
- [three.js](https://threejs.org/) (MIT) para a visualização 3D.

## Licença

[MIT](LICENSE). "Creality" e "HALOT" são marcas de seus respectivos donos e aparecem aqui apenas
para descrever compatibilidade.
