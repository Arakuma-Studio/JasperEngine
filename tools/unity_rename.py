#!/usr/bin/env python3
"""Rename JasperEngine's public API and editor identifiers to Unity terminology.

    tools/unity_rename.py --dry-run         per-rule match counts, planned path renames,
                                            identifier merges and collisions
    tools/unity_rename.py --apply-paths     git mv the renamed source files (deepest first)
    tools/unity_rename.py --apply-content   rewrite text files in place
    tools/unity_rename.py --check           fail if old identifiers remain outside the allowlist
    tools/unity_rename.py --project <dir>   migrate a game project (scenes, prefabs, Lua and
                                            C++ scripts, project.yaml); add --report-only to
                                            only list what would change

Clean break: no aliases, no compatibility shims and no load-time migration. Old
projects are migrated once with --project. docs/unity-glossary.md lists the renames.

The rules are token and regex rules, not line patches, so they also convert code
written against the old names after this script was written (run --apply-paths
and --apply-content again after merging such code). Every step is idempotent.
Operates on files tracked by git; run from anywhere inside the repository.

C, C++, Objective-C, Java, GLSL and Lua files are split into code, comment and
string segments, so a rule can rename a class in code without touching the same
English word in a UI label. Other text files form a single segment.
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path, PurePosixPath

# --------------------------------------------------------------------------------------
# Scope
# --------------------------------------------------------------------------------------

THIRD_PARTY = ("libs/", "engine/libs/", "editor/external/", "shadercompiler/libs/")
# History docs are not rewritten; this script and the glossary name the old identifiers.
EXCLUDED_PREFIXES = THIRD_PARTY + ("docs/superpowers/", "build/")
EXCLUDED_FILES = {"LICENSE", "NOTICE.md", "tools/unity_rename.py", "tools/rebrand.py",
                  "docs/unity-glossary.md"}

C_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm", ".java",
          ".glsl", ".vert", ".frag", ".js"}
LUA_EXTS = {".lua"}
# Project files that are YAML documents.
YAML_EXTS = {".scene", ".bundle", ".prefab", ".yaml", ".material"}

# Source files renamed with the class they declare. Stems are looked up next to
# each other in the listed directory with any extension (.h, .cpp).
FILE_RENAMES = {
    "engine/core/object/Object": "GameObject",
    "engine/core/object/Shape": "Primitive",
    "engine/core/object/Lines": "LineRenderer",
    "engine/core/object/Occluder2D": "ShadowCaster2D",
    "engine/core/object/environment/SkyBox": "Skybox",
    "engine/core/object/physics/Body2D": "Rigidbody2D",
    "engine/core/object/physics/Body3D": "Rigidbody",
    "engine/core/object/sound/Sound": "AudioSource",
    "engine/core/object/ui/Container": "LayoutGroup",
    "engine/core/object/ui/TextEdit": "InputField",
    "engine/core/object/ui/UILayout": "RectTransform",
    "engine/core/action/TimedAction": "Tween",
    "engine/core/action/AlphaAction": "AlphaTween",
    "engine/core/action/ColorAction": "ColorTween",
    "engine/core/action/PositionAction": "PositionTween",
    "engine/core/action/RotationAction": "RotationTween",
    "engine/core/action/ScaleAction": "ScaleTween",
    "engine/core/action/Particles": "ParticleSystem",
    "engine/core/component/AlphaActionComponent": "AlphaTweenComponent",
    "engine/core/component/Body2DComponent": "Rigidbody2DComponent",
    "engine/core/component/Body3DComponent": "RigidbodyComponent",
    "engine/core/component/BundleComponent": "PrefabComponent",
    "engine/core/component/ColorActionComponent": "ColorTweenComponent",
    "engine/core/component/LinesComponent": "LineRendererComponent",
    "engine/core/component/Occluder2DComponent": "ShadowCaster2DComponent",
    "engine/core/component/ParticlesComponent": "ParticleSystemComponent",
    "engine/core/component/PositionActionComponent": "PositionTweenComponent",
    "engine/core/component/RotationActionComponent": "RotationTweenComponent",
    "engine/core/component/ScaleActionComponent": "ScaleTweenComponent",
    "engine/core/component/SkyComponent": "SkyboxComponent",
    "engine/core/component/SoundComponent": "AudioSourceComponent",
    "engine/core/component/TextEditComponent": "InputFieldComponent",
    "engine/core/component/TimedActionComponent": "TweenComponent",
    "engine/core/component/UIContainerComponent": "LayoutGroupComponent",
    "engine/core/component/UILayoutComponent": "RectTransformComponent",
    "engine/core/manager/BundleManager": "PrefabManager",
    "engine/core/script/binding/ObjectClassesLua": "GameObjectClassesLua",
    "editor/window/Structure": "HierarchyWindow",
    "editor/window/Properties": "InspectorWindow",
    "editor/window/ResourcesWindow": "ProjectWindow",
    "editor/window/OutputWindow": "ConsoleWindow",
    "editor/window/dialog/BundlesWindow": "PrefabsWindow",
    "editor/util/EntityBundle": "Prefab",
    "editor/util/ShapeParameters": "PrimitiveParameters",
    "editor/command/type/AddEntityToBundleCmd": "AddEntityToPrefabCmd",
    "editor/command/type/ComponentToBundleLocalCmd": "ComponentToPrefabLocalCmd",
    "editor/command/type/ComponentToBundleSharedCmd": "ComponentToPrefabSharedCmd",
    "editor/command/type/CreateEntityBundleCmd": "CreatePrefabCmd",
    "editor/command/type/ImportEntityBundleCmd": "ImportPrefabCmd",
    "editor/command/type/RemoveEntityFromBundleCmd": "RemoveEntityFromPrefabCmd",
}
SOURCE_EXTS = (".h", ".cpp", ".mm", ".hpp")

# --------------------------------------------------------------------------------------
# Name tables shared by the context rules
# --------------------------------------------------------------------------------------

# Engine classes whose names are generic words; the rules rename them in code only.
CLASS_MAP = {
    "Object": "GameObject",
    "Shape": "Primitive",
    "Sound": "AudioSource",
    "Lines": "LineRenderer",
    "Particles": "ParticleSystem",
    "Container": "LayoutGroup",
    "SkyBox": "Skybox",
    "Occluder2D": "ShadowCaster2D",
    "Body2D": "Rigidbody2D",
    "Body3D": "Rigidbody",
    "UILayout": "RectTransform",
    "TextEdit": "InputField",
    "TimedAction": "Tween",
    "PositionAction": "PositionTween",
    "RotationAction": "RotationTween",
    "ScaleAction": "ScaleTween",
    "ColorAction": "ColorTween",
    "AlphaAction": "AlphaTween",
    "BundleManager": "PrefabManager",
    "EntityBundle": "Prefab",
}

# Component keys in scene and prefab YAML, which Catalog::getComponentName writes
# as the lowercased class name without "Component", plus "audio", an old alias
# Catalog::getComponentType accepted for the sound component.
COMPONENT_KEY_MAP = {
    "sound": "audiosource",
    "audio": "audiosource",
    "body2d": "rigidbody2d",
    "body3d": "rigidbody",
    "bundle": "prefab",
    "lines": "linerenderer",
    "occluder2d": "shadowcaster2d",
    "particles": "particlesystem",
    "sky": "skybox",
    "textedit": "inputfield",
    "timedaction": "tween",
    "alphaaction": "alphatween",
    "coloraction": "colortween",
    "positionaction": "positiontween",
    "rotationaction": "rotationtween",
    "scaleaction": "scaletween",
    "uicontainer": "layoutgroup",
    "uilayout": "recttransform",
}

# Entity type names the AI tools accept (create_entity "type").
AI_ENTITY_MAP = {
    "object": "gameobject",
    "textedit": "input_field",
    "container": "layout_group",
    "occluder_2d": "shadow_caster_2d",
    "body2d": "rigidbody2d",
    "body3d": "rigidbody",
    "sky": "skybox",
    "sound": "audio_source",
    "sound_3d": "audio_source_3d",
    "position_action": "position_tween",
    "rotation_action": "rotation_tween",
    "scale_action": "scale_tween",
    "color_action": "color_tween",
    "alpha_action": "alpha_tween",
    "particles": "particle_system",
    "lines": "line_renderer",
}

# enum class values whose names are generic words; renamed where qualified by the
# enum name and inside the enum's own definition.
ENUM_MAPS = {
    "EntityCreationType": {
        "OBJECT": "GAMEOBJECT", "TEXTEDIT": "INPUT_FIELD", "CONTAINER": "LAYOUT_GROUP",
        "OCCLUDER_2D": "SHADOW_CASTER_2D", "BODY2D": "RIGIDBODY2D", "BODY3D": "RIGIDBODY",
        "SKY": "SKYBOX", "SOUND": "AUDIO_SOURCE", "SOUND_3D": "AUDIO_SOURCE_3D",
        "POSITION_ACTION": "POSITION_TWEEN", "ROTATION_ACTION": "ROTATION_TWEEN",
        "SCALE_ACTION": "SCALE_TWEEN", "COLOR_ACTION": "COLOR_TWEEN", "ALPHA_ACTION": "ALPHA_TWEEN",
        "PARTICLES": "PARTICLE_SYSTEM", "LINES": "LINE_RENDERER",
    },
    "FileType": {"BUNDLE": "PREFAB"},
}

# Editor panels: class, settings key (editor settings.yaml "panels"), window title.
PANEL_KEYS = {"structure": "hierarchy", "properties": "inspector", "resources": "project", "output": "console"}
WINDOW_TITLES = {"Structure": "Hierarchy", "Properties": "Inspector", "Resources": "Project", "Output": "Console"}

# Tokens containing "bundle" that mean an Apple/Android/CMake bundle, not a prefab.
BUNDLE_KEEP = re.compile(
    r"CFBundle|NSBundle|mainBundle|[Bb]undleIdentifier|BUNDLE_IDENTIFIER|MACOSX_BUNDLE|"
    r"PRODUCT_BUNDLE|TARGET_BUNDLE|APP_BUNDLE|JASPERENGINE_BUNDLE_|bundleResourcePath|"
    r"AppleBundle|^[Bb]undled$|^BUNDLE$")

# --------------------------------------------------------------------------------------
# Rule engine
# --------------------------------------------------------------------------------------

ID = r"A-Za-z0-9_"
B = rf"(?<![{ID}])"         # identifier start
E = rf"(?![{ID}])"          # identifier end
P = r"(?<![A-Za-z0-9])"     # identifier start or start of a snake_case part

TREE_BUNDLE_SCOPE = (r"^editor/", r"^engine/core/", r"^engine/project/")
EDITOR = (r"^editor/",)


class Rule:
    """One regex rule.

    kinds: segments it applies to. C code, M comment, S string literal, T other
    text files, W the whole text of a C-like or Lua file before it is split.
    only/skip: path regexes (repository paths, or "project/<rel>" in --project).
    project: whether it applies when migrating a game project.
    """

    def __init__(self, name, pattern, repl, kinds="CMST", only=None, skip=None, project=True, flags=0):
        self.name = name
        self.re = re.compile(pattern, flags)
        self.repl = repl
        self.kinds = set(kinds)
        self.only = [re.compile(p) for p in only] if only else None
        self.skip = [re.compile(p) for p in skip] if skip else []
        self.project = project

    def applies(self, path, kind, project):
        if kind not in self.kinds:
            return False
        if project:
            return self.project
        if self.only is not None and not any(p.search(path) for p in self.only):
            return False
        return not any(p.search(path) for p in self.skip)


def mapper(table):
    """Replacement function looking a whole match up in a table."""
    return lambda m: table.get(m.group(0), m.group(0))


def variants(name, old, new, kinds="CMST", **kw):
    """PascalCase, camelCase (token start), lowercase and UPPERCASE forms of a
    distinctive name, as substrings of identifiers."""
    low_old, low_new = old.lower(), new.lower()
    camel_old, camel_new = old[0].lower() + old[1:], new[0].lower() + new[1:]
    rules = [Rule(f"{name} (Pascal)", old, new, kinds, **kw)]
    if camel_old != low_old:
        rules.append(Rule(f"{name} (camel)", P + camel_old, camel_new, kinds, **kw))
    rules.append(Rule(f"{name} (lower)", P + low_old, low_new, kinds, **kw))
    rules.append(Rule(f"{name} (UPPER)", P + old.upper(), new.upper(), kinds, **kw))
    return rules


# ---- Phase 1: context rules on whole C-like / Lua files ------------------------------

def _dispatch(m):
    cls = m.group(1)
    return f'DISPATCH_TYPE({cls}, "{CLASS_MAP.get(cls, cls).lower()}")'


def _register_literal(m):
    cls, lit = m.group(2), m.group(4)
    if lit == cls:
        lit = CLASS_MAP.get(lit, lit)
    return f"{m.group(1)}{lit}{m.group(5)}"


def _enum_block(table):
    def fix(m):
        return re.sub(B + r"(" + "|".join(table) + r")" + E, mapper(table), m.group(0))
    return fix


def _allowed_types(m):
    return re.sub(B + r"[a-z0-9_]+" + E, mapper(AI_ENTITY_MAP), m.group(0))


def _panel_block(m):
    return re.sub(B + r"(structure|properties|resources|output)" + E, mapper(PANEL_KEYS), m.group(0))


PHASE1 = [
    # Lua type dispatch keyed by the lowercased class name.
    Rule("DISPATCH_TYPE key", r'DISPATCH_TYPE\((\w+),\s*"(\w+)"\)', _dispatch, "W"),
    # LuaBridge registration: the Lua name follows the C++ class name.
    Rule("Lua class registration name",
         r'((?:beginClass|deriveClass)\s*<\s*(\w+)\s*(,[^>]*)?>\s*\(\s*")(\w+)(")', _register_literal, "W"),
    # Class names handed to the code editor's API suggestions.
    Rule("class-name literal", r'\b((?:objectClass|handleClass|meshClass)\(")(\w+)(?="\))',
         lambda m: m.group(1) + CLASS_MAP.get(m.group(2), m.group(2)), "W"),
    Rule("component key literal", r'(?<=normalizedName == ")(\w+)(?=")', mapper(COMPONENT_KEY_MAP), "W"),
    Rule("AI entity type", r'(?<=\{")(\w+)(?=",\s*EntityCreationType::)', mapper(AI_ENTITY_MAP), "W"),
    Rule("AI entity type list", r'Allowed type: [a-z0-9_, ]+', _allowed_types, "W"),
    # Exact literals naming a class: default entity names, script property types.
    Rule("class-name string", r'(?<=")(Object|Sound|TextEdit|Container|Particles|UILayout|SkyBox|'
         r'TimedAction|PositionAction|RotationAction|ScaleAction|ColorAction|AlphaAction|Occluder2D)'
         r'(?="(?!\s*,\s*ResourceType::))', mapper(CLASS_MAP), "W",
         skip=(r"^engine/core/thread/ResourceProgress\.",)),
    Rule("getObject/setObject", B + r"([gs]et)Object(?![A-Za-z0-9_:])", r"\1GameObject", "W"),
    Rule("Object classes", r"(?<!Game)ObjectClasses", "GameObjectClasses", "W"),
    # Physics shape YAML keys and property paths ("shapes[0].density").
    Rule('"shapes" key', r'(?<=")shapes(?=["\[])', "colliders", "W"),
    # Editor panels.
    Rule("panel settings key", r'(?<=panelsNode\[")(structure|properties|resources|output)(?="\])',
         mapper(PANEL_KEYS), "W", only=EDITOR, project=False),
    Rule("panel visibility field", r'(?<=[Vv]isibility\.)(structure|properties|resources|output)' + E,
         mapper(PANEL_KEYS), "W", only=EDITOR, project=False),
    Rule("panel visibility struct", r"struct PanelVisibility\w*\s*\{[^}]*\}", _panel_block, "W",
         only=EDITOR, project=False),
    Rule("project window settings key", r'"resources_window"', '"project_window"', "W", only=EDITOR, project=False),
    Rule("window title", r'(?<=WINDOW_NAME = ")(Structure|Properties|Resources|Output)(?=")',
         mapper(WINDOW_TITLES), "W", only=EDITOR, project=False),
]
for _enum, _table in ENUM_MAPS.items():
    PHASE1.append(Rule(f"{_enum} value", rf"(?<={_enum}::)(" + "|".join(_table) + ")" + E, mapper(_table), "W"))
    PHASE1.append(Rule(f"{_enum} definition", rf"enum\s+class\s+{_enum}\b[^{{;]*\{{[^}}]*\}}",
                       _enum_block(_table), "W"))

# ---- Phase 2: per-segment rules ------------------------------------------------------

PHASE2 = []  # the file-name rule is inserted first at run time (see FileNames)


def add(*rules):
    for r in rules:
        if isinstance(r, list):
            PHASE2.extend(r)
        else:
            PHASE2.append(r)


# Phrases in comments and UI strings, before the identifier rules see them.
add(
    Rule("phrase: entity bundle", r"\b([Ee])ntity [Bb]undle", lambda m: "Prefab" if m.group(1) == "E" else "prefab",
         "MST", only=TREE_BUNDLE_SCOPE),
    Rule("phrase: collision shape", r"\bcollision shape", "collider", "MS"),
    Rule("phrase: Add/Remove Shape", r"\b(Add|Remove) Shape\b", r"\1 Collider", "S"),
    Rule("phrase: Shape n", r'(?<=")Shape (?=")', "Collider ", "S"),
    Rule("phrase: Create Shape", r"\bCreate Shape\b", "Create Primitive", "S"),
    Rule("phrase: Text Edit", r"\bText Edit\b", "Input Field", "MS"),
    Rule("phrase: UI Container", r"\bUI Container\b", "Layout Group", "MS"),
    Rule("phrase: 2D Occluder", r"\b2D Occluder\b", "Shadow Caster 2D", "MS"),
    Rule("phrase: Body 2D/3D", r"\bBody ([23])D\b", lambda m: "Rigidbody 2D" if m.group(1) == "2" else "Rigidbody",
         "MS"),
    Rule("phrase: Sound Source", r"\bSound Source\b", "Audio Source", "MS"),
    Rule("phrase: 3D Sound", r"\b3D Sound\b", "3D Audio Source", "MS"),
    Rule("phrase: Timed Action", r"\bTimed Action\b", "Tween", "MS"),
    Rule("phrase: <kind> Action", r"\b(Position|Rotation|Scale|Color|Alpha) Action\b", r"\1 Tween", "MS"),
    Rule("phrase: editor windows", r"\b(Structure|Properties|Resources|Output) (window|panel|tree|node)\b",
         lambda m: f"{WINDOW_TITLES[m.group(1)]} {m.group(2)}", "MS", only=EDITOR, project=False),
    Rule("phrase: in Structure", r"\bin Structure\b", "in the Hierarchy", "MS", only=EDITOR, project=False),
    Rule("phrase: Properties inspector", r"\bProperties inspector\b", "Inspector", "MS", only=EDITOR, project=False),
    Rule("phrase: editor window classes in comments", B + r"(Structure|Properties)(?=::)",
         mapper({"Structure": "HierarchyWindow", "Properties": "InspectorWindow"}), "M", only=EDITOR, project=False),
    Rule("phrase: Shape( in strings", B + r"Shape(?=\()|(?<=Mesh/)Shape" + E + r"|(?<=\(or )Shape(?=\))",
         "Primitive", "S"),
    Rule("phrase: Object in strings", B + r"Object(?=\(| binding| wrapper)", "GameObject", "S"),
)

# Articles before renamed class names, in comments and strings (run after the renames).
ARTICLE_RULES = [
    Rule("article: a -> an", r"\ba (?=(?:AudioSource|InputField|AlphaTween)\w*)", "an ", "MS"),
    Rule("article: an -> a", r"\ban (?=(?:GameObject|ShadowCaster2D|Prefab|LayoutGroup|LineRenderer|ParticleSystem|"
         r"RectTransform|Skybox|Rigidbody|Primitive|Tween|PositionTween|RotationTween|ScaleTween|ColorTween|Collider)\w*)",
         "a ", "MS"),
]

# Distinctive names, as parts of any identifier, in code, comments and strings.
add(
    # Physics shapes become colliders (before Body*D so the combined forms convert).
    Rule("Shape2D/3D", r"(?<!JPH::)Shape(s?)([23]D)", r"Collider\1\2"),
    Rule("shape2D/3D", P + r"shape(s?)([23]D)", r"collider\1\2"),
    Rule("MAX_SHAPES_2D/3D", r"MAX_SHAPE(S?)_", r"MAX_COLLIDER\1_"),
    Rule("maxShape*", P + r"maxShape(s?)(?=[A-Z0-9])", r"maxCollider\1"),
    Rule("create*Shape", P + r"(create[A-Za-z]*?)Shape" + E, r"\1Collider"),
    Rule("get/set/is Shape<Property>", P + r"(get|set|is)Shape(Density|Friction|Restitution|Sensor|EnableHitEvents|"
         r"ContactEvents|PreSolveEvents|SensorEvents|Type|Contacts|Index[A-Z0-9]?)" + E, r"\1Collider\2"),
    Rule("*Shapes (body API)", P + r"(getNum|removeAll|applyMassFrom|num|needUpdate)Shapes" + E, r"\1Colliders"),
    Rule("Body*D shape", r"(Body[23]D|body[23][dD])(_?)([Ss])hape(s?)",
         lambda m: m.group(1) + m.group(2) + ("C" if m.group(3) == "S" else "c") + "ollider" + m.group(4)),
    Rule("shapeIndex", B + r"shapeIndex" + E, "colliderIndex"),
    Rule("shapes declaration", r"(Collider[23]D\b[^;\n]*?\s)shapes(?=\s*[;\[=])", r"\1colliders", "C"),
    Rule(".shapes member", r"(?<=\.|>|:)(?<!objModel\.)(?<!objModel->)shapes" + E, "colliders", "CS"),
    Rule("ShapeParameters", r"ShapeParameters", "PrimitiveParameters"),
    variants("Body2D", "Body2D", "Rigidbody2D"),
    variants("Body3D", "Body3D", "Rigidbody"),
    Rule("SkyBox", r"SkyBox", "Skybox"),
    Rule("SkyComponent", r"SkyComponent", "SkyboxComponent"),
    Rule("SKY_COMPONENT", r"SKY_COMPONENT", "SKYBOX_COMPONENT"),
    variants("Occluder2D", "Occluder2D", "ShadowCaster2D"),
    Rule("OCCLUDER_2D", P + r"OCCLUDER_2D", "SHADOW_CASTER_2D"),
    Rule("occluder_2d", P + r"occluder_2d", "shadow_caster_2d"),
    Rule("SoundComponent", r"SoundComponent", "AudioSourceComponent"),
    Rule("SOUND_COMPONENT", r"SOUND_COMPONENT", "AUDIO_SOURCE_COMPONENT"),
    Rule("SoundState/SoundAttenuation", r"Sound(State|Attenuation)", r"AudioSource\1"),
    Rule("soundState/soundAttenuation", P + r"sound(State|Attenuation)", r"audioSource\1"),
    Rule("UpdateFlags_Sound", r"UpdateFlags_Sound" + E, "UpdateFlags_AudioSource"),
    Rule("kSoundProperties", r"kSoundProperties", "kAudioSourceProperties"),
    Rule("LinesComponent", r"LinesComponent", "LineRendererComponent"),
    Rule("LINES_COMPONENT", r"LINES_COMPONENT", "LINE_RENDERER_COMPONENT"),
    Rule("ParticlesComponent", r"ParticlesComponent", "ParticleSystemComponent"),
    Rule("PARTICLES_COMPONENT", r"PARTICLES_COMPONENT", "PARTICLE_SYSTEM_COMPONENT"),
    variants("UILayout", "UILayout", "RectTransform"),
    Rule("UI_LAYOUT", P + r"UI_LAYOUT", "RECT_TRANSFORM"),
    variants("UIContainer", "UIContainer", "LayoutGroup"),
    Rule("UI_CONTAINER", P + r"UI_CONTAINER", "LAYOUT_GROUP"),
    Rule("ContainerType", r"ContainerType", "LayoutGroupType"),
    Rule("containerType", P + r"containerType", "layoutGroupType"),
    Rule("TextEdit (Pascal)", r"(?<!Custom)TextEdit(?!or)", "InputField"),
    Rule("TextEdit (camel)", P + r"textEdit(?!or)", "inputField"),
    Rule("TextEdit (lower)", P + r"textedit(?!or)", "inputfield"),
    Rule("TextEdit (Title)", P + r"Textedit(?!or)", "Inputfield"),
    Rule("TextEdit (UPPER)", P + r"(?<!STB_)TEXTEDIT", "INPUT_FIELD"),
    variants("TimedAction", "TimedAction", "Tween"),
)
for _kind in ("Position", "Rotation", "Scale", "Color", "Alpha"):
    add(variants(f"{_kind}Action", f"{_kind}Action", f"{_kind}Tween"),
        Rule(f"{_kind.upper()}_ACTION", P + rf"{_kind.upper()}_ACTION", f"{_kind.upper()}_TWEEN"),
        Rule(f"{_kind.lower()}_action", P + rf"{_kind.lower()}_action", f"{_kind.lower()}_tween"))

# Include guards of renamed headers whose names are generic words.
GUARDS = {"OBJECT_H": "GAMEOBJECT_H", "SHAPE_H": "PRIMITIVE_H", "LINES_H": "LINE_RENDERER_H",
          "Sound_h": "AudioSource_h", "CONTAINER_H": "LAYOUT_GROUP_H", "PARTICLES_H": "PARTICLE_SYSTEM_H"}
add(Rule("include guards", B + "(" + "|".join(GUARDS) + ")" + E, mapper(GUARDS), "CM",
         only=(r"^engine/core/(object|action)/",)))


# Bundle -> Prefab. Two rules: compound identifiers, and the plain word (also in
# comments and UI strings), which keeps Apple/Android bundle phrases.
def _bundle_token(m):
    tok = m.group(0)
    if BUNDLE_KEEP.search(tok):
        return tok
    tok = re.sub(r"EntityBundle", "Prefab", tok)
    # "prefab" alone would merge with the many "bundle" variables in the same scope.
    tok = re.sub(r"(?<![A-Za-z0-9])entityBundle", "prefabAsset", tok)
    tok = tok.replace("Bundle", "Prefab").replace("bundle", "prefab").replace("BUNDLE", "PREFAB")
    return tok


_BUNDLE_WORD = r"[Bb]undles?|BUNDLES?"
# The plain word stays when it names an application bundle; the context may span
# a line break inside a comment.
_BUNDLE_BEFORE = re.compile(r"(?:\bapp|\bApp|\.app|\bApple|\bmacOS|\biOS|\bpreload|\bXcode|\"Program Files\", a)"
                            r"[\s/*#]*$")
_BUNDLE_AFTER = re.compile(r"^\s*(?:[Ii]dentifier|display name|settings apply|precompiled|DESTINATION)")


def _bundle_word(m):
    word = m.group(0)
    before = m.string[max(0, m.start() - 40):m.start()]
    after = m.string[m.end():m.end() + 20]
    if _BUNDLE_BEFORE.search(before) or _BUNDLE_AFTER.search(after):
        return word
    return word.replace("Bundle", "Prefab").replace("bundle", "prefab").replace("BUNDLE", "PREFAB")
add(
    Rule("bundle (compound identifiers)",
         rf"{B}(?=[{ID}]*(?:[Bb]undle|BUNDLE))(?![Bb]undles?{E})(?!BUNDLES?{E})[{ID}]+", _bundle_token, "CMST",
         only=TREE_BUNDLE_SCOPE),
    Rule("bundle (word)", B + rf"(?:{_BUNDLE_WORD})" + E, _bundle_word, "CMST", only=TREE_BUNDLE_SCOPE,
         skip=(r"^editor/Exporter\.cpp$",)),
)

# Editor windows (editor code only).
add(
    Rule("editor window classes", B + r"(Structure|Properties)" + E,
         mapper({"Structure": "HierarchyWindow", "Properties": "InspectorWindow"}), "C", only=EDITOR, project=False),
    Rule("ResourcesWindow", r"ResourcesWindow", "ProjectWindow", only=EDITOR, project=False),
    Rule("OutputWindow", r"OutputWindow", "ConsoleWindow", only=EDITOR, project=False),
    Rule("editor window members", P + r"(structure|properties|resources|output)Window" + E,
         lambda m: PANEL_KEYS[m.group(1)] + "Window", only=EDITOR, project=False),
    Rule("editor window getters", r"(?<=[gs]et)(Structure|Properties)Window" + E,
         lambda m: WINDOW_TITLES[m.group(1)] + "Window", only=EDITOR, project=False),
    Rule("toggle panel commands", r"(?<=Toggle)(Structure|Properties|Resources|Output)" + E,
         mapper(WINDOW_TITLES), only=EDITOR, project=False),
    Rule("phrase: bare Structure in comments", B + r"Structure" + E + r"(?! is read)", "Hierarchy", "M",
         only=EDITOR, project=False),
)

# Classes named with generic words: code only, so the English words in comments
# and UI strings stay.
add(
    # "2D Object"/"3D Object" are Unity's own GameObject menu names
    Rule("Object", B + r"(?<!JPH::)(?<!/)(?<!2D )(?<!3D )Object" + E + r"(?!/)(?! ids)(?! files)(?! form)(?! code)",
         "GameObject", "CMS"),
    Rule("Shape", B + r"(?<!JPH::)(?<!/)Shape" + E + r"(?!/)(?!::Release)", "Primitive", "CM"),
    Rule("Sound", B + r"(?<!ResourceType::)Sound" + E, "AudioSource", "C",
         skip=(r"^engine/core/thread/ResourceProgress\.",)),
    Rule("Lines", B + r"Lines" + E, "LineRenderer", "C"),
    Rule("Particles", B + r"Particles" + E, "ParticleSystem", "C"),
    Rule("Container", B + r"Container" + E, "LayoutGroup", "C"),
)


add(ARTICLE_RULES)


class FileNames:
    """Rewrites references to renamed source files, e.g. #include "Body2D.h" or
    core/object/physics/Body2D.cpp in CMake. A reference with directories must
    match the end of a renamed path; a bare file name matches any renamed file."""

    name = "file names"
    kinds = set("CMST")

    def __init__(self, renames):
        self.by_name = defaultdict(list)
        for old, new in renames.items():
            self.by_name[PurePosixPath(old).name].append((old, new))
        names = "|".join(re.escape(n) for n in sorted(self.by_name, key=len, reverse=True)) or r"(?!)"
        self.re = re.compile(rf"(?<![\w.-])((?:[\w.-]+/)*)({names})(?![\w.])")

    def applies(self, path, kind, project):
        return kind in self.kinds

    def repl(self, m):
        prefix, name = m.group(1), m.group(2)
        for old, new in self.by_name[name]:
            if not prefix or old.endswith("/" + prefix + name) or old == prefix + name:
                return prefix + PurePosixPath(new).name
        return m.group(0)


# --------------------------------------------------------------------------------------
# Lexing
# --------------------------------------------------------------------------------------

C_LEX = re.compile(r'''
    (?P<M>//[^\n]*|/\*.*?\*/)
  | (?P<S>R"(?P<d>[^()\\\s"]{0,16})\(.*?\)(?P=d)"|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n]){1,8}')
''', re.S | re.X)
LUA_LEX = re.compile(r'''
    (?P<M>--\[(?P<e1>=*)\[.*?\](?P=e1)\]|--[^\n]*)
  | (?P<S>\[(?P<e2>=*)\[.*?\](?P=e2)\]|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
''', re.S | re.X)
SEP = "\x00"  # never present in a text file


def file_kind(path):
    name = PurePosixPath(path).name
    ext = PurePosixPath(path).suffix.lower()
    if name == "project.pbxproj":
        return "pbxproj"
    if ext in C_EXTS or name.endswith(".h.in"):
        return "c"
    if ext in LUA_EXTS:
        return "lua"
    return "text"


def segments(text, lexer):
    out, pos = [], 0
    for m in lexer.finditer(text):
        if m.start() > pos:
            out.append(["C", text[pos:m.start()]])
        out.append(["M" if m.group("M") is not None else "S", m.group(0)])
        pos = m.end()
    if pos < len(text):
        out.append(["C", text[pos:]])
    return out


# --------------------------------------------------------------------------------------
# Rewriting
# --------------------------------------------------------------------------------------

class Rewriter:
    def __init__(self, renames):
        self.renames = renames
        self.filenames = FileNames(renames)
        self.phase2 = [self.filenames] + PHASE2
        self.counts = Counter()

    def _sub(self, rule, text):
        new, n = rule.re.subn(rule.repl, text)
        if n and new != text:
            self.counts[rule.name] += n
        return new

    def rewrite(self, path, text, project=False):
        kind = file_kind(path)
        if kind == "pbxproj":
            return self.rewrite_pbxproj(path, text)
        if kind in ("c", "lua"):
            for r in PHASE1:
                if r.applies(path, "W", project):
                    text = self._sub(r, text)
            segs = segments(text, C_LEX if kind == "c" else LUA_LEX)
        else:
            segs = [["T", text]]
        # Rules run once per segment kind over all segments of that kind joined.
        for seg_kind in ("C", "M", "S", "T"):
            idx = [i for i, s in enumerate(segs) if s[0] == seg_kind]
            if not idx:
                continue
            joined = SEP.join(segs[i][1] for i in idx)
            for r in self.phase2:
                if r.applies(path, seg_kind, project):
                    joined = self._sub(r, joined)
            for i, part in zip(idx, joined.split(SEP)):
                segs[i][1] = part
        return "".join(s[1] for s in segs)

    def rewrite_pbxproj(self, path, text):
        """Renames the file references of renamed files only; the Xcode project
        also lists third-party files with the same names (Jolt's Shape.cpp)."""
        base = PurePosixPath(path).parent.parent  # the directory holding *.xcodeproj
        objects = {}
        obj = re.compile(r"^\t\t([0-9A-F]{24})(?: /\* .*? \*/)? = \{(?:([^\n]*)\};$|\n(.*?)^\t\t\};$)", re.S | re.M)
        for m in obj.finditer(text):
            objects[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
        parent = {}
        for oid, body in objects.items():
            cm = re.search(r"children = \((.*?)\);", body, re.S)
            if cm:
                for child in re.findall(r"([0-9A-F]{24})", cm.group(1)):
                    parent[child] = oid

        def field(body, key):
            fm = re.search(rf"\b{key} = (\"[^\"]*\"|[^;]*);", body)
            return fm.group(1).strip('"') if fm else None

        def full_path(oid, depth=0):
            body = objects.get(oid, "")
            p = field(body, "path")
            tree = field(body, "sourceTree")
            if depth > 64:
                return None
            if tree == "<group>" or tree is None:
                up = parent.get(oid)
                prefix = full_path(up, depth + 1) if up else str(base)
                if prefix is None:
                    return None
                return os.path.normpath(os.path.join(prefix, p)) if p else prefix
            if tree == "SOURCE_ROOT":
                return os.path.normpath(os.path.join(str(base), p or ""))
            return None

        renamed = {}  # file reference id -> (old name, new name)
        for oid, body in objects.items():
            if "isa = PBXFileReference" not in body:
                continue
            fp = full_path(oid)
            if fp and fp in self.renames:
                renamed[oid] = (PurePosixPath(fp).name, PurePosixPath(self.renames[fp]).name)
        build_files = {}
        for oid, body in objects.items():
            fm = re.search(r"fileRef = ([0-9A-F]{24})", body)
            if "isa = PBXBuildFile" in body and fm and fm.group(1) in renamed:
                build_files[oid] = renamed[fm.group(1)]
        ids = dict(renamed, **build_files)
        if not ids:
            return text

        def fix_line(line):
            for oid in re.findall(r"[0-9A-F]{24}", line):
                if oid in ids:
                    old, new = ids[oid]
                    line = re.sub(rf"(?<![\w.]){re.escape(old)}(?![\w.])", new, line)
            return line

        out = "\n".join(fix_line(l) for l in text.split("\n"))
        if out != text:
            self.counts["Xcode file references"] += sum(1 for a, b in zip(text.split("\n"), out.split("\n")) if a != b)
        return out


# --------------------------------------------------------------------------------------
# Repository helpers
# --------------------------------------------------------------------------------------

def repo_root():
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True)
    return Path(out.stdout.strip())


def tracked_files(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True)
    return [p for p in out.stdout.decode().split("\0") if p and (root / p).is_file() and not (root / p).is_symlink()]


def in_scope(path):
    return path not in EXCLUDED_FILES and not path.startswith(EXCLUDED_PREFIXES)


def read_text(full):
    data = full.read_bytes()
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("latin-1")


DATA_BLOB = re.compile(r"0x[0-9A-Fa-f]{2}\s*,")


def is_data_blob(text):
    """Generated byte-array headers (icons, fonts, sky textures): nothing to rename."""
    return len(DATA_BLOB.findall(text[:8192])) > 400


def text_files(root, files):
    for p in files:
        if not in_scope(p):
            continue
        text = read_text(root / p)
        if text is not None and not is_data_blob(text):
            yield p, text


def all_renames(files):
    """Old path -> new path for every tracked source file in FILE_RENAMES."""
    tracked = set(files)
    return {old: new for old, new in static_renames().items() if old in tracked}


def static_renames():
    """All FILE_RENAMES paths with every source extension, for references in
    projects and in trees where the files are already renamed."""
    out = {}
    for stem, new_stem in FILE_RENAMES.items():
        parent = str(PurePosixPath(stem).parent)
        for ext in SOURCE_EXTS:
            out[stem + ext] = f"{parent}/{new_stem}{ext}"
    return out


def token_changes(old_text, new_text):
    """(old token, new token) pairs for identifiers renamed between two texts."""
    ident = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    pairs = Counter()
    old_lines, new_lines = old_text.split("\n"), new_text.split("\n")
    if len(old_lines) != len(new_lines):
        old_lines, new_lines = [old_text], [new_text]
    for a, b in zip(old_lines, new_lines):
        if a == b:
            continue
        ta, tb = ident.findall(a), ident.findall(b)
        if len(ta) == len(tb):
            pairs.update((x, y) for x, y in zip(ta, tb) if x != y)
            continue
        sm = difflib.SequenceMatcher(a=ta, b=tb, autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == "replace" and i2 - i1 == j2 - j1:
                pairs.update((x, y) for x, y in zip(ta[i1:i2], tb[j1:j2]) if x != y)
    return pairs


# --------------------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------------------

# New names that third-party code also uses; the rules never touch these.
THIRD_PARTY_NAMES = [
    "SoLoud::AudioSource (engine/core/subsystem/AudioSystem.cpp) is always qualified; jasperengine::AudioSource is new",
    "tinygltf::Primitive (engine/core/subsystem/MeshSystem.cpp) is always qualified; jasperengine::Primitive is new",
    "JPH::Shape, the Jolt Collision/Shape/ include paths and Jolt's Shape.cpp/Object.h in the Xcode project keep their names",
    "DXGI_SWAP_CHAIN_DESC::OutputWindow (engine/platform/win) is not the editor's OutputWindow",
    "Android's prefab (engine/workspaces/androidstudio) and android.os.Bundle are unrelated to prefabs",
    "Apple bundles (CFBundle*, NSBundle, MACOSX_BUNDLE, bundle identifiers, .app bundles) stay bundles",
]


def cmd_dry_run(root, files):
    renames = all_renames(files)
    rw = Rewriter(static_renames())
    changed = 0
    merges, collisions = [], []
    per_file = {}
    for p, text in text_files(root, files):
        new = rw.rewrite(p, text)
        if new != text:
            changed += 1
            per_file[p] = (text, new)
    renamed_tokens = defaultdict(set)
    for p, (old, new) in per_file.items():
        pairs = token_changes(old, new)
        for (a, b) in pairs:
            renamed_tokens[b].add(a)
        # Merges and collisions only matter in code.
        kind = file_kind(p)
        if kind not in ("c", "lua"):
            continue
        lexer = C_LEX if kind == "c" else LUA_LEX
        old_code = "\n".join(t for k, t in segments(old, lexer) if k == "C")
        new_code = "\n".join(t for k, t in segments(new, lexer) if k == "C")
        into = defaultdict(set)
        for (a, b) in token_changes(old_code, new_code):
            into[b].add(a)
        old_tokens = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", old_code))
        for b, olds in into.items():
            if len(olds) > 1:
                merges.append(f"{p}: {', '.join(sorted(olds))} -> {b}")
            if b in old_tokens and b not in olds:
                collisions.append(f"{p}: {', '.join(sorted(olds))} -> {b}, which the file's code already uses")
    print("Content rules (matches):")
    rule_names = [r.name for r in [rw.filenames] + PHASE1 + PHASE2] + ["Xcode file references"]
    for name in rule_names:
        if rw.counts[name]:
            print(f"  {rw.counts[name]:6d}  {name}")
    print(f"  {changed} files would change")
    print(f"\nPath renames ({len(renames)}):")
    for old, new in sorted(renames.items()):
        print(f"  {old}\n    -> {new}")
    print(f"\nIdentifier renames ({len(renamed_tokens)} new names):")
    for b in sorted(renamed_tokens):
        print(f"  {', '.join(sorted(renamed_tokens[b]))} -> {b}")
    print(f"\nMerges: distinct old identifiers that become one name in the same file ({len(merges)}):")
    for m in merges:
        print("  " + m)
    print(f"\nCollisions: a new name the file already used ({len(collisions)}):")
    for c in collisions:
        print("  " + c)
    print("\nThird-party names left alone:")
    for n in THIRD_PARTY_NAMES:
        print("  " + n)
    return 0


def cmd_apply_paths(root, files):
    renames = all_renames(files)
    for old, new in sorted(renames.items(), key=lambda kv: -kv[0].count("/")):
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "mv", old, new], cwd=root, check=True)
    print(f"Renamed {len(renames)} paths")
    return 0


