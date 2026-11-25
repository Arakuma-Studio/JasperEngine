# JasperEngine

**Motor nativo de C++ para juegos 2D y 3D, con editor visual y scripting en Lua.**

JasperEngine deriva de [Doriax](https://github.com/doriaxengine/doriax) de Eduardo Doria y
colaboradores. Consulta [NOTICE.md](NOTICE.md). La versión en inglés de esta guía está en
[README.md](README.md).

---

## Índice

1. [Qué es](#qué-es)
2. [Plataformas soportadas](#plataformas-soportadas)
3. [Mapa del repositorio](#mapa-del-repositorio)
4. [Requisitos](#requisitos)
5. [Compilación](#compilación)
6. [Ejecutar el editor](#ejecutar-el-editor)
7. [Compilar con GitHub Actions](#compilar-con-github-actions)
8. [Pruebas](#pruebas)
9. [Herramientas de línea de comandos](#herramientas-de-línea-de-comandos)
10. [Cómo está organizado el motor](#cómo-está-organizado-el-motor)
11. [Documentación](#documentación)
12. [Reglas e invariantes del proyecto](#reglas-e-invariantes-del-proyecto)
13. [Licencia](#licencia)

---

## Qué es

JasperEngine es un motor de videojuegos completo con dos mitades:

- **Editor de escritorio** estilo Unity: jerarquía, inspector, ventana de proyecto, consola, vistas
  de Escena y de Juego, menús, atajos y Prefabs. Desde él se crean escenas 2D y 3D, se editan
  componentes, animaciones, huesos, terreno, materiales y shaders, y se juega o se exporta el juego.
- **Runtime compartido** de ECS para 2D y 3D, con escenas, serialización, renderizado, física,
  animación, audio, UI y scripting.

Lo más relevante para empezar:

| Área | Qué ofrece |
| --- | --- |
| Render | PBR con luces dinámicas, sombras en cascada, sky-driven IBL, SSAO, niebla, espejos planos; 40 shaders listos para usar; sprites, tilemaps, partículas, líneas y trails |
| Modelos | glTF, OBJ y FBX (conversión a glTF con ufbx), animación esquelética, morph targets e instancing |
| Animación | Keyframes, tweens, Animation Clips, capas y máscaras, Animator Controllers, retargeting humanoides, animación de pose, Vertex Animation (VAT) |
| Física | Box2D para 2D y Jolt para 3D, con rigidbodies, colliders, joints y callbacks tipo Unity |
| UI | UI nativa con Rect Transform, layout y widgets; Canvas Groups; Canvas Scaler; HTML/CSS UI con `<video>` |
| Audio | Audio Mixer con grupos, efectos, snapshots y streaming; audio espacial 3D |
| Scripting | Lua y C++ con el ciclo de vida de `MonoBehaviour`, corrutinas, timers, `PlayerPrefs`, JSON y saves atómicos |
| Herramientas para agentes | Servidor MCP integrado en el editor (`editor/mcp`, ver `docs/mcp`) |

También incluye generación de mapas de normales para sprites, síntesis de texturas, simulaciones de
fluidos, terreno con LOD por clipmap, navegación, foliage y un overlay de depuración en el juego.

---

## Plataformas soportadas

| Área | Soporte |
| --- | --- |
| Editor | Windows, Linux, macOS |
| Destinos de exportación | Windows, Linux, macOS, Android, iOS, HTML5 |
| Backends de renderizado | OpenGL / OpenGL ES, Vulkan, Metal, Direct3D 11 |
| Lenguajes de scripting | Lua, C++ |

El backend por defecto del editor es OpenGL en Windows y Linux, y Metal en macOS. Vulkan se puede
elegir en Windows o Linux al configurar CMake.

---

## Mapa del repositorio

| Ruta | Qué contiene |
| --- | --- |
| `engine/core/` | Runtime: ECS, escenas, componentes, bindings de Lua, HTML UI, jobs, logging |
| `engine/renders/` | Backends de renderizado sobre sokol-gfx |
| `engine/shaderlib/` | Shaders en GLSL |
| `engine/shaders/` | Cabeceras C++ de stub (no son GLSL) |
| `engine/platform/` | Código específico de cada sistema operativo |
| `engine/player/` | Punto de entrada del runtime precompilado |
| `engine/project/` | Proyecto base sobre el que se compila un juego |
| `engine/libs/`, `libs/` | Librerías de terceros incluidas en el repositorio |
| `editor/command/` | Comandos con undo; toda edición de escena pasa por uno |
| `editor/window/` | Paneles del editor |
| `editor/htmlui/` | Importadores de HTML UI |
| `editor/mcp/` | Servidor MCP y sus herramientas |
| `editor/cli/` | Subcomandos de línea de comandos; comparten el registro de herramientas MCP |
| `engine/tests/`, `editor/tests/` | Tests unitarios |
| `tools/mcp/tests/` | Tests del servidor MCP |
| `tools/` | Scripts de build, test y generadores |
| `tools/fixtures/`, `tools/test-projects/` | Proyectos de prueba versionados |
| `docker/` | Imágenes de compilación |
| `docs/` | Especificaciones, guías y documentación de pruebas |

**Orden de lectura recomendado si vienes de cero:** este README → `docs/testing.md` →
`docs/specs/README.md` → `docs/unity-glossary.md` → el área que vayas a tocar dentro de `docs/`.

---

## Requisitos

### La vía recomendada: Docker

El único requisito del host es **Docker**. Todo lo demás (compilador, CMake, librerías, Mesa,
Xvfb, etc.) vive dentro de las imágenes de `docker/`.

### Compilación nativa

- Compilador C++20: GCC 11, Clang 16, Apple Clang 15 (Xcode 15) o MSVC 19.30 (Visual Studio 2022),
  o versiones más nuevas.
- CMake 3.27 o superior.
- Python 3.

---

## Compilación

### Con Docker (Linux, Web, Android)

`tools/build.sh` compila dentro de las imágenes de `docker/` y escribe en `build/<target>/`:

```bash
tools/build.sh linux        # editor + proyecto de ejemplo del motor
tools/build.sh web          # proyecto de ejemplo para Emscripten
tools/build.sh android      # APK del proyecto de ejemplo
tools/build.sh unit-tests   # tests unitarios del motor y del editor
tools/build.sh shell linux  # shell interactiva dentro de la imagen Linux
```

El editor queda en:

```
build/linux/editor-install/bin/jasperengine-editor
```

Variables útiles:

| Variable | Efecto |
| --- | --- |
| `SKIP_ENGINE=1` | Compila solo el editor (target `linux`) |
| `JOBS=<n>` | Limita el paralelismo de compilación |
| `CPUS=<n>` | Limita las CPUs del contenedor |
| `WERROR=1` | Convierte los warnings de nuestros targets en errores |
| `GRAPHIC_BACKEND=vulkan` | Compila el editor Linux para Vulkan |
| `SANITIZE=address,undefined` | Compila con ASan + UBSan (también `thread`) |
| `BUILD_TYPE=Debug` | Cambia el tipo de build |

### Linux nativo

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential cmake git ninja-build pkg-config python3 \
  libx11-dev libxcursor-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libwayland-dev wayland-protocols \
  libdbus-1-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jasperengine-editor
./build/jasperengine-editor
```

> En Ubuntu 22.04 instala CMake desde `apt.kitware.com` (el del sistema es 3.22 y se necesita 3.27+).
> La imagen `docker/linux.Dockerfile` muestra el procedimiento exacto.

### macOS

```bash
brew install cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jasperengine-editor
open build/JasperEngine.app
```

### Windows

Desde un Developer PowerShell de Visual Studio 2022:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target jasperengine-editor jasperengine-editor-cmd
build\Release\jasperengine-editor.exe
```

`jasperengine-editor-cmd` es la variante de consola para automatización y no se compila por defecto.

### Editor Vulkan (Windows o Linux)

Instala el Vulkan SDK y añade `-DGRAPHIC_BACKEND=vulkan` al configurar, en un directorio aparte.

### Instalación redistribuible

```bash
cmake --install build --prefix dist
```

También existen presets de CMake listos para IDE y builds nativas en `CMakePresets.json` y
`engine/CMakePresets.json` (`linux-release`, `linux-debug`, `linux-vulkan-release`, `macos-release`,
`windows-vs2022`, `linux-asan`, etc.).

---

## Ejecutar el editor

```bash
./build/jasperengine-editor
```

Para crear un proyecto vacío desde la línea de comandos:

```bash
jasperengine-editor mcp --create <directorio-vacio>
```

Recomendaciones al probar cambios del motor:

- Trabaja siempre sobre una **copia de un proyecto**, no sobre un proyecto real.
- Puedes partir de un proyecto de `tools/fixtures/` o de `tools/test-projects/`.
- Arranca el editor desde su propio directorio de trabajo con un `settings.yaml`; `editor.log`
  aparecerá al lado.
- Modo headless para automatización:
  `env -u DISPLAY jasperengine-editor mcp --project <copia> --headless`
- Con `DISPLAY` definido, `--headless` usa ese display.
- Para HTTP añade `--http-port <puerto>` después de comprobar que el puerto está libre.

---

## Compilar con GitHub Actions

El repositorio incluye `.github/workflows/build-engine.yml`, un workflow independiente que compila
el motor y el editor en Linux y publica los binarios como artefactos descargables.

### Cuándo se ejecuta

- **Manual:** pestaña *Actions* → *Build engine* → *Run workflow* (puedes elegir la rama).
- **Automático:** en cada push a `main`, salvo que el cambio solo toque documentación
  (`**/*.md` o `docs/**`).

### Qué hace

1. Hace checkout del repositorio.
2. Libera espacio en disco si el runner no tiene los 30 GB que necesita la compilación.
3. Ejecuta `tools/build.sh linux`, que construye la imagen Docker de `docker/linux.Dockerfile` y
   dentro de ella compila el editor y el proyecto de ejemplo con `WERROR=1`.
4. Empaqueta y publica el artefacto **`jasperengine-linux-x64`**, que contiene:
   - `jasperengine-editor-linux-x64.tar.xz`: el árbol `editor-install` completo, con
     `bin/jasperengine-editor`.
   - `jasperengine-project`: el binario del proyecto de ejemplo, si la compilación lo produjo.

### Cómo descargarlo

Al terminar la ejecución, abre la ejecución en *Actions* y descarga
`jasperengine-linux-x64` desde la sección *Artifacts*. Los artefactos se conservan 30 días.

### Reproducirlo en local

Es exactamente el mismo camino que usa el workflow:

```bash
WERROR=1 tools/build.sh linux
```

Los resultados quedan en `build/linux/`, que está ignorado por git.

---

## Pruebas

Cada cambio debe traer tests, y todas las suites se ejecutan igual en cualquier máquina: dentro de
las imágenes Docker, a través de `tools/build.sh`. La referencia completa está en
[`docs/testing.md`](docs/testing.md).

| Suite | Comando |
| --- | --- |
| Motor + editor (unitarios) | `tools/build.sh unit-tests` |
| HTML/CSS UI | `tools/build.sh htmlui-tests` |
| Importadores de HTML UI | `tools/build.sh htmlui-editor-tests` |
| Servidor MCP | `tools/build.sh linux` y después `tools/build.sh mcp-tests -j 2` |
| Smoke test de extremo a extremo | `SMOKE_METHOD=compile tools/smoke-test.sh <proyecto>` |
| Web en Chrome headless | `tools/build.sh web` y después `tools/build.sh web-gate` |

Filtros y variantes habituales:

```bash
UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=Vector3.*' tools/build.sh unit-tests
SANITIZE=address,undefined tools/build.sh unit-tests
JASPER_KEEP_SCRATCH=1 tools/build.sh unit-tests   # conserva el directorio temporal de un fallo
```

En el día a día, para un cambio concreto, `tools/check-changed` ejecuta solo lo que ese cambio puede
afectar. `--dry-run` muestra el plan.

---

## Herramientas de línea de comandos

```bash
jasperengine-editor export --help              # exportar un proyecto a CMake independiente
jasperengine-editor shaders --help             # generar shaders
jasperengine-editor benchmark --help           # medir FPS de una escena
jasperengine-editor place-physics --help       # colocar objetos con física, sin ventana
jasperengine-editor bake-vat --help            # hornear o importar Vertex Animation (.vat)
jasperengine-editor synthesize-texture --help  # sintetizar texturas desde ejemplos
jasperengine-editor generate-sprite-normals --help  # mapas de normales para sprites
jasperengine-editor check --project . --normals     # detectar mapas de normales obsoletos
jasperengine-editor call <tool>                # ejecutar cualquier herramienta MCP desde la shell
```

---

## Cómo está organizado el motor

- **`engine/core`** es el runtime: ECS (`ecs/`), escenas y componentes (`component/`), entrada,
  render, física, animación, audio, UI HTML, jobs, logging y bindings de scripting.
- **`engine/renders`** implementa los backends sobre sokol-gfx, de modo que el mismo código funciona
  en GL 4.1 core, GLES3/WebGL2, Metal, D3D11 y Vulkan.
- **`engine/shaderlib`** contiene los shaders GLSL; **`engine/shaders`** solo cabeceras de stub.
- **`engine/platform`** aísla el código por sistema operativo y **`engine/player`** es el ejecutable
  que corre los juegos ya cocinados.
- **`engine/project`** es el proyecto base que un juego compilado hereda (más las escenas y assets
  cocinados por el editor).
- **`editor/`** es el editor: ventanas, comandos con undo, importadores, exportación y el servidor
  MCP que permite a agentes de IA inspeccionar y modificar el proyecto.

Un juego es un *player* precompilado más escenas y assets cocinados. Alternativamente, en modo
compilado, es `engine/project` más el C++ que genera el editor (factorías de escena y bindings de
scripts).

---

## Documentación

| Documento | Para qué sirve |
| --- | --- |
| [`docs/testing.md`](docs/testing.md) | Todas las suites, dónde va un test, nombres, sanitizers y qué corre CI |
| [`docs/specs/README.md`](docs/specs/README.md) | Decisiones globales y lista de "no copiar" de otros motores |
| [`docs/mcp/DESIGN.md`](docs/mcp/DESIGN.md) | Cómo funciona el servidor MCP |
| [`docs/mcp/conventions.md`](docs/mcp/conventions.md) | Reglas de las herramientas MCP |
| [`docs/shaders/README.md`](docs/shaders/README.md) | Biblioteca de shaders |
| [`docs/rendering/`](docs/rendering/) | Sombras, iluminación, batching, instancing, reflexiones |
| [`docs/editor/`](docs/editor/) | Ventanas y funciones del editor |
| [`docs/unity-glossary.md`](docs/unity-glossary.md) | Nombres heredados y migración de proyectos antiguos |
| [`docs/animation-clips.md`](docs/animation-clips.md) | Animation Clips, capas, máscaras y retargeting |
| [`docs/pose-animation.md`](docs/pose-animation.md) | Animación de pose para humanoides |
| [`docs/vertex-animation.md`](docs/vertex-animation.md) | Vertex Animation (VAT) |
| [`docs/data-safety.md`](docs/data-safety.md) | Escrituras atómicas y seguridad de datos |
| [`docs/editor/keyboard-shortcuts.md`](docs/editor/keyboard-shortcuts.md) | Atajos de teclado |
| [`AGENTS.md`](AGENTS.md) | Reglas completas para contribuir al motor (en inglés) |

---

## Reglas e invariantes del proyecto

Si vas a modificar el motor, estas reglas no son opcionales:

- Toda función del runtime funciona en los **cinco backends** (GL 4.1 core, GLES3/WebGL2, Metal,
  D3D11, Vulkan) y en los **seis destinos** (Windows, Linux, macOS, Android, iOS, HTML5).
- Ninguna función puede requerir compute shaders, SSBOs, bindless, draws indirectos ni atómicos de
  64 bits. Una ruta rápida de escritorio necesita un fallback equivalente.
- Una función desactivada no cuesta CPU ni GPU. Se prioriza móvil y web.
- Los proyectos antiguos deben seguir cargando. Un campo nuevo opcional lleva valor por defecto y no
  sube la versión del formato; renombrar, mover o retipar una clave almacenada exige subir la
  versión, una migración y un test.
- Toda escritura persistente pasa por `engine/core/io/AtomicFile.h` o un helper construido sobre él.
- Una función visible para el usuario lleva UI de editor con nombres tipo Unity y undo; una función
  del runtime lleva API de Lua; una función de usuario lleva herramientas MCP.
- C++20 dentro del subconjunto portable que valida `tools/check-cxx-subset.py`; el código debe
  compilar sin warnings con `WERROR=1`.
- No edites a mano los archivos generados: usa el generador indicado en `AGENTS.md`.

Flujo habitual antes de dar un cambio por terminado:

```bash
tools/check-changed          # qué se ve afectado por el cambio
tools/build.sh unit-tests    # si toca lógica del motor o del editor
python3 tools/check_raw_writes.py
python3 tools/check_stream_keys.py
```

---

## Licencia

JasperEngine se distribuye bajo [licencia MIT](LICENSE). Las librerías de terceros incluidas
conservan sus propias licencias.
