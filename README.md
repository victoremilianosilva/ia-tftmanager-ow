# TFT-like IA Runtime

Runtime de inferência da policy TFT treinada no repositório principal, usando a engine nativa C++ como fonte de verdade para regras, observações e máscaras. O runner de policy pode recomendar uma ação quando recebe observação e máscaras nativas válidas.

Este repositório **não treina nem faz fine-tuning do modelo**. Ele carrega checkpoints existentes e executa inferência, sem atualizar pesos.

**Limite importante da integração live:** a captura Overwolf e o contrato canônico de snapshot já existem, mas ainda não há um importador de produção que aplique esse DTO ao estado coerente de `TFTEngine`. O receiver Python atual valida e ordena snapshots; ele não produz observações/máscaras nativas nem disponibiliza inferência (`inferenceAvailable=false`). Portanto, o caminho GEP → Engine → PPO ainda não é uma integração live operacional.

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
- `live_inference/`: validação/sequenciamento do snapshot canônico e runner read-only; não implementa o importador nativo live.
- `data/`: catálogos estáticos necessários à execução, incluindo unidades, itens e meta local.
- `tests/`: regressões da engine, bindings, policy e contratos de inferência/snapshot.

O treinamento PPO, rollout, otimização, fine-tuning, avaliação experimental e auditorias ficam no repositório principal `ia-tft-manager`.

Consulte [a fronteira de integração live](docs/GEP_MAPPING.md) e a [auditoria de importação nativa](docs/NATIVE_ENGINE_LIVE_IMPORT_AUDIT.md) antes de tratar qualquer snapshot GEP como entrada de inferência.
