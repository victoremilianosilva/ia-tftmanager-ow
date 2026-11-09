# Build, testes e integração contínua

## Pré-requisitos

- Python e dependências listadas em `requirements.txt`.
- CMake e toolchain C++ compatíveis com a plataforma.
- pybind11 instalado no mesmo interpretador Python usado na build.
- PyTorch compatível com o checkpoint e com o dispositivo de inferência.

## Build recomendada

Na raiz do repositório:

~~~powershell
python -m pip install -r requirements.txt
python scripts/build_native.py
~~~

O script consulta `python -m pybind11 --cmakedir`, configura CMake com `BUILD_TESTING=ON`, compila Release e executa CTest com `--output-on-failure`.

## Testes Python

Com o módulo nativo compilado e disponível no caminho de importação:

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
