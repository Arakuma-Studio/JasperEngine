#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Keeps the engine's Apple privacy manifest honest (docs/shipping.md "Privacy manifest"): App Store
# Connect refuses apps that call a required-reason API without declaring it. Scans the code that
# runs on Apple platforms for each category's API names and fails when a category is used but
# engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy does not declare it (the editor's
# ApplePrivacy::engineManifest writes the same file, ApplePrivacyTest keeps them equal).
#
#   python3 tools/check_apple_privacy.py [--root DIR]     exit 1 with the uses of an undeclared category

import argparse
import os
import plistlib
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# what compiles on Apple: the engine core, the Apple and macOS platform code, and the vendored
# libraries (their tests, tools and other platforms' backends aside)
SCANNED = ("engine/core", "engine/platform/apple", "engine/platform/mac", "engine/platform/common", "engine/libs")
SKIPPED = re.compile(r"(^|/)(tests?|test|examples?|docs?|tools|benchmarks?|win32|windows|android|emscripten|linux)(/|$)", re.I)
SOURCE = re.compile(r"\.(c|cc|cpp|cxx|h|hh|hpp|inl|m|mm)$")

# Apple's required reason API categories (developer.apple.com, "Describing use of required reason
# API") and how their APIs are named in C, C++ and Objective-C
CATEGORIES = {
    "FileTimestamp": re.compile(r"\b(f?l?stat|fstatat|getattrlist|getattrlistbulk|fgetattrlist|getattrlistat)\s*\(|"
                                r"\bst_[amc]time|\blast_write_time\b|NSFileCreationDate|NSFileModificationDate|"
                                r"\b(creationDate|contentModificationDate|attributeModificationDate)\b"),
    "SystemBootTime": re.compile(r"\bmach_absolute_time\b|\bsystemUptime\b|kern\.boottime|KERN_BOOTTIME"),
    "DiskSpace": re.compile(r"\b(f?statv?fs)\s*\(|filesystem::space\b|NSFileSystemFreeSize|NSFileSystemSize\b|"
                            r"volumeAvailableCapacity|volumeTotalCapacity"),
    "ActiveKeyboards": re.compile(r"\bactiveInputModes\b"),
    "UserDefaults": re.compile(r"\bNSUserDefaults\b|\bUserDefaults\b"),
}


def declared(root):
    path = os.path.join(root, "engine", "workspaces", "xcode", "ios", "PrivacyInfo.xcprivacy")
    with open(path, "rb") as f:
        manifest = plistlib.load(f)
    prefix = "NSPrivacyAccessedAPICategory"
    return {item["NSPrivacyAccessedAPIType"][len(prefix):] for item in manifest.get("NSPrivacyAccessedAPITypes", [])
            if item.get("NSPrivacyAccessedAPIType", "").startswith(prefix) and item.get("NSPrivacyAccessedAPITypeReasons")}


def uses(root):
    """category -> [path:line] of its API names in Apple-compiled code"""
    found = {}
    for base in SCANNED:
        for directory, dirs, files in os.walk(os.path.join(root, base)):
            relative_dir = os.path.relpath(directory, root).replace(os.sep, "/")
            dirs[:] = sorted(d for d in dirs if not SKIPPED.search(relative_dir + "/" + d + "/"))
            for name in sorted(files):
                if not SOURCE.search(name):
                    continue
                path = os.path.join(directory, name)
                with open(path, encoding="utf-8", errors="replace") as f:
                    for number, line in enumerate(f, 1):
                        code = line.split("//", 1)[0]
                        for category, pattern in CATEGORIES.items():
                            if pattern.search(code):
                                found.setdefault(category, []).append(f"{relative_dir}/{name}:{number}")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=ROOT, help="the repository (default: this one)")
    args = parser.parse_args()
    have = declared(args.root)
    missing = {category: places for category, places in uses(args.root).items() if category not in have}
    for category, places in sorted(missing.items()):
        print(f"{category} is used but the engine's privacy manifest does not declare it "
              f"(engine/workspaces/xcode/*/PrivacyInfo.xcprivacy and ApplePrivacy::engineManifest):")
        for place in places[:10]:
            print(f"  {place}")
        if len(places) > 10:
            print(f"  ... {len(places) - 10} more")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
