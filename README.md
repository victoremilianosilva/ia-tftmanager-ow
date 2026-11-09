# TFT-like IA Runtime

Runtime de produção para executar a policy TFT treinada no repositório principal, usando a engine nativa C++ como fonte de verdade das regras e ações.

Este repositório **não treina nem faz fine-tuning do modelo**. Ele carrega checkpoints existentes e executa inferência durante a partida.

## Build

No Windows PowerShell, com Python, CMake e uma toolchain C++ compatível:

~~~powershell
python -m pip install -r requirements.txt
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build cpp/build --config Release --parallel
ctest --test-dir cpp/build -C Release --output-on-failure
~~~

Consulte [build e testes](docs/build-and-tests.md).

## Componentes

- `cpp/`: engine, regras, bindings e testes nativos.
- `policy.py`: arquitetura da policy, compatibilidade de checkpoints e inferência.
- `cpp_lobby_env.py`: adaptador Python para a engine nativa.
- `engine_contracts.py`: IDs e contratos compartilhados de ações/estratégias.
- `data/`: catálogos estáticos necessários à execução, incluindo unidades, itens e meta local.
- `tests/`: regressões da engine, bindings, policy e contrato de inferência.

O treinamento PPO, rollout, otimização, fine-tuning, avaliação experimental e auditorias ficam no repositório principal `ia-tft-manager`.
