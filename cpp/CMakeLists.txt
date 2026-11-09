cmake_minimum_required(VERSION 3.20)

project(tft_combat_engine
    VERSION 0.1.0
    LANGUAGES CXX
)

# =========================================================
# Configuração global
# =========================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Composition-direction hysteresis is tunable without editing engine logic.
set(TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE "0.20" CACHE STRING
    "Minimum relative advantage required to change a locked composition")
set(TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE "0.75" CACHE STRING
    "Minimum absolute strength advantage required to change a locked composition")
set(TFT_COMP_DIRECTION_REQUIRED_UPDATES "3" CACHE STRING
    "Consecutive strategy refreshes required before a non-emergency direction change")
add_compile_definitions(
    TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE=${TFT_COMP_DIRECTION_MIN_RELATIVE_ADVANTAGE}
    TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE=${TFT_COMP_DIRECTION_MIN_ABSOLUTE_ADVANTAGE}
    TFT_COMP_DIRECTION_REQUIRED_UPDATES=${TFT_COMP_DIRECTION_REQUIRED_UPDATES}
)

# Default to the generated, versioned catalog for the active Set 18.
add_compile_definitions(
    TFT_DEFAULT_ITEM_CATALOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/itens.json"
    TFT_DEFAULT_TRAITS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/traits.json"
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/lib"
)

# Diretórios principais de includes.
set(TFT_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set(TFT_COMBAT_INCLUDE_DIR
    "${TFT_INCLUDE_DIR}/combat"
)

set(TFT_DECISION_INCLUDE_DIR
    "${TFT_INCLUDE_DIR}/decision"
)

set(TFT_LOBBY_INCLUDE_DIR
    "${TFT_INCLUDE_DIR}/lobby"
)

# =========================================================
# Python e pybind11
# =========================================================

find_package(Python REQUIRED COMPONENTS
    Interpreter
    Development
)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PYBIND11_RESULT
)

if(NOT PYBIND11_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Nao foi possivel localizar pybind11 no Python selecionado.\n"
        "Instale com: ${Python_EXECUTABLE} -m pip install pybind11"
    )
endif()

find_package(pybind11 CONFIG REQUIRED
    PATHS "${PYBIND11_CMAKE_DIR}"
    NO_DEFAULT_PATH
)

# =========================================================
# 📦 JSON para o catálogo de campeões
# =========================================================

include(FetchContent)

find_package(nlohmann_json 3.11 CONFIG QUIET)

if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )

    FetchContent_MakeAvailable(nlohmann_json)
endif()


message(STATUS "Python: ${Python_EXECUTABLE}")
message(STATUS "pybind11: ${PYBIND11_CMAKE_DIR}")

# =========================================================
# Função auxiliar para configurar módulos Python
# =========================================================

# Keep assert-based regression checks active even when CI builds Release.
function(enable_tft_test_assertions target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

function(configure_tft_module target)
    target_compile_features(
        ${target}
        PRIVATE
            cxx_std_20
    )

    # Os módulos usam includes no formato:
    #   decision/DecisionEngine.hpp
    #   lobby/TftEngine.hpp
    #   CombatEngine.hpp (no módulo de combate)
    target_include_directories(
        ${target}
        PRIVATE
            "${TFT_INCLUDE_DIR}"
    )

    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
        )
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endfunction()

# =========================================================
# Módulo: combate
#
# Estrutura:
#   src/combat/CombatEngine.cpp
#   bindings/combat_engine_bindings.cpp
#   include/combat/.hpp
# =========================================================

pybind11_add_module(
    combat_engine
    src/combat/CombatEngine.cpp
    bindings/combat_engine_bindings.cpp
)

configure_tft_module(combat_engine)

# CombatEngine.cpp e combat_engine_bindings.cpp usam includes
# sem o prefixo "combat/", como "CombatEngine.hpp".
target_include_directories(
    combat_engine
    PRIVATE
        "${TFT_COMBAT_INCLUDE_DIR}"
)

# =========================================================
# Módulo: decisões
#
# Estrutura:
#   src/decision/*.cpp
#   include/decision/*.hpp
# =========================================================

pybind11_add_module(
    decision_engine
    src/decision/DecisionEngine.cpp
    src/decision/AntiLoopEngine.cpp
    src/decision/MoveValidator.cpp
    src/decision/ActionMaskEngine.cpp
    src/decision/decision_bindings.cpp
)

configure_tft_module(decision_engine)

# Os arquivos usam includes com prefixo "decision/".
# O diretório include/ é suficiente para esses caminhos.
# include/decision também é adicionado para compatibilidade
# com arquivos que incluam headers diretamente pelo nome.
target_include_directories(
    decision_engine
    PRIVATE
        "${TFT_DECISION_INCLUDE_DIR}"
)