def cmd_apply_content(root, files):
    rw = Rewriter(static_renames())
    changed = 0
    for p, text in text_files(root, files):
        new = rw.rewrite(p, text)
        if new != text:
            (root / p).write_bytes(new.encode("utf-8"))
            changed += 1
    print(f"Rewrote {changed} files, {sum(rw.counts.values())} replacements")
    return 0


# Old names that must not survive anywhere in scope, whichever rule was meant to
# catch them. Lines matching CHECK_ALLOW_LINE are fine (Apple/Android bundles,
# third-party names).
CHECK_PATTERNS = [
    r"Body[23]D", P + r"body[23][dD]", r"SoundComponent", r"SkyBox", r"SkyComponent", r"Occluder2D",
    r"UILayout", r"UIContainer", r"(?<!Custom)TextEdit(?!or)", r"TimedAction",
    r"(Position|Rotation|Scale|Color|Alpha)Action", r"EntityBundle", r"BundleManager", r"BundleComponent",
    r"ResourcesWindow", r"(?<!\.)OutputWindow", r"(?<!Game)ObjectClasses", r"LinesComponent", r"ParticlesComponent",
    r"(?<!JPH::)Shape[23]D", r"numShapes", r"MAX_SHAPES", r"\.bundle\b",
]
CHECK_ALLOW_LINE = re.compile(
    r"CFBundle|NSBundle|mainBundle|[Bb]undle[ -]?[Ii]dentifier|BUNDLE|\.app\b|app bundle|preload bundle|"
    r"android\.os\.Bundle|Bundle savedInstanceState|[Bb]undled|macOS bundle|iOS bundle|the bundle|"
    r"Bundle precompiled|Bundle settings|Bundle: |codesign|embedding into")


