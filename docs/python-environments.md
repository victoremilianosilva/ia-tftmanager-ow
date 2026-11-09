# Runtime Python e bindings nativos

## CppTFTLobbyWrapper

`cpp_lobby_env.py` disponibiliza a engine nativa `tft_engine` para o runtime Python. O adaptador expõe reset/step, normaliza ações para seis inteiros, converte observações e recompensas para arrays NumPy e encaminha consultas de máscaras, estratégias, métricas anti-loop e estado público.

`step(actions)` espera oito ações, uma por jogador. O retorno contém observações, recompensas, flag de conclusão da lobby e informações públicas da partida.

## Inferência da policy

`policy.py` define a arquitetura usada pelos checkpoints PPO treinados no repositório principal. O runtime carrega os pesos existentes, valida/migra somente formatos de checkpoint explicitamente suportados e chama a policy com observações e máscaras produzidas pela engine.

O runtime não cria otimizador, não coleta rollouts e não atualiza pesos.

## Bindings

`cpp/bindings/tft_engine_bindings.cpp` e `cpp/bindings/combat_engine_bindings.cpp` expõem APIs nativas para Python. O módulo compilado precisa estar no caminho de importação. O contrato inclui IDs de ações, shapes, tipos numéricos e formatos de snapshots.

## Fechamento de recursos

Ambientes nativos têm `close()`. O processo hospedeiro deve fechar a sessão no encerramento e não deve fechar uma engine enquanto `step` estiver ativo em outra thread.

Consulte [build e testes](build-and-tests.md).
