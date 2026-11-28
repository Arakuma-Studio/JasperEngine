# engine/core/dialogue (docs/specs/dialogue-graphs.md R15): the dialogue graph kind, its compiler and
# runner, the variable store and the Dialogue Runner component's object, the JASPERENGINE_DIALOGUE
# module. Its Lua binding (core/script/binding/DialogueClassesLua.cpp) is always compiled: without the
# module it binds the names as stubs that log once.

set(JASPERENGINE_DIALOGUE_SRCS
    core/dialogue/DialogueExpression.cpp
    core/dialogue/DialogueProgram.cpp
    core/dialogue/DialogueKind.cpp
    core/dialogue/DialogueVariables.cpp
    core/dialogue/DialogueRunner.cpp
    core/object/DialogueRunner.cpp
)