def cmd_check(root, files):
    problems = []
    for old, new in sorted(all_renames(files).items()):
        problems.append(f"path: {old} (should be {new})")
    rw = Rewriter(static_renames())
    pattern = re.compile("|".join(f"(?:{p})" for p in CHECK_PATTERNS))
    for p, text in text_files(root, files):
        new = rw.rewrite(p, text)
        if new != text:
            for n, (a, b) in enumerate(zip(text.split("\n"), new.split("\n")), 1):
                if a != b:
                    problems.append(f"{p}:{n}: pending rename: {a.strip()[:150]}")
        for n, line in enumerate(text.split("\n"), 1):
            if pattern.search(line) and not CHECK_ALLOW_LINE.search(line):
                problems.append(f"{p}:{n}: old name: {line.strip()[:150]}")
    for line in problems:
        print(line)
    print(f"{len(problems)} occurrence(s) outside the allowlist")
    return 1 if problems else 0


# --------------------------------------------------------------------------------------
# Game project migration
# --------------------------------------------------------------------------------------

PROJECT_SKIP_DIRS = {".git", ".jasperengine", "build", "node_modules"}
# Generated by the editor on open/build; stale copies would still name old headers.
PROJECT_REGENERATED = (".jasperengine/engine-api", ".jasperengine/generated")

