# TFT-like RL Engine

Projeto de ambiente TFT-like para treinamento e avaliação de uma policy PPO compartilhada entre os oito jogadores de cada lobby. A engine de regras é C++ e é acessada por Python via bindings nativos.

## Documentação

A documentação técnica está em [docs/README.md](docs/README.md). Ela descreve arquitetura, subsistemas da engine, contrato de ações/máscaras, treinamento, avaliação e build/testes.

## Build rápida

No Windows PowerShell, com CMake, compilador C++ e dependências Python instaladas:

~~~powershell
python -m pip install -r requirements.txt
python scripts/build_native.py
~~~

Consulte [build e testes](docs/build-and-tests.md) para os pré-requisitos.

## Avaliação unificada da policy

Para gerar um relatório consolidado de auditoria estratégica, ações/recompensas,
máscaras, ciclos de estado e loops nativos em um único Markdown:

~~~powershell
python scripts/evaluate_all.py --checkpoint checkpoints/latest.pt --seeds 11,12,13
~~~

O relatório final é gravado em `reports/evaluation_report.md`; os dados brutos
JSONL/JSON ficam em `reports/evaluation_data/`. Para comparar dois checkpoints,
adicione `--baseline checkpoints/base.pt --candidate checkpoints/candidato.pt --matches 20`.
Para incluir a ablação dos controles de composição, acrescente `--ablation`.
Essas avaliações opcionais podem executar partidas adicionais e demorar mais.

## Catálogo de composições OP.GG

O snapshot local de meta é consumido pela engine C++ como sinal auxiliar para comparar composições conhecidas com o plano nativo, usando snapshots de early/mid game como informação de transição, sem tornar unidades obrigatórias. Veja [docs/opgg-meta-catalog.md](docs/opgg-meta-catalog.md).

## Componentes principais

- cpp/: engine C++, bindings e testes nativos.
- trainer.py, policy.py, ppo.py, rollout.py: treinamento PPO.
- cpp_lobby_env.py, vector_lobby_env.py: adaptadores Python.
- scripts/, tests/: ferramentas e regressões.
