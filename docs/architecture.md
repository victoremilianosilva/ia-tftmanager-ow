# Arquitetura geral

## Visão por camadas

```text
processo hospedeiro de produção
                |
        TFTPolicy (PyTorch)
                |
   CppTFTLobbyWrapper (Python)
                |
         pybind11 bindings
                |
          TFTEngine (C++)
   ┌────────────┼───────────────┐
 lobby/economia/loja       decisões/máscaras
   combate/recompensas     estratégia/posicionamento
                |
       observações, rewards, dones,
       estado público, snapshots e eventos
```

O caminho principal de execução passa pela engine nativa. `engine_contracts.py` mantém enums estáveis entre Python e C++; não implementa as regras do jogo.

## Responsabilidades

- **Engine C++ (`cpp/include`, `cpp/src`)**: estado da lobby, ações, economia, loja, unidades, estoque, combate, recompensas, máscaras, validação de movimento e snapshots.
- **Bindings (`cpp/bindings`, `cpp/src/decision/decision_bindings.cpp`)**: expõem a API nativa para Python.
- **Adaptador Python (`cpp_lobby_env.py`)**: normaliza entradas/saídas e conecta a engine nativa ao processo de inferência.
- **Policy (`policy.py`)**: carrega pesos de checkpoints treinados e escolhe ações autoregressivas condicionadas pelas máscaras da engine.
- **Hospedeiro de runtime**: carrega o checkpoint e chama a policy para cada decisão; não atualiza os pesos.
- **Viewer (`viewer/`)**: carrega checkpoint para avaliação, chama a engine existente, mostra snapshots e registra sessões; não treina a policy.
- **Testes (`tests/`)**: validam engine, bindings, contratos e inferência.

## Ciclo de uma ação

1. A engine disponibiliza observações e máscaras para cada jogador.
2. A policy escolhe o tipo de ação e os componentes condicionais válidos.
3. O adaptador transforma as ações no formato esperado pelo binding.
4. A engine valida e aplica as oito ações da lobby no passo nativo conjunto.
5. A engine resolve eventos da rodada conforme seu ciclo interno e retorna recompensas, observações e informação de auditoria.
6. O treinador guarda a transição no buffer; o viewer, quando usado, persiste a linha do tempo da sessão.

## Contratos críticos

- Uma lobby contém 8 jogadores.
- Tabuleiro: 28 posições (4 × 7); banco: 9 slots; loja: 5 slots.
- A ação é um vetor de 6 componentes, com dimensões `[7, 5, 9, 28, 28, 28]`.
- As máscaras condicionais de movimento têm formato `[origem, destino]`.
- O estado terminal precisa preservar ranking/colocações antes de qualquer reset automático do ambiente vetorizado.

Detalhes: [contratos e dados](contracts-and-data.md), [ambientes Python](python-environments.md), [treinamento](rl-training.md).
