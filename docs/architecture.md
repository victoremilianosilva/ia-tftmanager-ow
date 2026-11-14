# Arquitetura geral

## Visão por camadas

```text
processo hospedeiro de inferência
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

O caminho de inferência da policy consome observações e máscaras fornecidas pela engine nativa. `engine_contracts.py` mantém enums estáveis entre Python e C++; não implementa as regras do jogo.

## Responsabilidades

- **Engine C++ (`cpp/include`, `cpp/src`)**: estado da lobby, ações, economia, loja, unidades, estoque, combate, recompensas, máscaras, validação de movimento e snapshots.
- **Bindings (`cpp/bindings`, `cpp/src/decision/decision_bindings.cpp`)**: expõem a API nativa para Python.
- **Adaptador Python (`cpp_lobby_env.py`)**: normaliza entradas/saídas e conecta a engine nativa ao processo de inferência.
- **Policy (`policy.py`)**: carrega pesos de checkpoints treinados e escolhe ações autoregressivas condicionadas pelas máscaras da engine.
- **Hospedeiro de runtime**: carrega o checkpoint e chama a policy para cada decisão; não atualiza os pesos.
- **Viewer (`viewer/`)**: carrega checkpoint para avaliação, chama a engine existente, mostra snapshots e registra sessões; não treina a policy.
- **Validação live (`live_inference/gep_snapshot.py`, `live_inference/service.py`)**: valida e sequencia o envelope canônico vindo do Overwolf. O receiver não constrói observações/máscaras nativas e informa `inferenceAvailable=false`.
- **DTO C++ live (`cpp/include/lobby/LiveState.hpp`)**: representa e valida o contrato normalizado; não importa/muta o estado interno de `TFTEngine`.
- **Testes (`tests/`)**: validam engine, bindings, contratos, inferência e receiver de snapshots.

## Ciclo de uma ação (engine/simulação)

1. A engine disponibiliza observações e máscaras para cada jogador.
2. A policy escolhe o tipo de ação e os componentes condicionais válidos.
3. O adaptador transforma as ações no formato esperado pelo binding.
4. A engine valida e aplica as oito ações da lobby no passo nativo conjunto.
5. A engine resolve eventos da rodada conforme seu ciclo interno e retorna recompensas, observações e informação de auditoria.
6. O treinador guarda a transição no buffer; o viewer, quando usado, persiste a linha do tempo da sessão.

Esse ciclo descreve a integração com a engine/simulação. Ele **não** deve ser reutilizado como prova de que snapshots GEP já são importados para uma partida live.

## Fronteira de estado live — status atual

- O repositório Overwolf interpreta GEP, reconcilia baseline/deltas e grava dois JSONL locais: eventos GEP sanitizados e envelopes canônicos.
- O aplicativo Overwolf atualmente não envia esses snapshots ao Python; captura e persistência local não equivalem a transporte.
- O receiver Python valida schema, privacidade e sequência. Seus endpoints não invocam a engine nem a policy.
- `PPOInferenceRunner` consome uma observação e as máscaras nativas já construídas; não converte `tft.live-state` em estado Engine.
- O DTO C++ valida campos normalizados, mas não existe API de produção que reconcilie board, bench, shop, itens, economia, pool, estado estratégico e máscaras dentro de `TFTEngine`.
- Até existir importer, transporte, proveniência por campo e replay end-to-end aprovados, não chamar observações/máscaras simuladas de live nem declarar inferência live.

A integração live permanece read-only: não usar `reset()`, `step()` ou setters de teste para importar snapshot, e nunca executar a ação recomendada no jogo.

## Contratos críticos

- Uma lobby contém 8 jogadores.
- Tabuleiro: 28 posições (4 × 7); banco: 9 slots; loja: 5 slots.
- A ação é um vetor de 6 componentes, com dimensões atuais `[9, 5, 9, 28, 28, 28]`.
- A dimensão legada `[7, 5, 9, 28, 28, 28]` é tratada apenas pela lógica explícita de migração de checkpoints antigos; não é o contrato atual.
- As máscaras condicionais de movimento têm formato `[origem, destino]`.
- O estado terminal precisa preservar ranking/colocações antes de qualquer reset automático do ambiente vetorizado.

Detalhes: [contratos e dados](contracts-and-data.md), [mapeamento live GEP](GEP_MAPPING.md), [auditoria de importação nativa](NATIVE_ENGINE_LIVE_IMPORT_AUDIT.md), [ambientes Python](python-environments.md), [treinamento](rl-training.md).
