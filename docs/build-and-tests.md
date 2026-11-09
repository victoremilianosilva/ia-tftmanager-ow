# Build, testes e integração contínua

## Pré-requisitos

- Python e dependências de requirements.txt.
- CMake e toolchain C++ compatíveis com a plataforma.
- pybind11 instalado no mesmo interpretador Python usado na build.

## Build recomendada

Na raiz do repositório:

~~~powershell
python -m pip install -r requirements.txt
python scripts/build_native.py
~~~

O script consulta python -m pybind11 --cmakedir, configura CMake com BUILD_TESTING=ON, compila Release e executa CTest com --output-on-failure. Se a build falhar, corrija a primeira falha de configuração/compilação antes de interpretar resultados de teste.

## Testes Python

Com o módulo nativo compilado e disponível no caminho de importação:

~~~powershell
python -m pytest tests
~~~

Para triagem, selecione testes relevantes como tests/test_action_dimensions.py ou tests/test_lobby_end_to_end.py.

## CI

.github/workflows/cpp-parity.yml cobre a trilha de paridade C++ configurada no workflow. Não assuma que ele executa todos os testes Python; confira os passos efetivos ao alterar cobertura.

## Critérios de conclusão

- Build nativa termina com sucesso.
- CTest passa.
- Testes Python relacionados passam.
- Mudanças de contrato incluem fixtures/regressões, não apenas documentação.
- Testes não executados devem ser descritos como pendentes, nunca como aprovados.
