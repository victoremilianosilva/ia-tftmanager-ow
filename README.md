# TFT-like RL Engine

Projeto de engine TFT-like em C++ e execução de uma policy PPO compartilhada entre os oito jogadores de cada lobby. A engine de regras é acessada pelo runtime Python via bindings nativos.

## Documentação

A documentação técnica está em [docs/README.md](docs/README.md). Ela descreve arquitetura, subsistemas da engine, contrato de ações/máscaras, inferência e build/testes.

## Build rápida

No Windows PowerShell, com CMake, compilador C++ e dependências Python instaladas:

~~~powershell
python -m pip install -r requirements.txt
python scripts/build_native.py
~~~

Consulte [build e testes](docs/build-and-tests.md) para os pré-requisitos.

## Catálogo de composições OP.GG

O snapshot local de meta é consumido pela engine C++ como sinal auxiliar para comparar composições conhecidas com o plano nativo, usando snapshots de early/mid game como informação de transição, sem tornar unidades obrigatórias. Veja [docs/opgg-meta-catalog.md](docs/opgg-meta-catalog.md).

## Componentes principais

- cpp/: engine C++, bindings e testes nativos.
- policy.py e módulos de inferência: carregamento e execução da policy.
- cpp_lobby_env.py, vector_lobby_env.py: adaptadores Python do runtime.
- scripts/: build e utilitários operacionais.
- tests/: regressões da engine, bindings e contrato de inferência.
