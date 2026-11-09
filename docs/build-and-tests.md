# Build, testes e integração contínua

## Pré-requisitos

- Python e dependências listadas em `requirements.txt`.
- CMake e toolchain C++ compatíveis com a plataforma.
- PyTorch compatível com o checkpoint e com o dispositivo de inferência.
- pybind11 instalado no mesmo interpretador Python usado na configuração CMake.

## Build recomendada

Na raiz do repositório:

~~~powershell
python -m pip install -r requirements.txt
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build cpp/build --config Release --parallel
ctest --test-dir cpp/build -C Release --output-on-failure
~~~

O módulo pybind11 gerado pela build precisa estar no caminho de importação Python.

## Testes Python

Com os módulos nativos compilados e disponíveis no caminho de importação:

~~~powershell
python -m pytest tests
~~~

A suíte cobre engine, bindings, contrato de ações/máscaras e compatibilidade da policy. Não contém treinamento PPO nem fine-tuning.

## CI

`.github/workflows/cpp-parity.yml` compila os módulos C++, executa CTest e regressões Python do runtime.

## Critérios de conclusão

- Build nativa termina com sucesso.
- CTest passa.
- Testes Python relacionados à engine e inferência passam.
- Mudanças de contrato incluem fixtures/regressões.
- Testes não executados devem ser descritos como pendentes, nunca como aprovados.
