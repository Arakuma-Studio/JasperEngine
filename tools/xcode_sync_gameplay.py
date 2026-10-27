#!/usr/bin/env python3
# Adds the engine sources of S22 (gameplay: input actions, character controllers, water) that the
# Xcode workspace does not list yet. CMake lists them in engine/CMakeLists.txt; the Xcode project
# lists files explicitly:
#
#   python3 tools/xcode_sync_gameplay.py [--check]
#
# Each file joins the groups and build phases of a sibling file the project already has (its
# path is relative to the sibling's group), as tools/htmlui/xcode_sync_widgets.py does for its
# single files. Object ids are derived from the file path ("COMP" + sha1 of "s22/<file>"), so
# they never collide with other branches' ids. --check exits 1 when files are missing (nothing
# is written).

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "htmlui"))
import xcode_sync_compat as base  # noqa: E402

# (file under engine/core, the sibling under engine/core whose groups and build phases it joins)
FILES = [
    ("input/InputControlPath.cpp", "Input.cpp"),
    ("input/InputControlPath.h", "Input.h"),
    ("input/InputActionAsset.cpp", "Input.cpp"),
    ("input/InputActionAsset.h", "Input.h"),
    ("input/InputActionAssetJson.h", "Input.h"),
    ("input/InputActions.cpp", "Input.cpp"),
    ("input/InputActions.h", "Input.h"),
    ("input/InputRebind.cpp", "Input.cpp"),
    ("input/InputRebind.h", "Input.h"),
    ("input/InputVirtualControls.cpp", "Input.cpp"),
    ("input/InputVirtualControls.h", "Input.h"),
    ("script/binding/GameplayClassesLua.cpp", "script/binding/ProfilerClassesLua.cpp"),
    ("component/CharacterCollision.h", "component/Joint2DComponent.h"),
    ("component/CharacterController2DComponent.h", "component/Joint2DComponent.h"),
    ("subsystem/CharacterController2DSystem.cpp", "subsystem/PhysicsSystem.cpp"),
    ("subsystem/CharacterController2DSystem.h", "subsystem/PhysicsSystem.h"),
    ("object/physics/CharacterController2D.cpp", "object/physics/Joint2D.cpp"),
    ("object/physics/CharacterController2D.h", "object/physics/Joint2D.h"),
    ("component/CharacterControllerComponent.h", "component/Joint3DComponent.h"),
    ("object/physics/ControllerColliderHit.h", "object/physics/Joint3D.h"),
    ("object/physics/CharacterController.cpp", "object/physics/Joint3D.cpp"),
    ("object/physics/CharacterController.h", "object/physics/Joint3D.h"),
    ("subsystem/CharacterControllerSystem.cpp", "subsystem/PhysicsSystem.cpp"),
    ("subsystem/CharacterControllerSystem.h", "subsystem/PhysicsSystem.h"),
    ("util/WaterWaves.h", "object/environment/Water.h"),
    ("component/WaterVolumeComponent.h", "component/Joint3DComponent.h"),
    ("component/BuoyancyComponent.h", "component/Joint3DComponent.h"),
    ("subsystem/WaterSystem.cpp", "subsystem/PhysicsSystem.cpp"),
    ("subsystem/WaterSystem.h", "subsystem/PhysicsSystem.h"),
    ("object/environment/WaterVolume.cpp", "object/environment/Water.cpp"),
    ("object/environment/WaterVolume.h", "object/environment/Water.h"),
    ("object/physics/Buoyancy.cpp", "object/physics/Joint3D.cpp"),
    ("object/physics/Buoyancy.h", "object/physics/Joint3D.h"),
]


def file_type(name):
    return "sourcecode.c.h" if name.endswith(".h") else "sourcecode.cpp.cpp"


def add_file(lines, relative, sibling_relative):
    name = os.path.basename(relative)
    sibling = os.path.basename(sibling_relative)
    group_path = os.path.relpath(relative, os.path.dirname(sibling_relative) or ".").replace(os.sep, "/")
    path = "s22/" + relative
    ref = base.object_id("ref", path)
    reference = re.compile(r"^\t\t([0-9A-F]{24}) /\* " + re.escape(sibling) + r" \*/ = \{isa = PBXFileReference;.*path = "
                           + re.escape(sibling) + ";")
    sibling_ref = next((m.group(1) for m in map(reference.match, lines) if m), None)
    if sibling_ref is None:
        raise SystemExit("the sibling %s is not in the Xcode project" % sibling_relative)
    base.insert_after_line(lines, "%s /* %s */ = {isa = PBXFileReference;" % (sibling_ref, sibling), [
        "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
        % (ref, name, file_type(name), group_path)])
    # the groups listing the sibling
    child = "%s /* %s */," % (sibling_ref, sibling)
    for i in [i for i, line in enumerate(lines) if line.strip() == child][::-1]:
        lines.insert(i + 1, "\t\t\t\t%s /* %s */," % (ref, name))
    # the sibling's build files and the phases listing them
    phase = "Headers" if name.endswith(".h") else "Sources"
    build_file = re.compile(r"^\t\t([0-9A-F]{24}) /\* " + re.escape(sibling) + " in " + phase + r" \*/ = \{isa = PBXBuildFile;")
    builds = [m.group(1) for m in map(build_file.match, lines) if m]
    for index, sibling_build in enumerate(builds):
        build = base.object_id("%s%d" % (phase, index + 1), path)
        base.insert_after_line(lines, "%s /* %s in %s */ = {isa = PBXBuildFile;" % (sibling_build, sibling, phase), [
            "\t\t%s /* %s in %s */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, phase, ref, name)])
        entry = "%s /* %s in %s */," % (sibling_build, sibling, phase)
        for i in [i for i, line in enumerate(lines) if line.strip() == entry][::-1]:
            lines.insert(i + 1, "\t\t\t\t%s /* %s in %s */," % (build, name, phase))


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    missing = [(relative, sibling) for relative, sibling in FILES if base.object_id("ref", "s22/" + relative) not in text]
    if not missing:
        print("Xcode project lists every S22 file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(relative for relative, _ in missing))
        return 1
    for relative, sibling in missing:
        add_file(lines, relative, sibling)
    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
