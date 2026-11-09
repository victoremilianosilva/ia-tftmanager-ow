# Documentação técnica

Documentação da engine C++ e do runtime de inferência.

## Comece por aqui

- [Arquitetura geral](architecture.md) — componentes e fluxo de dados.
- [Engine de lobby](lobby-engine.md) — ciclo de partida e estado dos oito jogadores.
- [Catálogo e instâncias de unidades](units-catalog.md) — definições estáticas, cópias, estrelas e pool.
- [Economia e loja](economy-shop.md) — ouro, XP, juros, reroll e ofertas.
- [Ações, máscaras e anti-loop](decisions-masks-antiloop.md) — contrato da policy e validação.
- [Combate](combat.md) — entradas, simulação e eventos.
- [Recompensas e observações](rewards-observations.md) — sinais e estado observado pela policy.
- [Estratégia, sinergias e posicionamento](strategy-synergies-positioning.md).
- [Ambiente Python e bindings](python-environments.md).
- [Build, testes e CI](build-and-tests.md).
- [Contratos e dados](contracts-and-data.md).
- [Catálogo de meta](opgg-meta-catalog.md).

## Princípios

1. A engine C++ é a fonte de verdade para as regras da partida e os resultados das ações.
2. O runtime Python carrega checkpoints treinados e executa inferência; não realiza treinamento nem otimização PPO.
3. As dimensões de ações, observações e máscaras são contratos entre policy e engine; mudanças precisam ser validadas em conjunto.
4. O treinamento, fine-tuning e avaliação experimental pertencem ao repositório principal `ia-tft-manager`.

## Entradas principais

- Policy: `policy.py`
- Adaptador nativo: `cpp_lobby_env.py`
- Engine: `cpp/include/lobby/TftEngine.hpp` e implementações em `cpp/src/`
