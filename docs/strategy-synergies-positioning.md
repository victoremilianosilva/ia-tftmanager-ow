# Estratégia, sinergias e posicionamento

## Posicionamento

`PositioningEngine` avalia a formação no tabuleiro 4 × 7 usando perfis/roles e posições. A linha 0 é tratada como frontline e a linha 3 como backline. A avaliação considera proteção de carries: tanques oferecem a proteção mais forte; fighters à frente contam como proteção parcial. O classificador de papéis usa fighter como desempate quando faltam sinais, evitando que um perfil sem dados vire tank por ordem do enum. O resultado alimenta observações e recompensas; não é o motor que simula ataques e movimentação durante o combate.

## Sinergias

`SynergyCounter` conta traits das unidades e `SynergyEngine` acompanha contagens ativas/parciais, marcos e nível de trait. `CompositionPlanner` usa os marcos do set carregado para pontuar o ganho marginal de completar cada breakpoint: composições com sinergias fechadas (por exemplo, 2/4/6 quando esses forem os marcos reais) devem vencer uma distribuição que só espalha traits sem ativá-las. Os marcos nunca devem ser presumidos universais: a lista vem dos dados do set atual.

## Contexto estratégico

`StrategyInfo` é um snapshot de estratégia para observação e diagnóstico: estratégia atual/anterior, confiança, idade, motivo, scores, poder do tabuleiro, força do banco, contagem de traits ativas e informações de foco/composição. O processo hospedeiro pode consultar o snapshot de estratégia sem manter uma policy separada por jogador: o mesmo checkpoint é compartilhado pelos oito assentos.

## Integração

A estratégia não substitui as regras legais da engine. Ela informa estado/heurísticas; as ações continuam sujeitas às máscaras e à validação. Mudanças em traits, foco ou posicionamento precisam ser avaliadas com o catálogo e as recompensas atuais.

Testes relacionados: `cpp/tests/test_synergy_counter.cpp`, `tests/test_action_mask_validation.py` e testes de paridade de sinergia, quando aplicáveis.

## Upgrade retention and bench policy

- Buying the second matching 1-star copy receives a meaningful option-value reward even before the pair changes the board; composition fit still affects its value.
- Selling a copy that protects an upgrade receives a stronger penalty. The penalty is intentionally larger than the small reward for freeing a bench slot, so clearing space does not casually erase a pair.
- With a crowded but not full bench, a pair that supports the locked/core trait can still be purchased. A full bench requires a stronger immediate impact or a more advanced upgrade, preventing unbounded accumulation.
- Validate the behavior through pair-vs-singleton sale reward regressions and episode metrics: 2-star conversion rate, pairs sold before upgrading, bench occupancy, and gold spent on purchases that never improve the board.
