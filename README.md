# xbox360-wifi-11g

O Wi-Fi interno do Xbox 360 S/E **recebe** só uns 4 Mb/s com muitos roteadores atuais. Este repositório explica por quê e traz o ajuste que usamos no [orbita](https://x360.pistatrack.com) (cliente de xCloud pro 360). Com ele, o console passa a receber **11–13 Mb/s** pelo mesmo Wi-Fi.

*English summary at the end.*

## O problema

Medimos com o próprio console:

| Direção | Velocidade |
|---|---|
| PC → Xbox (Xbox recebendo) | **4,3 Mb/s** |
| Xbox → PC (Xbox enviando) | 32 Mb/s |

Foi medido de dois jeitos, com o mesmo resultado:
- **FTP do Aurora**, sem nenhum homebrew rodando. Ou seja, o limite é do sistema, não de um app.
- **Um laboratório dentro de um XEX:** o PC mandava UDP e TCP a até 20 Mb/s, e a placa nunca entregou mais que ~450 pacotes por segundo.

A conta fecha com o **roteador mandando pro Xbox na taxa mínima do Wi-Fi, 6 Mb/s**. Um pacote de 1500 bytes a 6 Mb/s ocupa ~2,2 ms no ar, o que dá ~460 pacotes/s, exatamente o teto medido. O sinal estava bom (RSSI −56 dBm) e o próprio Xbox transmitia em 802.11n MCS7 (72 Mb/s). O lento era só a direção roteador → Xbox.

Já existia relato disso: o tópico do SNBForums [*"Xbox 360 Slim – 11n slow, 11g normal"*](https://www.snbforums.com/threads/xbox-360-slim-11n-slow-11g-normal.10040/). Em 802.11n fica lento, em 802.11g é normal. Trocar o roteador pra só "g" resolve, mas atrapalha todos os outros aparelhos da casa.

## A causa, dentro do console

- O driver Wi-Fi do sistema fica dentro do kernel: o "Omni", do chip Marvell 88W8786U.
- Ele decide se associa em 11n por um bit (`0x800`) nas capacidades do adaptador, guardadas na estrutura global dele.
- Esse bit ligado = associa em 11n = o roteador manda pro console na taxa mínima.

Pra achar isso usamos o kernel de debug que vem no XDK, que tem símbolos. Não distribuímos nada dele aqui. Funções relevantes:
- `OmniDoGetOpt` / `OmniDoSetOpt`: as opções do `XNetGetOpt` / `XNetSetOpt` que o driver atende.
- `Omni80211GetCurrentRate`: a opção `0x2B00`.
- `Omni80211SetTxPowerLevel`: a opção `0x0D010205`, que termina chamando `OmniReconfigure`.
- `OmniNextConfigStage`: a etapa `Enable11N` e a escolha de associar com HT.

## O ajuste

Arquivo: [`wifi_force_g.c`](wifi_force_g.c).

1. **Achar a estrutura do driver na memória**, sem endereço fixo, então funciona em qualquer versão do kernel:
   - Procurar no código do kernel a instrução `cmplwi cr6, r4, 0x2B00` (`0x2B042B00`).
   - Logo depois vem um `bl` pra `Omni80211GetCurrentRate`, que começa com `lis` + `addi` carregando o endereço da estrutura. Decodificamos essas duas instruções.
2. **Conferir que é mesmo a estrutura certa** (tipo do aparelho e formato das capacidades). Se não bater, não mexe em nada.
3. **Apagar o bit `0x800`** (11n) nas capacidades (`+0x00C`).
4. **Pedir ao driver pra se reconfigurar:** reaplicar a mesma potência de transmissão com `XNetSetOpt(0x0D010205, …)`. Ele reassocia, agora em **802.11g**.
5. **Esperar o IP voltar**, uns 1–3 s. Confirmação: o bit `0x4000` em `+0x478` (associado com HT) apaga.

Detalhes:
- **É só memória:** vale até desligar o console. Não grava nada na flash nem nas configurações de rede.
- **Requisitos:** um XEX compilado com o XDK, rodando num console onde o título pode ler e escrever dados do kernel (RGH/JTAG).
- **Testado com:** kernel 17559, 360 S com Wi-Fi interno e um roteador Wi-Fi 6 em 2,4 GHz.

## Como usar

```c
int wifi_force_g(void);   /* 1 = em 11g (ou já estava), -1 = não fez */

XNetStartup(&params);
WSAStartup(MAKEWORD(2, 2), &wsa);
/* ...esperar ter IP... */
wifi_force_g();
```

Linkar com `xboxkrnl.lib` (por causa do `MmIsAddressValid`) e `xnet.lib`.

## Como conferir

1. **Antes:** pelo FTP do Aurora, mande um arquivo grande do PC pro Xbox e anote a velocidade.
2. Rode um XEX que chame `wifi_force_g()`.
3. **Depois:** volte pro Aurora sem desligar o console, porque o ajuste continua valendo, e repita o envio. Esperado: ~4 Mb/s antes e 11–13 Mb/s depois.

## English summary

On many current routers, the Xbox 360 S/E internal Wi-Fi associates in 802.11n and the router then transmits to the console at the 6 Mb/s base rate. The console *receives* only ~4.3 Mb/s, while it still *sends* at ~32 Mb/s. Aurora's FTP shows the same, with no homebrew running.

The system driver ("Omni", Marvell, inside the kernel) enables 11n from bit `0x800` of the adapter capabilities in its global state. `wifi_force_g.c` does the following:
- locates that state without hard-coded addresses (from the `0x2B00` option handler → `Omni80211GetCurrentRate` → `lis`/`addi`);
- clears the bit;
- re-applies the TX power level with `XNetSetOpt(0x0D010205)`, which makes the driver reconfigure and re-associate in 802.11g.

Result: 11–13 Mb/s receive. The change is memory-only and lasts until power-off. It requires an XDK-built XEX on an RGH/JTAG console.

## Licença

MIT. Veja [LICENSE](LICENSE).
