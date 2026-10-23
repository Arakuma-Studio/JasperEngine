# Writes tools/fixtures/legacy_paths_project: a project saved before asset GUIDs (S04 AC1/AC14)
import base64, os, struct, sys, zlib, json
root = sys.argv[1]
def w(rel, data, binary=False):
    p = os.path.join(root, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f: f.write(data if binary else data.encode())
def png(r, g, b):
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes([r, g, b]) * 2 for _ in range(2))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
w('assets/textures/brick.png', png(180, 70, 50), True)
w('assets/textures/mask.png', png(255, 255, 255), True)
w('assets/textures/rock.png', png(120, 120, 120), True)
# one triangle, two named meshes and an unnamed one (sub-assets)
positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
gltf = {
    "asset": {"version": "2.0"},
    "buffers": [{"byteLength": len(positions), "uri": "data:application/octet-stream;base64," + base64.b64encode(positions).decode()}],
    "bufferViews": [{"buffer": 0, "byteLength": len(positions)}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}],
    "meshes": [{"name": "Hull", "primitives": [{"attributes": {"POSITION": 0}}]},
               {"name": "Mast", "primitives": [{"attributes": {"POSITION": 0}}]},
               {"primitives": [{"attributes": {"POSITION": 0}}]}],
    "materials": [{"name": "Paint"}],
    "nodes": [{"mesh": 0, "name": "Hull"}, {"mesh": 1, "name": "Mast"}, {"mesh": 2}],
    "scenes": [{"nodes": [0, 1, 2]}], "scene": 0,
}
w('assets/models/ship.gltf', json.dumps(gltf, indent=2) + "\n")
samples = b'\x00\x00' * 64
wav = b'RIFF' + struct.pack('<I', 36 + len(samples)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 8000, 16000, 2, 16) + b'data' + struct.pack('<I', len(samples)) + samples
w('assets/sounds/beep.wav', wav, True)
w('assets/ui/hud.html', '<!DOCTYPE html>\n<html>\n<head>\n<link rel="stylesheet" href="hud.css">\n</head>\n<body>\n<img src="../textures/brick.png">\n</body>\n</html>\n')
w('assets/ui/hud.css', 'body { background-image: url("../textures/rock.png"); }\n')
w('scripts/Spinner.lua', '''---@class Spinner : ScriptInstance
local Helpers = require("util.helpers")
local Spinner = { properties = {} }

-- S04: the Asset module answers in the editor's Play and in exported games (assetmap.bin).
-- 4f17... is the GUID textures/brick.png got when the project was upgraded (its legacy GUID).
function Spinner:init()
    local guid = "4f172276042c39a8da88bc7b7635e7e1"
    local path = Asset.path(guid)
    if path ~= "textures/brick.png" then
        print("[ERROR] [S04] Asset.path(" .. guid .. ") = " .. tostring(path))
    elseif Asset.guid("textures/brick.png") ~= guid then
        print("[ERROR] [S04] Asset.guid = " .. tostring(Asset.guid("textures/brick.png")))
    elseif not Asset.exists(guid) or Asset.ref(guid).type ~= "texture" then
        print("[ERROR] [S04] Asset.exists / Asset.ref")
    else
        print("[S04] Asset.path " .. guid .. " = " .. path)
    end
end

return Spinner
''')
w('scripts/util/helpers.lua', 'return {}\n')
w('shaders/tint.vert', '#version 450\nvoid main() {}\n')
w('shaders/tint.frag', '#version 450\n#include "common.glsl"\nvoid main() {}\n')
w('shaders/common.glsl', '// shared\n')
transform = '''      transform:
        position: [0, 0, 0]
        rotation: [1, 0, 0, 0]
        scale: [1, 1, 1]
'''
def tex(path): return '{textureType: 2D, source: path, path: %s, minFilter: linear, magFilter: linear, wrapU: repeat, wrapV: repeat, releaseDataAfterLoad: true}' % path
w('materials/Brick.material', '''name: materials/Brick.material
baseColorFactor: [1, 1, 1, 1]
metallicFactor: 0
roughnessFactor: 1
baseColorTexture: %s
''' % tex('textures/mask.png'))
w('prefabs/Lamp.prefab', '''type: Prefab
version: 3
nextEntity: 1002
root: 1001
members:
  - type: Entity
    entity: 1001
    name: Lamp
    components:
%s      light:
        type: spot
        spotMask: %s
''' % (transform, tex('textures/mask.png')))
def scene(id, name, entities):
    return '''id: %d
name: %s
scene:
  backgroundColor: [0.16, 0.18, 0.24, 1]
sceneType: scene_3d
mainCamera: 0
entities:
%s''' % (id, name, entities)
main = '''  - type: Entity
    entity: 1001
    name: Ship
    components:
%s      model:
        filename: models/ship.gltf
  - type: Entity
    entity: 1002
    name: Speaker
    components:
%s      audiosource:
        filename: sounds/beep.wav
      script:
        scripts:
          - type: lua
            path: Spinner.lua
            className: Spinner
            enabled: true
  - type: Entity
    entity: 1003
    name: Lamp
    components:
      prefab: {name: Lamp, path: prefabs/Lamp.prefab}
%s''' % (transform, transform, transform)
closed = '''  - type: Entity
    entity: 1001
    name: Spot
    components:
%s      light:
        type: spot
        spotMask: %s
  - type: Entity
    entity: 1002
    name: Hud
    components:
      uidocument:
        filename: ui/hud.html
  - type: Entity
    entity: 1003
    name: Lantern
    components:
%s      light:
        type: spot
        spotMask: %s
  - type: Entity
    entity: 1004
    name: Boat
    components:
%s      model:
        filename: models/ship.gltf
''' % (transform, tex('textures/mask.png'), transform, tex('textures/brick.png'), transform)
w('Main.scene', scene(1, 'Main', main))
w('Closed.scene', scene(2, 'Closed', closed))
w('project.yaml', '''name: LegacyPaths
canvasWidth: 1280
canvasHeight: 720
assetsDir: assets
luaDir: scripts
startSceneId: 1
scenes:
  - filepath: Main.scene
  - filepath: Closed.scene
''')
w('README.md', 'Fixture: a project saved before asset GUIDs (no .meta files, no guid: keys, references by path). docs/specs/S04-asset-identity.md AC1, AC14. Generated; tests copy it before opening it.\n')
