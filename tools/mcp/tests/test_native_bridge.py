"""Native bridge (spec 2026-10-31-native-bridge section 9, MCP e2e): native_plugins create/list
round-trip and refusal to overwrite; the native settings keys through set/get_project_settings
(and their refusals); editor Play calls a C++ plugin from plugins/native/cpp and a Lua mock, twice
(the Play library reloads: one registration, no stale state); a source export writes the Android
nativeplugins module (registry, keep rules, dependencies, repositories, manifest fragments), the
Xcode project (plugin sources per target, SDK frameworks linked or embedded, system frameworks,
Swift packages, bridging headers, deployment target), the Podfile, Info.plist keys, the
entitlements and the web script tags, and a re-export writes the same bytes. No device needed."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

import pbxproj
from harness import Editor

EDITOR = {"create": True, "name": "Native Test"}
CI_EXCLUDED = ("long-running (over 2 minutes on a loaded server: a C++ plugin compiled for Play, source exports for each platform); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 1500

CPP_PLUGIN = """#include "JasperEngine.h"

using namespace jasperengine;

JASPERENGINE_NATIVE_PLUGIN(Calc) {
    plugin.method("add", [](const NativeJson& args) -> NativeJson {
        if (args.size() != 2) throw NativeError("bad_args", "add takes two numbers");
        return args[0].get<double>() + args[1].get<double>();
    });
    plugin.method("echo", [](const NativeJson& args) -> NativeJson { return args; });
    plugin.asyncMethod("later", [](NativeCall call) { call.resolve({{"n", call.args().size()}}); });
}
"""

MOCK = """local Device = {}
function Device.getCode() return 42 end
function Device.fail() return nil, "no device", "not_ready" end
function Device.login(user, call) pendingLogin = call; return Native.pending() end
return Device
"""

PROBE = """---@class NativeProbe : ScriptInstance
local NativeProbe = { properties = {} }

function NativeProbe:init()
    nativeResults = {}
    nativeResults.add = Native.call("Calc", "add", 2, 3.5)
    local v, err, code = Native.call("Calc", "add", 1)
    nativeResults.addError = code .. ":" .. err
    nativeResults.code = Native.call("Device", "getCode")
    local _, _, failCode = Native.call("Device", "fail")
    nativeResults.failCode = failCode
    local _, _, missing = Native.call("Nobody", "x")
    nativeResults.missing = missing
    Native.callAsync("Calc", "later", {1, 2, 3}, function(result) nativeResults.later = result.n end, {owner = self})
    Native.callAsync("Device", "login", {"ann"}, function(result) nativeResults.login = result.token end, {owner = self})
    Native.on("Calc", "ping", function(payload) nativeResults.ping = payload.n end, self)
    Native.emitMock("Calc", "ping", {n = 9})
end

