# Contratos, enums e formatos de dados

## Contrato de ação

`engine_contracts.py` mantém IDs Python estáveis para tipos de ação e estratégia. O C++ possui contratos correspondentes em `DecisionTypes.hpp` e `TftEngine.hpp`. Qualquer alteração precisa ser refletida em policy, máscaras, conversores, buffer, bindings e testes.

| Constante | Valor |
|---|---:|
| Jogadores por lobby | 8 |
| Posições de tabuleiro | 28 (4 × 7) |
| Slots de banco | 9 |
| Slots de loja | 5 |
| Componentes da ação | 6 |
| Dimensões atuais das cabeças | [9, 5, 9, 28, 28, 28] |
| Dimensões legadas aceitas pela migração explícita | [7, 5, 9, 28, 28, 28] |

A primeira cabeça atual contém nove tipos de ação (IDs 0–8 em `ActionType`), incluindo as operações de item. As operações de item usam cabeças/máscaras condicionais adicionais; isso não altera o vetor externo de seis componentes. A dimensão antiga de sete tipos é uma origem histórica de checkpoint, não o contrato atual.

## Máscaras

As máscaras booleanas produzidas pela engine chegam ao PyTorch durante a inferência. Matrizes condicionais representam origem × destino, e o tipo da ação determina quais componentes têm significado. Policy e engine precisam compartilhar a mesma convenção de índices. Máscaras nativas permanecem autoridade de legalidade; validação de snapshot GEP não as substitui.

## Catálogo de unidades

`units_gerado.json` é uma lista de definições de campeões. A engine lê campos como `apiName`, `name`, `cost`, `traits`, `stats`, `combat_profile`, habilidade e ícones. O viewer usa aliases do catálogo para tornar snapshots legíveis.

## Logs do viewer

O schema de sessão atual é versão 1. `metadata.json` guarda metadados da partida; cada log de jogador contém entries e eventos com `sequence_id`, `timestamp_iso`, `round`, `stage`, `event_type`, `text` e `payload`. O replay valida versão e ordem dos IDs.

## Alterar contratos com segurança

1. Mude o tipo/shape na definição de origem.
2. Atualize binding e wrapper.
3. Atualize a policy e os adaptadores de observação/inferência; o contrato deve continuar alinhado com as dimensões nativas.
4. Adicione fixtures de equivalência e casos inválidos.
5. Atualize esta documentação e os documentos funcionais afetados.
6. Execute CTest e testes Python relacionados.
