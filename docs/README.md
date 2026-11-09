# Documentação técnica

Esta documentação descreve o código presente no repositório, separando arquitetura, contratos e funcionalidades.

## Comece por aqui

- [Arquitetura geral](architecture.md) — componentes e fluxo de dados.
- [Engine de lobby](lobby-engine.md) — ciclo de partida e estado dos oito jogadores.
- [Catálogo e instâncias de unidades](units-catalog.md) — definições estáticas, cópias, estrelas e pool.
- [Economia e loja](economy-shop.md) — ouro, XP, juros, reroll e ofertas.
- [Ações, máscaras e anti-loop](decisions-masks-antiloop.md) — contrato da policy e validação.
- [Combate](combat.md) — entradas, simulação e eventos.
- [Recompensas e observações](rewards-observations.md) — sinais de treino e estado observado.
- [Estratégia, sinergias e posicionamento](strategy-synergies-positioning.md).
- [Treinamento PPO](rl-training.md) — coleta, rollout, otimização e checkpoints.
- [Ambientes Python e bindings](python-environments.md).
- [Avaliação e ferramentas](evaluation-tooling.md).
- [Build, testes e CI](build-and-tests.md).
- [Contratos e dados](contracts-and-data.md).

## Princípios usados nesta documentação

1. A engine C++ é a fonte de verdade para as regras da partida e os resultados das ações.
2. Python/PyTorch controla treinamento, coleta de experiências, inferência e ferramentas de avaliação.
3. As dimensões de ações, observações e máscaras são contratos entre policy e engine; mudanças precisam ser validadas em conjunto.
5. Uma funcionalidade descrita aqui não é considerada testada só por estar documentada. Veja os comandos e limitações em [build e testes](build-and-tests.md).

## Entradas principais

- Treinamento: `trainer.py`
- Policy: `policy.py`
- PPO: `ppo.py`
- Buffer: `rollout.py`
- Adaptador nativo: `cpp_lobby_env.py`
- Engine: `cpp/include/lobby/TftEngine.hpp` e implementações em `cpp/src/`