return NativeProbe
"""


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_native_plugins_create_and_list(ed: Editor):
    ed.require_tools("native_plugins")
    import shutil
    shutil.rmtree(ed.project / "plugins", ignore_errors=True)
    empty = await ed.call("native_plugins", action="list")
    ed.check_eq((empty["exists"], empty["plugins"], empty["mocks"]), (False, [], []))

    made = await ed.call("native_plugins", action="create", name="Device", platforms=["android", "ios", "macos", "web", "cpp"],
                         package="com.test.plugins", methods=[{"name": "getCode"}, {"name": "login", "async": True}])
    paths = [f["path"] for f in made["files"]]
    ed.check_eq(paths, ["plugins/native/android/java/com/test/plugins/Device.java", "plugins/native/apple/Device.h",
                        "plugins/native/apple/Device.m", "plugins/native/web/device.js", "plugins/native/cpp/DevicePlugin.cpp",
                        "native_mocks/Device.lua"])   # the Lua folder of a new project is its root
    for path in paths:
        ed.check((ed.project / path).is_file(), f"{path} written")
    ed.check_in("public static void login(final NativeCall call)", (ed.project / paths[0]).read_text())
    ed.check_in("+ (void)login:(NSArray*)args call:(JasperNativeCall*)call", (ed.project / paths[2]).read_text())
    ed.check_in('JasperNative.register("Device"', (ed.project / paths[3]).read_text())
    ed.check_in("#if !(defined(__ANDROID__)", (ed.project / paths[4]).read_text())

    # refuses to overwrite; overwrite replaces
    await ed.check_error("native_plugins", action="create", name="Device", platforms=["web"], contains="already exists")
    again = await ed.call("native_plugins", action="create", name="Device", platforms=["web"], overwrite=True,
                          methods=[{"name": "getCode"}])
    ed.check_eq([f["overwritten"] for f in again["files"]], [True, True])
    await ed.check_error("native_plugins", action="create", name="1bad", platforms=["web"], contains="identifier")
    await ed.check_error("native_plugins", action="create", name="X", platforms=["windows"], contains="platform")

    # Kotlin + Swift
    await ed.call("native_plugins", action="create", name="Store", platforms=["android", "ios"], android_language="kotlin",
                  apple_language="swift", methods=[{"name": "buy", "async": True}])
    listed = await ed.call("native_plugins", action="list")
    ed.check_eq(listed["exists"], True)
    by_name = {p["name"]: p for p in listed["plugins"]}
    ed.check_eq(sorted(by_name), ["Device", "Store"])
    ed.check_eq(by_name["Device"]["methods"], ["getCode", "login"])
    ed.check_eq(sorted(by_name["Device"]["platforms"]), ["android", "apple", "cpp", "web"])
    ed.check_eq(by_name["Store"]["methods"], ["buy"])
    ed.check_eq(by_name["Store"]["platforms"]["apple"], ["apple/Store.swift"])
    ed.check_eq(listed["mocks"], ["Device", "Store"])

    # tidy up for the next tests (their own plugins)
    import shutil
    shutil.rmtree(ed.project / "plugins")
    for mock in ("Device", "Store"):
        (ed.project / "native_mocks" / f"{mock}.lua").unlink()


async def test_native_settings_keys(ed: Editor):
    values = {
        "android.gradle_dependencies": ["com.squareup.okio:okio:3.9.0", "implementation platform('com.google.firebase:firebase-bom:33.0.0')"],
        "android.gradle_repositories": ["https://maven.example.com/releases"],
        "android.gradle_properties": ["android.enableJetifier=true"],
        "android.extra_permissions": ["com.google.android.gms.permission.AD_ID"],
        "android.manifest_xml": '<uses-permission android:name="android.permission.READ_PHONE_STATE" tools:node="remove" />',
        "android.manifest_application_xml": '<meta-data android:name="com.example.APP_ID" android:value="abc" />',
        "ios.plist_entries": {"NSUserTrackingUsageDescription": "Ads", "ITSAppUsesNonExemptEncryption": False, "Count": 3,
                              "Ratio": 1.5, "UIBackgroundModes": ["fetch"], "Numeric": "15", "SKAdNetworkItems": ["replaced"]},
        "ios.plist_xml": "<key>SKAdNetworkItems</key><array><dict><key>SKAdNetworkIdentifier</key><string>x.skadnetwork</string></dict></array>",
        "ios.frameworks": ["StoreKit", "AppTrackingTransparency?", "AudioToolbox?"],
        "ios.cocoapods": [{"name": "Alamofire", "version": "~> 5.9"}, {"name": "Custom", "source": "https://github.com/x/specs.git"}],
        "ios.swift_packages": [{"url": "https://github.com/RevenueCat/purchases-ios", "version": "4", "products": ["RevenueCat"]}],
        "ios.entitlements_xml": "<key>aps-environment</key><string>development</string>",
        "ios.deployment_target": "16.0",
        "macos.plist_entries": {"LSApplicationCategoryType": "public.app-category.games"},
        "macos.frameworks": ["GameKit"],
        "web.script_urls": ["https://sdk.example.com/sdk.js"],
    }
    await ed.call("set_project_settings", values=values)
    got = (await ed.call("get_project_settings", keys=list(values)))["settings"]
    ed.check_eq(got, values)
    await ed.check_error("set_project_settings", values={"android.manifest_xml": "<a><b></a>"}, contains="well-formed")
    await ed.check_error("set_project_settings", values={"ios.deployment_target": "12.0"}, contains="15.0")
    await ed.check_error("set_project_settings", values={"ios.frameworks": ["Store Kit"]}, contains="framework")
    await ed.check_error("set_project_settings", values={"ios.swift_packages": [{"url": "x", "version": "1"}]}, contains="URL")
    await ed.check_error("set_project_settings", values={"android.gradle_repositories": ["maven"]}, contains="URL")
    described = (await ed.call("get_project_settings", keys=["ios.cocoapods"], describe=True))
    ed.check_in("Podfile", json.dumps(described))
    await ed.call("save_project")
    project_yaml = (ed.project / "project.yaml").read_text()
    # a string that reads like a number is written {string: ...}
    for needle in ("gradleDependencies:", "plistEntries:", "cocoapods:", "swiftPackages:", "scriptUrls:", "deploymentTarget:",
                   "Numeric:\n      string: "):
        ed.check_in(needle, project_yaml)
    listed = await ed.call("native_plugins", action="list")
    ed.check_eq(listed["settings"]["web.script_urls"], ["https://sdk.example.com/sdk.js"])


async def test_play_cpp_plugin_and_mock(ed: Editor):
    ed.require_tools("play", "wait", "eval_lua", "create_script")
    import shutil
    shutil.rmtree(ed.project / "plugins", ignore_errors=True)
    (ed.project / "plugins" / "native" / "cpp").mkdir(parents=True, exist_ok=True)
    (ed.project / "plugins" / "native" / "cpp" / "CalcPlugin.cpp").write_text(CPP_PLUGIN)
    (ed.project / "native_mocks").mkdir(parents=True, exist_ok=True)
    (ed.project / "native_mocks" / "Device.lua").write_text(MOCK)
    made = await ed.call("create_scene", name="NativeMain", type="2D", path="scenes/NativeMain")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Probe")
    await ed.call("create_script", kind="lua", name="NativeProbe", content=PROBE, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    for run in range(2):
        await ed.call("play", action="start", scene=scene, timeout_seconds=900)
        try:
            await ed.call("wait", **{"until": "nativeResults ~= nil and nativeResults.later ~= nil and nativeResults.ping ~= nil",
                                     "timeout_seconds": 30})
            results = json.loads((await lua(ed, "return Json.encode(nativeResults)"))[0])
            ed.check_eq(results["add"], 5.5, f"run {run}: the C++ plugin")
            ed.check_eq(results["addError"], "bad_args:add takes two numbers")
            ed.check_eq(results["code"], 42, "the Lua mock")
            ed.check_eq(results["failCode"], "not_ready")
            ed.check_eq(results["missing"], "not_found")
            ed.check_eq(results["later"], 3)
            ed.check_eq(results["ping"], 9)
            ed.check("login" not in results, "pending until resolved")
            await lua(ed, "pendingLogin:resolve({token = 'abc'})")
            await ed.call("wait", **{"until": "nativeResults.login ~= nil", "timeout_seconds": 10})
            ed.check_eq((await lua(ed, "return nativeResults.login"))[0], "abc")
            ed.check_eq(await lua(ed, "return Native.has('Calc', 'add'), Native.has('Calc', 'nope'), Native.has('Device'), Native.has('Nobody')"),
                        [True, False, True, False])
            ed.check_eq(await lua(ed, "local v = Native.call('Calc', 'echo', 1, 'é', {a = Json.null}); return v[1], v[2], v[3].a == Json.null"),
                        [1, "é", True])
        finally:
            await ed.call("play", action="stop")
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"no errors: {errors}")


# the system frameworks every Apple export links explicitly (Exporter appleEngineFrameworks*): an
# iOS export without AdMob once linked AVFoundation only through the AdMob SDK's module. At least
# the template's APP_FRAMEWORKS (tools/xcode_sync_engine.py on main), each linked once.
APP_FRAMEWORKS = ["AVFoundation", "AudioToolbox", "CoreMedia", "CoreVideo"]
IOS_ENGINE_FRAMEWORKS = APP_FRAMEWORKS + ["CoreGraphics", "Foundation", "GameController", "Metal", "MetalKit", "QuartzCore", "UIKit"]
MACOS_ENGINE_FRAMEWORKS = APP_FRAMEWORKS + ["AppKit", "CoreAudio", "CoreGraphics", "Foundation", "GameController", "Metal", "MetalKit",
                                            "QuartzCore"]


def linked_frameworks(root: dict, target_name: str) -> dict[str, list[bool]]:
    """System framework name -> one weak flag per Frameworks-phase entry of the app target."""
    objects = root["objects"]
    target = pbxproj.target(root, target_name)
    result: dict[str, list[bool]] = {}
    for phase_id in target["buildPhases"]:
        if objects[phase_id]["isa"] != "PBXFrameworksBuildPhase":
            continue
        for build_id in objects[phase_id]["files"]:
            build = objects[build_id]
            ref = objects.get(build.get("fileRef", ""), {})
            name = (ref.get("name") or ref.get("path") or "").rsplit("/", 1)[-1]
            if name.endswith(".framework") and (ref.get("sourceTree") == "SDKROOT" or "System/Library" in ref.get("path", "")):
                result.setdefault(name[:-len(".framework")], []).append("Weak" in json.dumps(build.get("settings", {})))
    return result


def check_engine_frameworks(ed: Editor, root: dict, what: str) -> None:
    for target_name, wanted in (("JasperEngine iOS", IOS_ENGINE_FRAMEWORKS), ("JasperEngine macOS", MACOS_ENGINE_FRAMEWORKS)):
        linked = linked_frameworks(root, target_name)
        for framework in wanted:
            ed.check_eq(linked.get(framework), [False], f"{what}: {target_name} links {framework} once, strongly")
        ed.check_eq({k: v for k, v in linked.items() if len(v) != 1}, {}, f"{what}: {target_name} links each framework once")


def write_stub_xcframework(root: Path, name: str, static: bool) -> None:
    """A minimal .xcframework: Info.plist with one library and its binary (an archive or not)."""
    bundle = root / f"{name}.xcframework"
    lib = bundle / "ios-arm64" / f"{name}.framework"
    lib.mkdir(parents=True, exist_ok=True)
    (lib / name).write_bytes(b"!<arch>\n" + b"\0" * 64 if static else b"\xcf\xfa\xed\xfe" + b"\0" * 64)
    (bundle / "Info.plist").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">\n<dict>\n\t<key>AvailableLibraries</key>\n\t<array>\n\t\t<dict>\n'
        f'\t\t\t<key>LibraryIdentifier</key>\n\t\t\t<string>ios-arm64</string>\n\t\t\t<key>LibraryPath</key>\n\t\t\t<string>{name}.framework</string>\n'
        '\t\t</dict>\n\t</array>\n</dict>\n</plist>\n')


def digest(paths: list[Path]) -> dict[str, str]:
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}


async def test_source_export(ed: Editor):
    native = ed.project / "plugins" / "native"
    import shutil
    for folder in ("android", "apple", "ios", "macos", "web"):
        shutil.rmtree(native / folder, ignore_errors=True)
    (native / "android" / "java" / "com" / "t").mkdir(parents=True, exist_ok=True)
    (native / "android" / "java" / "com" / "t" / "Device.java").write_text(
        "package com.t;\n\npublic class Device {\n    public static int getCode() { return 7; }\n}\n")
    (native / "android" / "kotlin" / "com" / "t" / "k").mkdir(parents=True, exist_ok=True)
    (native / "android" / "kotlin" / "com" / "t" / "k" / "Store.kt").write_text(
        "package com.t.k\n\nobject Store {\n    @JvmStatic fun price(): Double = 0.99\n}\n")
    (native / "android" / "libs").mkdir(parents=True, exist_ok=True)
    (native / "android" / "libs" / "x.jar").write_bytes(b"PK\x05\x06" + b"\0" * 18)
    (native / "android" / "AndroidManifest.xml").write_text(
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android" xmlns:tools="http://schemas.android.com/tools">\n'
        '    <uses-permission android:name="android.permission.READ_PHONE_STATE" tools:node="remove" />\n</manifest>\n')
    (native / "android" / "proguard-rules.pro").write_text("-keep class com.sdk.** { *; }\n")
    (native / "apple").mkdir(parents=True, exist_ok=True)
    (native / "apple" / "Device.h").write_text("#import <Foundation/Foundation.h>\n@interface Device : NSObject <JasperNativePlugin>\n@end\n")
    (native / "apple" / "Device.m").write_text('#import "Device.h"\n@implementation Device\n+ (id)getCode:(NSArray*)args { return @7; }\n@end\n')
    (native / "ios").mkdir(parents=True, exist_ok=True)
    (native / "ios" / "Store.swift").write_text("import Foundation\n@objc(Store) public class Store: NSObject {\n"
                                                "    @objc public static func price(_ args: [Any]) -> Any? { return 0.99 }\n}\n")
    write_stub_xcframework(native / "ios" / "sdk", "StaticSdk", static=True)
    write_stub_xcframework(native / "ios" / "sdk", "DynamicSdk", static=False)
    (native / "macos").mkdir(parents=True, exist_ok=True)
    (native / "macos" / "MacOnly.m").write_text("#import <Foundation/Foundation.h>\n@interface MacOnly : NSObject\n@end\n@implementation MacOnly\n@end\n")
    # a framework with its version links (kept as links), a link out of the project (left out), a
    # C++ file of a platform folder (Xcode/Gradle's, never the CMake build's)
    linked = native / "ios" / "sdk" / "Linked.framework"
    (linked / "Versions" / "A").mkdir(parents=True, exist_ok=True)
    (linked / "Versions" / "A" / "Linked").write_bytes(b"\xcf\xfa\xed\xfe" + b"\0" * 64)
    os.symlink("A", linked / "Versions" / "Current")
    os.symlink("Versions/Current/Linked", linked / "Linked")
    os.symlink("/etc/hostname", native / "ios" / "outside.txt")
    (native / "ios" / "helper.cpp").write_text("int nativeHelper() { return 1; }\n")
    (native / "web").mkdir(parents=True, exist_ok=True)
    (native / "web" / "b.js").write_text('JasperNative.register("B", {f: function () { return 1; }});\n')
    (native / "web" / "a.js").write_text('JasperNative.register("A", {f: function () { return 1; }});\n')

    out = ed.scratch.root / "export-native"

    async def export() -> str:
        r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, timeout_seconds=900)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        return Path(r["result"]["log_file"]).read_text()

    log = await export()
    studio = out / "workspaces" / "androidstudio"
    settings = (studio / "settings.gradle").read_text()
    ed.check_in("include ':jasperbridge'", settings)
    ed.check_in("include ':nativeplugins'", settings)
    module = (studio / "nativeplugins" / "build.gradle").read_text()
    ed.check_in("namespace 'com.jaspergameengine.nativeplugins'", module)
    ed.check_in("api project(':jasperbridge')", module)
    ed.check_in('api "com.squareup.okio:okio:3.9.0"', module)
    ed.check_in("implementation platform('com.google.firebase:firebase-bom:33.0.0')", module)
    ed.check_in("manifest.srcFile pluginsRootAbs+'/AndroidManifest.xml'", module)
    ed.check_in("compileOnly fileTree(dir: pluginsRootAbs+'/libs'", module)
    registry = (studio / "nativeplugins" / "generated" / "com" / "jaspergameengine" / "nativeplugins" / "JasperNativeRegistry.java").read_text()
    ed.check_in('classes.put("Device", "com.t.Device");', registry)
    ed.check_in('classes.put("Store", "com.t.k.Store");', registry)
    rules = (studio / "nativeplugins" / "generated" / "consumer-rules.pro").read_text()
    for rule in ("-keep class com.t.** { *; }", "-keep class com.t.k.** { *; }", "-keep class com.jaspergameengine.** { *; }",
                 "-keep class com.sdk.** { *; }"):
        ed.check_in(rule, rules)
    app_gradle = (studio / "app" / "build.gradle").read_text()
    ed.check_in("implementation project(':jasperbridge')", app_gradle)
    ed.check_in("implementation project(':nativeplugins')", app_gradle)
    ed.check_in("plugins/native/android/libs", app_gradle)
    ed.check_in('maven { url "https://maven.example.com/releases" }', (studio / "build.gradle").read_text())
    ed.check_in("android.enableJetifier=true", (studio / "gradle.properties").read_text())
    manifest = (studio / "app" / "src" / "main" / "AndroidManifest.xml").read_text()
    ed.check_in('xmlns:tools="http://schemas.android.com/tools"', manifest)
    ed.check_in('<uses-permission android:name="com.google.android.gms.permission.AD_ID" />', manifest)
    ed.check_in('tools:node="remove"', manifest)
    ed.check_in('<meta-data android:name="com.example.APP_ID" android:value="abc" />', manifest)
    ed.check(manifest.index("tools:node") < manifest.index("<application"), "manifest_xml before <application>")
    ed.check(manifest.index("com.example.APP_ID") > manifest.index("<application"), "application XML inside it")
    ed.check((out / "project" / "plugins" / "native" / "android" / "java" / "com" / "t" / "Device.java").is_file(), "plugin sources copied")
    ed.check(not (out / "project" / "assets" / "plugins").exists(), "plugins/native is not an asset")
    ed.check((out / "project" / "scripts" / "plugins" / "native" / "cpp" / "CalcPlugin.cpp").is_file(), "the C++ plugins ship as a script root")
    main_cpp = (out / "project" / "main.cpp").read_text()
    ed.check_in('{"Device", "Java/Kotlin plugins run only on Android; Objective-C/Swift plugins run only on iOS and macOS"}', main_cpp)
    ed.check("{\"Calc\"" not in main_cpp, "a C++ plugin runs everywhere")

    # Xcode
    xcode = out / "workspaces" / "xcode"
    root = pbxproj.parse((xcode / "JasperEngine.xcodeproj" / "project.pbxproj").read_text(encoding="utf-8"))
    ed.check_eq(pbxproj.dangling_references(root), [], "no dangling references")
    ios_sources = pbxproj.target_sources(root, "JasperEngine iOS")
    mac_sources = pbxproj.target_sources(root, "JasperEngine macOS")
    ed.check_in("Device.m", ios_sources)
    ed.check_in("Store.swift", ios_sources)
    ed.check("MacOnly.m" not in ios_sources, "macos/ stays out of iOS")
    ed.check_in("Device.m", mac_sources)
    ed.check_in("MacOnly.m", mac_sources)
    ed.check("Store.swift" not in mac_sources and "Device.h" not in ios_sources, "ios/ out of macOS, headers out of Sources")
    for name in ("jasperengine-ios", "jasperengine-macos"):
        compiled = pbxproj.target_sources(root, name)
        for source in ("NativePlatformApple.mm", "JasperNative.m", "NativeBridge.cpp", "NativeClassesLua.cpp"):
            ed.check_eq(compiled.count(source), 1, f"{name} compiles {source}")
    objects = root["objects"]
    ios_target = pbxproj.target(root, "JasperEngine iOS")
    phases = {objects[p]["isa"]: objects[p] for p in ios_target["buildPhases"]}
    linked = []
    for build_id in phases["PBXFrameworksBuildPhase"]["files"]:
        build = objects[build_id]
        linked.append(objects[build["fileRef"]].get("path") if "fileRef" in build else "product:" + objects[build["productRef"]]["productName"])
        if "fileRef" in build and objects[build["fileRef"]].get("path", "").endswith("AppTrackingTransparency.framework"):
            ed.check_in("Weak", json.dumps(build.get("settings", {})), "the weak system framework")
    for item in ("StaticSdk.xcframework", "DynamicSdk.xcframework", "System/Library/Frameworks/StoreKit.framework",
                 "System/Library/Frameworks/AppTrackingTransparency.framework", "product:RevenueCat"):
        ed.check_in(item, linked)
    # the engine's own frameworks, explicit (not autolinked), each once: a project's "AudioToolbox?"
    # joins the engine's strong one
    check_engine_frameworks(ed, root, "with plugins")
    ed.check_eq(linked_frameworks(root, "JasperEngine macOS").get("GameKit"), [False], "macos.frameworks")
    embed = [objects[p] for p in ios_target["buildPhases"] if objects[p]["isa"] == "PBXCopyFilesBuildPhase"]
    ed.check_eq(len(embed), 1, "one Embed Frameworks phase")
    embedded = [objects[objects[b]["fileRef"]]["path"] for b in embed[0]["files"]]
    ed.check_eq(sorted(embedded), ["DynamicSdk.xcframework", "Linked.framework"], "only the dynamic SDKs are embedded")
    ed.check_eq(embed[0]["dstSubfolderSpec"], "10")
    packages = [o for o in objects.values() if o.get("isa") == "XCRemoteSwiftPackageReference"]
    ed.check_eq([(p["repositoryURL"], p["requirement"]["kind"], p["requirement"]["minimumVersion"]) for p in packages],
                [("https://github.com/RevenueCat/purchases-ios", "upToNextMajorVersion", "4.0.0")])  # "4" padded
    header_refs = [o.get("path") for o in objects.values() if o.get("isa") == "PBXFileReference" and o.get("path") == "Device.h"]
    ed.check_eq(header_refs, [], "plugin headers are no file references (they would shadow engine headers)")
    exported_link = out / "project" / "plugins" / "native" / "ios" / "sdk" / "Linked.framework"
    ed.check((exported_link / "Versions" / "Current").is_symlink(), "a framework's Versions/Current stays a link")
    ed.check(not (out / "project" / "plugins" / "native" / "ios" / "outside.txt").exists(), "a link out of the project is not exported")
    ed.check_in("Linked.framework", embedded, "the dynamic framework behind its links is embedded")
    ed.check_eq(len(objects[root["rootObject"]]["packageReferences"]), 1)
    ed.check_eq(len(ios_target["packageProductDependencies"]), 1)
    for settings in pbxproj.target_settings(root, "JasperEngine iOS"):
        ed.check_eq(settings["SWIFT_VERSION"], "5.0")
        ed.check_in("JasperNative-Bridging-Header-iOS.h", settings["SWIFT_OBJC_BRIDGING_HEADER"])
        ed.check_eq(settings["IPHONEOS_DEPLOYMENT_TARGET"], "16.0")
        ed.check_eq(settings["CODE_SIGN_ENTITLEMENTS"], "ios/JasperEngine.entitlements")
        ed.check_in("$(SRCROOT)/../../project/plugins/native/ios/sdk", settings["FRAMEWORK_SEARCH_PATHS"])
        ed.check_in("$(SRCROOT)/../../platform/apple", settings["HEADER_SEARCH_PATHS"])
        ed.check_in("$(SRCROOT)/../../project/plugins/native/apple", settings["HEADER_SEARCH_PATHS"], "plugin headers are a search path")
        ed.check_in("@executable_path/Frameworks", settings["LD_RUNPATH_SEARCH_PATHS"])
    for settings in pbxproj.target_settings(root, "JasperEngine macOS"):
        ed.check(settings.get("SWIFT_OBJC_BRIDGING_HEADER") is None, "macOS has no Swift plugin")
        ed.check(settings.get("CODE_SIGN_ENTITLEMENTS", "") != "ios/JasperEngine.entitlements", "the iOS entitlements stay on iOS")
    bridging = (out / "project" / "plugins" / "native" / "JasperNative-Bridging-Header-iOS.h").read_text()
    ed.check_in('#import "JasperNative.h"', bridging)
    ed.check_in('#import "apple/Device.h"', bridging)
    ed.check(not (out / "project" / "plugins" / "native" / "JasperNative-Bridging-Header-macOS.h").exists(), "no Swift on macOS")
    podfile = (xcode / "Podfile").read_text()
    ed.check_in("platform :ios, '16.0'", podfile)
    ed.check_in("target 'JasperEngine iOS' do", podfile)
    ed.check_in("use_frameworks! :linkage => :static", podfile)
    ed.check_in("pod 'Alamofire', '~> 5.9'", podfile)
    ed.check_in("source 'https://github.com/x/specs.git'", podfile)
    ed.check_in("source 'https://cdn.cocoapods.org/'", podfile)
    ed.check((xcode / "README-pods.txt").is_file(), "README-pods.txt")
    ed.check_in("pod install", log)
    plist = (xcode / "ios" / "Info.plist").read_text()
    ed.check_in("<key>NSUserTrackingUsageDescription</key>\n\t<string>Ads</string>", plist)
    ed.check_in("<key>ITSAppUsesNonExemptEncryption</key>\n\t<false/>", plist)
    ed.check_in("<key>Count</key>\n\t<integer>3</integer>", plist)
    ed.check_in("<key>Ratio</key>\n\t<real>1.5</real>", plist)
    ed.check_in("<key>UIBackgroundModes</key>\n\t<array>\n\t\t<string>fetch</string>\n\t</array>", plist)
    ed.check_in("<key>Numeric</key>\n\t<string>15</string>", plist)
    ed.check_eq(plist.count("<key>SKAdNetworkItems</key>"), 1, "ios.plist_xml's keys replace the others")
    ed.check("replaced" not in plist, "the raw XML wins over a plist entry of the same key")
    ed.check_in("<key>LSApplicationCategoryType</key>", (xcode / "macos" / "Info.plist").read_text())
    ed.check_in("<key>aps-environment</key>", (xcode / "ios" / "JasperEngine.entitlements").read_text())

    # web
    apply_cmake = (out / "project" / "export-settings" / "apply.cmake").read_text()
    ed.check_in(r'set(extra "${native_stub}\n${head}\n")', apply_cmake, "the JasperNative stub comes first, before web.head_include")
    stub = apply_cmake.index("window.JasperNative = window.JasperNative ||")
    url = apply_cmake.index('<script src="https://sdk.example.com/sdk.js"></script>')
    a = apply_cmake.index("plugins/a.js")
    b = apply_cmake.index("plugins/b.js")
    ed.check(url < a < b, "SDK URLs, then plugin files in name order")
    ed.check(stub > 0, "the stub is defined")
    ed.check_in('file(COPY "${CMAKE_CURRENT_LIST_DIR}/../plugins/native/web/${native_file}"', apply_cmake)
    ed.check((out / "project" / "plugins" / "native" / "web" / "a.js").is_file(), "web plugins copied")
    cmake_text = (out / "CMakeLists.txt").read_text()
    ed.check_in("JASPERENGINE_NATIVE_PLUGIN_SOURCES", cmake_text, "macOS CMake builds compile the ObjC plugins")
    ed.check_in('string(FIND "${native_source}" "${PROJECT_ROOT}/plugins/native/" native_at)', cmake_text,
                "the platform folders' C++ (ios/helper.cpp) stays out of the CMake glob")

    # JASPER_NATIVE_EXPORT_KEEP=<dir>: a copy of this export (to build it with Gradle / Xcode by hand)
    keep = os.environ.get("JASPER_NATIVE_EXPORT_KEEP")
    if keep:
        import shutil
        shutil.rmtree(keep, ignore_errors=True)
        shutil.copytree(out, keep, symlinks=True)

    # re-export: the same bytes
    watched = [studio / "settings.gradle", studio / "build.gradle", studio / "gradle.properties", studio / "app" / "build.gradle",
               studio / "nativeplugins" / "build.gradle", studio / "app" / "src" / "main" / "AndroidManifest.xml",
               xcode / "JasperEngine.xcodeproj" / "project.pbxproj", xcode / "Podfile", xcode / "ios" / "Info.plist",
               xcode / "ios" / "JasperEngine.entitlements", out / "project" / "export-settings" / "apply.cmake",
               out / "project" / "main.cpp"]
    before = digest(watched)
    await export()
    ed.check_eq(digest(watched), before, "a re-export writes the same files")

    # without the settings and the Apple/Android folders: nothing of them lingers
    import shutil
    for folder in ("android", "apple", "ios", "macos"):
        shutil.rmtree(native / folder)
    await ed.call("set_project_settings", values={"android.gradle_dependencies": [], "ios.cocoapods": [], "ios.entitlements_xml": "",
                                                  "ios.swift_packages": [], "ios.frameworks": [], "ios.deployment_target": "15.0"})
    await export()
    ed.check("include ':nativeplugins'" not in (studio / "settings.gradle").read_text(), "no nativeplugins module")
    ed.check(not (studio / "nativeplugins").exists(), "the module folder is gone")
    ed.check(not (xcode / "Podfile").exists(), "no Podfile")
    ed.check(not (xcode / "ios" / "JasperEngine.entitlements").exists(), "no entitlements")
    ed.check(not (out / "project" / "plugins" / "native" / "android").exists(), "the copied plugins go too")
    root = pbxproj.parse((xcode / "JasperEngine.xcodeproj" / "project.pbxproj").read_text(encoding="utf-8"))
    ed.check("Device.m" not in pbxproj.target_sources(root, "JasperEngine iOS"), "no plugin source left")
    # a project without plugins or frameworks links the engine's frameworks too
    check_engine_frameworks(ed, root, "without plugins")
    ed.check("StoreKit" not in linked_frameworks(root, "JasperEngine iOS"), "ios.frameworks emptied")
    # without plugins the app targets still find JasperNative.h (their own sources import it), and
    # CocoaPods' xcconfig values are inherited
    for name in ("JasperEngine iOS", "JasperEngine macOS"):
        for settings in pbxproj.target_settings(root, name):
            ed.check_in("$(SRCROOT)/../../platform/apple", settings["HEADER_SEARCH_PATHS"], f"{name}: JasperNative.h")
            ed.check_eq(settings["HEADER_SEARCH_PATHS"][0], "$(inherited)", f"{name}: $(inherited) first")
            if "OTHER_LDFLAGS" in settings:
                ed.check_eq(settings["OTHER_LDFLAGS"][0], "$(inherited)", f"{name}: Pods' linker flags kept")
