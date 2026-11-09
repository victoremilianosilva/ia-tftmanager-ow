# Catálogo competitivo OP.GG para TFT

## Uso no runtime

A engine C++ consome somente o snapshot normalizado `data/opgg_tft_meta.json`. A consulta externa ao OP.GG não acontece durante a partida nem durante a inferência.

O arquivo é um dado estático versionado junto com o runtime. Atualizações/preparação do snapshot pertencem ao fluxo de dados do repositório principal, não ao processo de produção deste repositório.

## Como o planejador usa os dados

O `CompositionPlanner` mantém seu plano nativo e compara com as composições do catálogo:

- nível até 5: o snapshot `early_game` informa peças de transição;
- nível 6–7: o snapshot `mid_game` informa a transição intermediária;
- nível 8+: as unidades finais definem o alvo;
- unidades possuídas, unidades na loja, traits representadas, custo, dificuldade estimada de aquisição e papéis carry/frontline afetam o score;
- estatísticas de Top 4 só dão um bônus pequeno e quando há amostra mínima;
- a candidata externa só substitui o plano nativo quando obtém vantagem contextual.

O catálogo não restringe máscaras nem torna compras obrigatórias. Um catálogo ausente, inválido ou incompatível não pode interromper a partida; a engine deve continuar com o planejador nativo.

## Contrato de dados

O catálogo precisa conter IDs compatíveis com `data/units_gerado.json`. Unidades sem correspondência são ignoradas. A atualização do snapshot e a avaliação de qualidade da fonte são responsabilidades do pipeline de dados/treinamento no repositório principal.