YAML_KEY_MAP = {"shapes": "colliders"}


def migrate_yaml(text, rw, path):
    """Scene, prefab and project YAML: keys and the values that name classes."""
    out = []
    comp_parent = None  # indent of the current "components:" key
    comp_indent = None  # indent of its children
    for line in text.split("\n"):
        m = re.match(r"^(\s*)(- )?([A-Za-z_][A-Za-z0-9_]*)(:)(.*)$", line)
        if not m:
            lm = re.match(r"^(\s*- )(.*)$", line)
            if lm:
                line = lm.group(1) + re.sub(r"\.bundle\b", ".prefab", lm.group(2))
            out.append(line)
            continue
        indent = len(m.group(1)) + (2 if m.group(2) else 0)
        key, rest = m.group(3), m.group(5)
        if comp_parent is not None and indent <= comp_parent:
            comp_parent = comp_indent = None
        is_component = False
        if comp_parent is not None:
            if comp_indent is None:
                comp_indent = indent
            is_component = indent == comp_indent and not m.group(2)
        if is_component:
            key = COMPONENT_KEY_MAP.get(key, key)
        elif key in YAML_KEY_MAP:
            key = YAML_KEY_MAP[key]
        else:
            # Keys the editor writes from renamed identifiers (bundlePath,
            # maxShapes2D, ...) follow the same identifier rules as its source.
            key = rw.rewrite("project/" + path + ".key", key, project=True)
        if key == "components" and rest.strip() == "":
            comp_parent, comp_indent = indent, None
        vm = re.match(r"^(\s*)(\S.*?)(\s*)$", rest)
        if vm:
            v = vm.group(2)
            if key == "ptrTypeName" and v in CLASS_MAP:
                v = CLASS_MAP[v]
            elif key == "type" and v == "EntityBundle":
                v = "Prefab"
            v = re.sub(r"\.bundle\b", ".prefab", v)
            rest = vm.group(1) + v + vm.group(3)
        out.append(f"{m.group(1)}{m.group(2) or ''}{key}{m.group(4)}{rest}")
    return "\n".join(out)