# =========================================================
# Módulo: lobby completa
#
# A API está declarada em:
#   include/lobby/TftEngine.hpp
#
# O módulo só será criado quando a implementação existir.
# Isso permite compilar e testar combate/decisões enquanto
# src/lobby/TftEngine.cpp ainda está pendente.
# =========================================================

set(TFT_LOBBY_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lobby/TftEngine.cpp"
)

set(TFT_LOBBY_BINDINGS
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/tft_engine_bindings.cpp"
)

if(EXISTS "${TFT_LOBBY_SOURCE}")
    pybind11_add_module(
        tft_engine
        src/lobby/TftEngine.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitInstanceFactory.cpp
        src/lobby/UnitPool.cpp
        src/lobby/ShopEngine.cpp
        src/lobby/EconomyEngine.cpp
        src/combat/CombatEngine.cpp
        src/decision/AntiLoopEngine.cpp
        src/decision/MoveValidator.cpp
        src/decision/ActionMaskEngine.cpp
        bindings/tft_engine_bindings.cpp
    )

    target_link_libraries(
        tft_engine
        PRIVATE nlohmann_json::nlohmann_json
    )

    configure_tft_module(tft_engine)

    target_compile_definitions(
        tft_engine
        PRIVATE
            TFT_DEFAULT_CATALOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    target_include_directories(
        tft_engine
        PRIVATE
            "${TFT_LOBBY_INCLUDE_DIR}"
            "${TFT_COMBAT_INCLUDE_DIR}"
            "${TFT_DECISION_INCLUDE_DIR}"
    )

    message(STATUS "Modulo tft_engine habilitado.")
else()
    message(WARNING
        "Modulo tft_engine ainda desabilitado: "
        "src/lobby/TftEngine.cpp nao existe. "
        "Implemente a API declarada em include/lobby/TftEngine.hpp."
    )
endif()

# =========================================================
# Testes nativos
# =========================================================

include(CTest)

if(BUILD_TESTING)
    add_executable(test_live_state_dto
        tests/test_live_state_dto.cpp
        src/lobby/LiveState.cpp
    )
    configure_tft_module(test_live_state_dto)
    enable_tft_test_assertions(test_live_state_dto)
    add_test(NAME live_state_dto COMMAND test_live_state_dto)

    add_executable(test_combat_engine
        tests/test_combat_engine.cpp
        src/combat/CombatEngine.cpp
    )
    configure_tft_module(test_combat_engine)
    target_include_directories(test_combat_engine
        PRIVATE "${TFT_COMBAT_INCLUDE_DIR}"
    )
    add_test(NAME test_combat_engine COMMAND test_combat_engine)

    add_executable(
        test_composition_direction_policy
        tests/test_composition_direction_policy.cpp
    )
    configure_tft_module(test_composition_direction_policy)
    enable_tft_test_assertions(test_composition_direction_policy)
    add_test(
        NAME composition_direction_policy
        COMMAND test_composition_direction_policy
    )

    add_executable(test_composition_planner tests/test_composition_planner.cpp)
    configure_tft_module(test_composition_planner)
    target_link_libraries(test_composition_planner PRIVATE nlohmann_json::nlohmann_json)
    enable_tft_test_assertions(test_composition_planner)
    add_test(NAME composition_planner COMMAND test_composition_planner)

    add_executable(test_composition_planner_opgg tests/test_composition_planner_opgg.cpp)
    configure_tft_module(test_composition_planner_opgg)
    target_link_libraries(test_composition_planner_opgg PRIVATE nlohmann_json::nlohmann_json)
    enable_tft_test_assertions(test_composition_planner_opgg)
    add_test(NAME composition_planner_opgg_selection
        COMMAND test_composition_planner_opgg selection)
    add_test(NAME composition_planner_opgg_incompatible_fallback
        COMMAND test_composition_planner_opgg incompatible)

    add_executable(test_unit_combat_classifier tests/test_unit_combat_classifier.cpp)
    configure_tft_module(test_unit_combat_classifier)
    enable_tft_test_assertions(test_unit_combat_classifier)
    add_test(NAME test_unit_combat_classifier COMMAND test_unit_combat_classifier)

    add_executable(test_board_evaluation tests/test_board_evaluation.cpp)
    configure_tft_module(test_board_evaluation)
    enable_tft_test_assertions(test_board_evaluation)
    add_test(NAME test_board_evaluation COMMAND test_board_evaluation)

    add_executable(test_strategy_policy tests/test_strategy_policy.cpp)
    configure_tft_module(test_strategy_policy)
    enable_tft_test_assertions(test_strategy_policy)
    add_test(NAME test_strategy_policy COMMAND test_strategy_policy)

    add_executable(test_reward_model tests/test_reward_model.cpp)
    target_include_directories(test_reward_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_reward_model COMMAND test_reward_model)

    add_executable(test_item_engine tests/test_item_engine.cpp)
    configure_tft_module(test_item_engine)
    enable_tft_test_assertions(test_item_engine)
    target_link_libraries(test_item_engine PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME item_engine COMMAND test_item_engine)

    add_executable(test_synergy_counter tests/test_synergy_counter.cpp)
    target_include_directories(test_synergy_counter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_synergy_counter COMMAND test_synergy_counter)


    # -----------------------------------------------------
    # Contrato de erros
    # -----------------------------------------------------

    add_executable(
        test_decision_error
        tests/test_decision_error.cpp
    )

    configure_tft_module(test_decision_error)

    add_test(
        NAME decision_error_contract
        COMMAND test_decision_error
    )

    # -----------------------------------------------------
    # Validador de movimentos
    # -----------------------------------------------------

    add_executable(
        test_move_validator
        tests/test_move_validator.cpp
        src/decision/MoveValidator.cpp
    )

    configure_tft_module(test_move_validator)

    target_include_directories(
        test_move_validator
        PRIVATE
            "${TFT_DECISION_INCLUDE_DIR}"
    )

    add_test(
        NAME move_validator
        COMMAND test_move_validator
    )

    # -----------------------------------------------------
    # Anti-loop
    # -----------------------------------------------------

    add_executable(
        test_anti_loop_engine
        tests/test_anti_loop_engine.cpp
        src/decision/AntiLoopEngine.cpp
    )

    configure_tft_module(test_anti_loop_engine)
    enable_tft_test_assertions(test_anti_loop_engine)

    target_include_directories(
        test_anti_loop_engine
        PRIVATE
            "${TFT_DECISION_INCLUDE_DIR}"
    )

    add_test(
        NAME anti_loop_engine
        COMMAND test_anti_loop_engine
    )

    # -----------------------------------------------------
    # Máscaras de ações
    # -----------------------------------------------------

    add_executable(
        test_action_mask_engine
        tests/test_action_mask_engine.cpp
        src/decision/ActionMaskEngine.cpp
        src/decision/MoveValidator.cpp
        src/decision/AntiLoopEngine.cpp
    )

    configure_tft_module(test_action_mask_engine)
    enable_tft_test_assertions(test_action_mask_engine)

    target_include_directories(
        test_action_mask_engine
        PRIVATE
            "${TFT_DECISION_INCLUDE_DIR}"
    )

    add_test(
        NAME action_mask_engine
        COMMAND test_action_mask_engine
    )

    # -----------------------------------------------------
    # Importação dos módulos Python
    # -----------------------------------------------------

    add_test(
        NAME combat_engine_import
        COMMAND
            "${Python_EXECUTABLE}"
            -c
            "import combat_engine; print(combat_engine.__doc__)"
    )

    set_tests_properties(
        combat_engine_import
        PROPERTIES
            ENVIRONMENT
                "PYTHONPATH=$<TARGET_FILE_DIR:combat_engine>"
    )

    add_test(
        NAME decision_engine_import
        COMMAND
            "${Python_EXECUTABLE}"
            -c
            "import decision_engine; print(decision_engine.__doc__)"
    )

    set_tests_properties(
        decision_engine_import
        PROPERTIES
            ENVIRONMENT
                "PYTHONPATH=$<TARGET_FILE_DIR:decision_engine>"
    )

    add_executable(
        test_unit_instance_factory
        tests/test_unit_instance_factory.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitInstanceFactory.cpp
    )

    configure_tft_module(test_unit_instance_factory)

    target_link_libraries(
        test_unit_instance_factory
        PRIVATE nlohmann_json::nlohmann_json
    )

    add_test(
        NAME unit_instance_factory
        COMMAND test_unit_instance_factory
            "${CMAKE_SOURCE_DIR}/../data/units_gerado.json"
    )

    add_executable(
        test_unit_pool
        tests/test_unit_pool.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitPool.cpp
    )

    target_include_directories(
        test_unit_pool
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
        test_unit_pool
        PRIVATE
        nlohmann_json::nlohmann_json
    )

    add_test(
        NAME unit_pool
        COMMAND test_unit_pool
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    add_executable(
        test_tft_engine_pool
        tests/test_tft_engine_pool.cpp
        src/lobby/TftEngine.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitInstanceFactory.cpp
        src/lobby/UnitPool.cpp
        src/lobby/ShopEngine.cpp
        src/lobby/EconomyEngine.cpp
        src/combat/CombatEngine.cpp
        src/decision/AntiLoopEngine.cpp
        src/decision/MoveValidator.cpp
        src/decision/ActionMaskEngine.cpp
    )

    configure_tft_module(test_tft_engine_pool)

    # CombatEngine.cpp includes "CombatEngine.hpp" without the combat/ prefix.
    target_include_directories(
        test_tft_engine_pool
        PRIVATE
            "${TFT_COMBAT_INCLUDE_DIR}"
    )

    target_link_libraries(
        test_tft_engine_pool
        PRIVATE nlohmann_json::nlohmann_json
    )

    target_compile_definitions(
        test_tft_engine_pool
        PRIVATE
            TFT_DEFAULT_CATALOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    add_test(
        NAME tft_engine_pool
        COMMAND test_tft_engine_pool
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    add_test(
        NAME tft_engine_item_actions
        COMMAND test_tft_engine_pool
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/items_test_catalog.json"
    )

    # Exercise the actual set catalog and PvE anvil schedule end-to-end.
    add_test(
        NAME tft_engine_item_anvil_rewards
        COMMAND test_tft_engine_pool
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/items_test_catalog.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/itens.json"
    )

    # Validate the active item catalog consumed by the engine.
    add_executable(
        test_item_catalog
        tests/test_item_catalog.cpp
    )
    target_include_directories(
        test_item_catalog
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(
        test_item_catalog
        PRIVATE nlohmann_json::nlohmann_json
    )
    add_test(
        NAME item_catalog_active
        COMMAND test_item_catalog
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/itens.json"
    )

    # Validate trait milestones and identifier mapping from the active production catalog.
    add_executable(
        test_trait_milestones
        tests/test_trait_milestones.cpp
    )
    configure_tft_module(test_trait_milestones)
    enable_tft_test_assertions(test_trait_milestones)
    target_link_libraries(
        test_trait_milestones
        PRIVATE nlohmann_json::nlohmann_json
    )
    add_test(
        NAME trait_milestones_active
        COMMAND test_trait_milestones
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/traits.json"
    )

    # -----------------------------------------------------
    # Integração de combate + teste de estresse anti-loop
    # -----------------------------------------------------

    add_executable(
        test_tft_combat_anti_loop_soak
        tests/test_tft_combat_anti_loop_soak.cpp
        src/lobby/TftEngine.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitInstanceFactory.cpp
        src/lobby/UnitPool.cpp
        src/lobby/ShopEngine.cpp
        src/lobby/EconomyEngine.cpp
        src/combat/CombatEngine.cpp
        src/decision/AntiLoopEngine.cpp
        src/decision/MoveValidator.cpp
        src/decision/ActionMaskEngine.cpp
    )

    configure_tft_module(test_tft_combat_anti_loop_soak)

    # CombatEngine.cpp usa #include "CombatEngine.hpp" sem prefixo.
    # O teste de integração precisa também do diretório específico de combate.
    target_include_directories(
        test_tft_combat_anti_loop_soak
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/combat"
    )

    target_link_libraries(
        test_tft_combat_anti_loop_soak
        PRIVATE nlohmann_json::nlohmann_json
    )

    target_compile_definitions(
        test_tft_combat_anti_loop_soak
        PRIVATE
            TFT_DEFAULT_CATALOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    add_test(
        NAME tft_combat_anti_loop_soak
        COMMAND test_tft_combat_anti_loop_soak
            "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    # -----------------------------------------------------
    # Loja C++ e pool compartilhado
    # -----------------------------------------------------
    add_executable(
        test_shop_engine
        tests/test_shop_engine.cpp
        src/lobby/ShopEngine.cpp
        src/lobby/ChampionCatalog.cpp
        src/lobby/UnitInstanceFactory.cpp
        src/lobby/UnitPool.cpp
    )
    configure_tft_module(test_shop_engine)
    target_link_libraries(test_shop_engine PRIVATE nlohmann_json::nlohmann_json)
    add_test(
        NAME shop_engine
        COMMAND test_shop_engine "${CMAKE_CURRENT_SOURCE_DIR}/../data/units_gerado.json"
    )

    # -----------------------------------------------------
    # Economia
    # -----------------------------------------------------

    add_executable(
        test_economy_engine
        tests/test_economy_engine.cpp
        src/lobby/EconomyEngine.cpp
    )

    configure_tft_module(test_economy_engine)

    target_include_directories(
        test_economy_engine
        PRIVATE
            "${TFT_LOBBY_INCLUDE_DIR}"
    )

    add_test(
        NAME economy_engine
        COMMAND test_economy_engine
    )

    # O teste da lobby só existe quando seu módulo é criado.
    if(TARGET tft_engine)
        add_test(
            NAME tft_engine_import
            COMMAND
                "${Python_EXECUTABLE}"
                -c
                "import tft_engine; print(tft_engine.__doc__)"
        )

        set_tests_properties(
            tft_engine_import
            PROPERTIES
                ENVIRONMENT
                    "PYTHONPATH=$<TARGET_FILE_DIR:tft_engine>"
        )
    endif()

endif()