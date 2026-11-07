# Paths in globs: a folder named with [ ] * or ? (an export into "My Game [v1]", a user cache below
# such a user name) would otherwise be read as a glob pattern, and the glob would find nothing.
# jasperengine_glob_escape(<out> <path>) puts each of those characters in a class of its own
# ("[" -> "[[]", "]" -> "[]]"; CMake's glob reads a "]" first in a class as the character).
include_guard(GLOBAL)

function(jasperengine_glob_escape out path)
    string(REGEX REPLACE "([][*?])" "[\\1]" escaped "${path}")
    set(${out} "${escaped}" PARENT_SCOPE)
endfunction()