def cmd_project(project_dir, dry_run):
    project = Path(project_dir).resolve()
    if not (project / "project.yaml").is_file():
        print(f"{project} has no project.yaml", file=sys.stderr)
        return 1
    rw = Rewriter(static_renames())
    changed, renamed, removed = [], [], []
    for dirpath, dirnames, filenames in os.walk(project):
        dirnames[:] = [d for d in dirnames if d not in PROJECT_SKIP_DIRS]
        for fn in filenames:
            full = Path(dirpath) / fn
            rel = str(full.relative_to(project)).replace(os.sep, "/")
            if full.is_symlink():
                continue
            text = read_text(full)
            if text is None:
                continue
            ext = full.suffix.lower()
            if ext in YAML_EXTS:
                new = migrate_yaml(text, rw, rel)
            else:
                new = rw.rewrite("project/" + rel, text, project=True)
                if ext in (".md", ".txt", ".cmake") or fn == "CMakeLists.txt":
                    new = re.sub(r"\.bundle\b", ".prefab", new)
            if new != text:
                changed.append(rel)
                if not dry_run:
                    full.write_bytes(new.encode("utf-8"))
            if ext == ".bundle":
                renamed.append((rel, rel[: -len(".bundle")] + ".prefab"))
    for rel in PROJECT_REGENERATED:
        if (project / rel).exists():
            removed.append(rel)
            if not dry_run:
                shutil.rmtree(project / rel)
    for old, new in renamed:
        if not dry_run:
            (project / old).rename(project / new)
    verb = "Would" if dry_run else "Did"
    print(f"{verb} rewrite {len(changed)} files:", *changed, sep="\n  ")
    print(f"{verb} rename {len(renamed)} prefab files:", *[f"{a} -> {b}" for a, b in renamed], sep="\n  ")
    print(f"{verb} remove {len(removed)} regenerated folders:", *removed, sep="\n  ")
    print("Rule matches:")
    for name, n in sorted(rw.counts.items(), key=lambda kv: -kv[1]):
        print(f"  {n:6d}  {name}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply-paths", action="store_true")
    mode.add_argument("--apply-content", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--project", metavar="DIR")
    parser.add_argument("--report-only", action="store_true", help="with --project: change nothing")
    args = parser.parse_args()

    if args.project:
        return cmd_project(args.project, args.report_only)
    root = repo_root()
    files = tracked_files(root)
    if args.dry_run:
        return cmd_dry_run(root, files)
    if args.apply_paths:
        return cmd_apply_paths(root, files)
    if args.apply_content:
        return cmd_apply_content(root, files)
    return cmd_check(root, files)


if __name__ == "__main__":
    sys.exit(main())
