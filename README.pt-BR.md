# halot-send

Envie arquivos fatiados para impressoras de resina Creality HALOT pela rede, direto do Linux.
Sem HALOT BOX, sem Wine nem Proton.

[Read in English](README.md)

- **Interface web** (roda localmente, abre no navegador):
  - visualização 3D montada a partir das camadas do arquivo fatiado
  - visualizador camada por camada e a miniatura do fatiador
  - gerenciador de impressoras: várias impressoras, busca na rede, status ao vivo
  - enviar, enviar e imprimir, pausar/retomar, parar, com progresso por camada e tempo restante
- **Linha de comando** para scripts e tarefas rápidas.
- Lê arquivos `.cxdlpv4`, o formato que o HALOT BOX exporta para impressoras HALOT.

Testado com a **HALOT-R6**. Outros modelos HALOT usam o mesmo protocolo até onde sabemos
(a base do protocolo vem de um projeto para a HALOT Mage Pro), mas não foram testados.
Relatos são bem-vindos.

> [!WARNING]
> Ferramenta não oficial, sem vínculo nem aprovação da Creality. Ela conversa com a impressora
> usando um protocolo descoberto por observação. Confira a cuba de resina e a plataforma antes de
> imprimir, como faria com o software oficial.

## Instalação

Precisa de Python 3.10+ e da impressora na mesma rede que o computador.

```sh
git clone https://github.com/alexandre-pansan/halot-send.git
cd halot-send
./start.sh
```

Na primeira execução, o `start.sh` cria um virtualenv em `.venv` (com o
[uv](https://docs.astral.sh/uv/), se você tiver, ou com `python3 -m venv`), instala as dependências
e abre a interface em <http://127.0.0.1:8765/>.

Para fazer na mão:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python halot_send.py ui
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
halot-send version                       # versão do firmware, testa a conexão
halot-send status                        # estado da impressão
halot-send send peca.cxdlpv4             # só envia
halot-send send peca.cxdlpv4 --print     # envia e imprime (pede confirmação)
halot-send print peca.cxdlpv4            # imprime um arquivo que já está na impressora
halot-send pause                         # pausa / retoma
halot-send stop                          # cancela a impressão
halot-send ui                            # interface web (também é o padrão sem comando)
```

Rode como `.venv/bin/python halot_send.py ...`, ou crie um script `halot-send` no seu `PATH`.

## Configuração

| O quê | Onde |
|---|---|
| Configurações da interface (impressoras, senhas, pastas) | `~/.config/halot-send/settings.json`, permissão 600, editado pela interface |
| Impressora da linha de comando | `~/.config/halot-send/config` (veja `config.example`), ou as variáveis `HALOT_HOST` / `HALOT_PASSWORD` |
| Cache da visualização 3D | `~/.cache/halot-send/` |

Na primeira execução, a interface importa a impressora da configuração da linha de comando.

A senha da impressora precisa ter de 1 a 8 caracteres. Dá para definir senhas maiores na
impressora, mas elas dão erro de token, inclusive no HALOT BOX.

## Problemas comuns

- **"Senha errada (TOKEN_ERROR)"**: a senha não bate com a da impressora. Confira na tela dela, ou
  defina uma nova lá.
- **"A impressora ainda está recebendo outro arquivo"**: um envio anterior ficou pela metade. Feche
  outros programas conectados à impressora (incluindo outro halot-send) ou reinicie a impressora.
- **Impressora aparece offline**: confira o IP e se a porta 18188 está acessível a partir do
  computador.

## Como funciona

O protocolo é um WebSocket na porta 18188 com comandos JSON e um token baseado em DES; os envios
vão em blocos comprimidos de 1 MiB. Tudo o que se sabe sobre ele está em
[docs/PROTOCOL.md](docs/PROTOCOL.md) (em inglês).

| Arquivo | O que faz |
|---|---|
| `halot_send.py` | protocolo da impressora e linha de comando |
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
  `.cxdlpv4`. O halot-send tem implementação própria em Python e não contém código do UVtools.
- [three.js](https://threejs.org/) (MIT) para a visualização 3D.

## Licença

[MIT](LICENSE). "Creality" e "HALOT" são marcas de seus respectivos donos e aparecem aqui apenas
para descrever compatibilidade.
