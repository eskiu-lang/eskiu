"""packages_catalog.py: the package catalog table (used by packages_page.py)."""

# ---------------------------------------------------------------------------
# Package catalog: site/packages.html and site/es/packages.html.
#
# Each package's tagline and code example come from its README at the released
# tag (raw.githubusercontent.com, so the catalog matches what `eskiuc add` gets).
# The table below adds what a README does not carry: the README section the
# example comes from, a short note and the requirements, in English and Spanish,
# and the Spanish tagline.

PKG_ORG = "eskiu-lang"
PKG_REV = "v0.1.0"   # the release a package is shown at, unless its entry has "rev"

PACKAGES_CATALOG = [
    {
        "name": "gpu",
        "rev": "v0.7.0",
        "section": "Compute",
        "tag_es": "Cómputo en GPU, renderizado, modelos 3D y machine learning para Eskiu, sobre WebGPU.",
        "note_en": "One API runs on Metal (macOS, iOS), Vulkan (Linux, Android) and DirectX 12 (Windows) through "
                   "wgpu-native, with shaders in WGSL. It draws glTF and OBJ models with physically "
                   "based shading, runs Llama models from GGUF or safetensors files (Llama 2 7B Q4_0 "
                   "at 45 tokens per second on an M4 Pro; llama.cpp does 58 on the same machine) and "
                   "trains small networks (97% test accuracy on MNIST). It has what a 3D renderer needs: "
                   "several render targets, cube maps, GPU mipmaps, shadow maps, image-based lighting, "
                   "morph targets, KTX2 and HDR textures, and camera and video frames drawn from YUV. "
                   "On iOS and Android it draws into a <code>GpuView</code> of a ui app, and the same "
                   "programs build for the browser through emdawnwebgpu, with a WebGL2 backend where the "
                   "browser has no WebGPU. Desktop windows take keyboard, mouse, touch and gamepads. "
                   "<a href=\"gpu.html\">What it does, with its benchmarks →</a>",
        "note_es": "Una sola API corre sobre Metal (macOS, iOS), Vulkan (Linux, Android) y DirectX 12 (Windows) a "
                   "través de wgpu-native, con shaders en WGSL. Dibuja modelos glTF y OBJ con "
                   "sombreado físico (PBR), corre modelos Llama desde archivos GGUF o safetensors "
                   "(Llama 2 7B Q4_0 a 45 tokens por segundo en un M4 Pro; llama.cpp da 58 en la "
                   "misma máquina) y entrena redes pequeñas (97% de precisión en MNIST). Trae lo que necesita "
                   "un motor 3D: varios render targets, cube maps, mipmaps en la GPU, mapas de sombras, "
                   "iluminación basada en imágenes, morph targets, texturas KTX2 y HDR, y cuadros de cámara "
                   "y video dibujados desde YUV. En iOS y Android dibuja en un <code>GpuView</code> de una "
                   "app de ui, y los mismos programas compilan para el navegador con emdawnwebgpu, con un "
                   "backend de WebGL2 donde el navegador no tiene WebGPU. Las ventanas de escritorio "
                   "reciben teclado, mouse, toque y gamepads. "
                   "<a href=\"gpu.html\">Qué hace, con sus mediciones →</a>",
        "needs_en": "Nothing to install: the first build downloads wgpu-native 29, and SDL3 for a "
                    "desktop window (<code>&lt;gpu/window&gt;</code>). With Eskiu 0.15.1, "
                    "<code>ESKIU_SYSTEM_LIBS=SDL3</code> links the system's SDL3 instead.",
        "needs_es": "Nada que instalar: el primer build descarga wgpu-native 29, y SDL3 para una "
                    "ventana de escritorio (<code>&lt;gpu/window&gt;</code>). Con Eskiu 0.15.1, "
                    "<code>ESKIU_SYSTEM_LIBS=SDL3</code> enlaza el SDL3 del sistema en su lugar.",
        "setup": [],
        "build": ["eskiuc build"],
    },
    {
        "name": "scene",
        "rev": "v0.2.0",
        "section": "A lit sphere",
        "tag_es": "Un grafo de escena y un renderizador de materiales para Eskiu, sobre gpu.",
        "note_en": "Nodes, cameras, shapes and glTF scenes, drawn with Constant, Lambert, Phong, Blinn or "
                   "PBR materials under ambient, directional, point and spot lights, transparency sorted "
                   "back to front, HDR with tone mapping and bloom, skyboxes, image-based lighting and "
                   "shadow maps, plus input, physics, animation, particles and headset rendering for Meta Quest and Apple Vision Pro. Checked against analytic values on Metal, Vulkan and DirectX 12.",
        "note_es": "Nodos, cámaras, figuras y escenas glTF, dibujados con materiales Constant, Lambert, Phong, "
                   "Blinn o PBR bajo luces ambiental, direccional, puntual y spot, transparencias ordenadas "
                   "de atrás hacia adelante, HDR con tone mapping y bloom, skyboxes, iluminación basada en "
                   "imágenes y mapas de sombras, además de input, física, animación, partículas y render "
                   "para Meta Quest y Apple Vision Pro. Probado contra valores analíticos en Metal, Vulkan y "
                   "DirectX 12.",
        "needs_en": "The first build downloads wgpu-native through gpu.",
        "needs_es": "El primer build descarga wgpu-native a través de gpu.",
        "setup": [],
        "build": ["eskiuc build"],
    },
    {
        "name": "onnx",
        "rev": "v0.2.1",
        "section": "Inference",
        "tag_es": "Inferencia de modelos ONNX para Eskiu, sobre ONNX Runtime.",
        "note_en": "A model exported from PyTorch, TensorFlow or scikit-learn runs on the CPU "
                   "everywhere, and on CoreML, CUDA, DirectML or WebGPU when the ONNX Runtime build "
                   "has them. Wrong input names, types or shapes come back as error messages. Its image "
                   "resizers give the same bytes as Pillow and OpenCV, and <code>&lt;onnx/detect&gt;</code> "
                   "decodes SSD boxes and runs NMS bit for bit as numpy, so a Python vision pipeline "
                   "moves over with the same numbers.",
        "note_es": "Un modelo exportado desde PyTorch, TensorFlow o scikit-learn corre en el CPU en "
                   "cualquier sistema, y en CoreML, CUDA, DirectML o WebGPU cuando el build de ONNX "
                   "Runtime los trae. Un nombre, tipo o shape de entrada equivocado regresa como "
                   "mensaje de error. Sus redimensionadores de imagen dan los mismos bytes que Pillow y "
                   "OpenCV, y <code>&lt;onnx/detect&gt;</code> decodifica cajas SSD y corre NMS bit por "
                   "bit como numpy, así que un pipeline de visión en Python se pasa con los mismos números.",
        "needs_en": "Nothing to install: the first build downloads ONNX Runtime 1.31 (macOS arm64, "
                    "Linux and Windows).",
        "needs_es": "Nada que instalar: el primer build descarga ONNX Runtime 1.31 (macOS arm64, "
                    "Linux y Windows).",
        "setup": [],
        "build": ["eskiuc build"],
    },
    {
        "name": "tflite",
        "rev": "v0.1.1",
        "section": "Inference",
        "tag_es": "Inferencia de modelos de TensorFlow Lite para Eskiu.",
        "note_en": "A <code>.tflite</code> model converted from TensorFlow, Keras, JAX or PyTorch runs "
                   "on the CPU with XNNPACK, and on the GPU through the Metal delegate (macOS) or the "
                   "OpenCL/OpenGL delegate (Linux). Quantized inputs and outputs are converted for you.",
        "note_es": "Un modelo <code>.tflite</code> convertido desde TensorFlow, Keras, JAX o PyTorch "
                   "corre en el CPU con XNNPACK, y en la GPU con el delegado de Metal (macOS) o el de "
                   "OpenCL/OpenGL (Linux). Las entradas y salidas cuantizadas se convierten solas.",
        "needs_en": "Nothing to install on macOS and Linux: the first build downloads "
                    "<code>libtensorflowlite_c</code> 2.21, which Google does not publish for desktops. "
                    "On Windows the package's <code>tools/get_tflite.sh</code> fetches it.",
        "needs_es": "Nada que instalar en macOS y Linux: el primer build descarga "
                    "<code>libtensorflowlite_c</code> 2.21, que Google no publica para escritorio. "
                    "En Windows lo baja el script <code>tools/get_tflite.sh</code> del paquete.",
        "setup": [],
        "build": ["eskiuc build"],
    },
    {
        "name": "compress",
        "section": "gzip and zlib",
        "tag_es": "zlib, gzip, DEFLATE crudo y archivos ZIP para Eskiu, escrito en Eskiu.",
        "note_en": "DEFLATE at levels 0 to 9, zlib and gzip in both directions, and ZIP archives "
                   "with ZIP64. Every read is bounds checked and the output is capped by a size "
                   "limit, so a small file cannot expand into all of memory.",
        "note_es": "DEFLATE en niveles del 0 al 9, zlib y gzip en los dos sentidos, y archivos ZIP "
                   "con ZIP64. Cada lectura revisa sus límites y la salida tiene un tope de tamaño, "
                   "así que un archivo chico no puede inflarse hasta llenar la memoria.",
        "needs_en": "Nothing to install: no zlib and no C code.",
        "needs_es": "No hay que instalar nada: no usa zlib ni código en C.",
        "setup": [],
        "build": [],
    },
    {
        "name": "yaml",
        "section": "Reading a file",
        "tag_es": "Archivos de configuración YAML para Eskiu, leídos al árbol JSON de la biblioteca estándar.",
        "note_en": "One YAML document under the YAML 1.2 core schema (block and flow collections, every "
                   "scalar style, comments) becomes a <code>JsonValue</code>, so settings are read with "
                   "<code>json_get</code> paths. Errors say the line and column. Pure Eskiu.",
        "note_es": "Un documento YAML con el esquema core de YAML 1.2 (colecciones en bloque y en línea, "
                   "todos los estilos de escalares, comentarios) se vuelve un <code>JsonValue</code>, así "
                   "que la configuración se lee con rutas de <code>json_get</code>. Los errores dicen línea "
                   "y columna. Eskiu puro.",
        "needs_en": "Nothing to install.",
        "needs_es": "No hay que instalar nada.",
        "setup": [],
        "build": [],
    },
    {
        "name": "audio",
        "rev": "v0.4.0",
        "section": "Playing a sound",
        "tag_es": "Sonido para Eskiu: archivos WAV y Ogg Vorbis, un mezclador y salida con SDL3.",
        "note_en": "Reads WAV files in every common encoding (PCM, float, IMA and Microsoft ADPCM) "
                   "as ffmpeg decodes them, and Ogg Vorbis on every platform with a decoder written in "
                   "Eskiu, whole or streamed, honoring the loop points a file carries. It mixes any number of voices with volume, pan, pitch, loops "
                   "and gapless intro-to-loop playlists, and sends the mix to the default output "
                   "device. In a ui app on iOS and Android it plays through the system (RemoteIO, "
                   "AAudio) and decodes MP3, AAC and FLAC with the system's codecs.",
        "note_es": "Lee archivos WAV en todas las codificaciones comunes (PCM, float, ADPCM de IMA y "
                   "de Microsoft) igual que los decodifica ffmpeg, y Ogg Vorbis en todas las plataformas con un "
                   "decodificador escrito en Eskiu, completo o en streaming, respetando los puntos de loop "
                   "que trae el archivo. Mezcla todas las voces que "
                   "quieras con volumen, paneo, tono, loops y listas de intro y loop sin cortes, y "
                   "manda la mezcla a la salida de audio predeterminada. En una app de ui en iOS y "
                   "Android suena a través del sistema (RemoteIO, AAudio) y decodifica MP3, AAC y "
                   "FLAC con los códecs del sistema.",
        "needs_en": "<code>&lt;audio/wav&gt;</code>, <code>&lt;audio/ogg&gt;</code> and <code>&lt;audio/mixer&gt;</code> are pure "
                    "Eskiu; <code>&lt;audio/device&gt;</code> needs SDL3 (<code>brew install sdl3</code> "
                    "on macOS).",
        "needs_es": "<code>&lt;audio/wav&gt;</code>, <code>&lt;audio/ogg&gt;</code> y <code>&lt;audio/mixer&gt;</code> son Eskiu puro; "
                    "<code>&lt;audio/device&gt;</code> necesita SDL3 (<code>brew install sdl3</code> en "
                    "macOS).",
        "setup": [],
        "build": ["eskiuc build -L/opt/homebrew/lib"],
    },
    {
        "name": "font",
        "section": "Drawing text",
        "tag_es": "Fuentes TrueType para Eskiu: contornos de glifos, renderizado con antialiasing, "
                  "acomodo de texto y atlas.",
        "note_en": "Reads <code>.ttf</code> files, renders glyphs into antialiased coverage bitmaps, "
                   "lays out lines of UTF-8 text with kerning, and packs a character set into one "
                   "texture for text on the GPU.",
        "note_es": "Lee archivos <code>.ttf</code>, convierte los glifos en bitmaps de cobertura con "
                   "antialiasing, acomoda líneas de texto UTF-8 con kerning y empaca un conjunto de "
                   "caracteres en una sola textura para dibujar texto en la GPU.",
        "needs_en": "Nothing to install.",
        "needs_es": "No hay que instalar nada.",
        "setup": [],
        "build": [],
    },
    {
        "name": "jni",
        "section": None,
        "tag_es": "Métodos nativos para Android y la JVM, escritos en Eskiu.",
        "note_en": "A native method of a Java or Kotlin class is an Eskiu function with the JNI "
                   "name, with no C glue: strings cross as real UTF-8, lookups come back as a "
                   "<code>Result</code>, and the whole JNIEnv table is there underneath. It runs "
                   "on Android and on the desktop JVMs of macOS, Linux and Windows.",
        "note_es": "Un método nativo de una clase de Java o Kotlin es una función de Eskiu con el "
                   "nombre de JNI, sin pegamento en C: los strings cruzan como UTF-8 de verdad, las "
                   "búsquedas regresan como <code>Result</code> y toda la tabla de JNIEnv está "
                   "debajo. Corre en Android y en las JVM de escritorio de macOS, Linux y Windows.",
        "needs_en": "Nothing to install: the VM hands the library its tables.",
        "needs_es": "No hay que instalar nada: la VM le pasa sus tablas a la biblioteca.",
        "setup": [],
        "build": ["eskiuc build --target aarch64-linux-android24 -o libhello.so"],
    },
    {
        "name": "quickjs",
        "section": "A game loop",
        "tag_es": "JavaScript para programas de Eskiu, sobre QuickJS.",
        "note_en": "Embeds QuickJS, the engine rquickjs 0.6.2 bundles with the same patches, for games "
                   "and tools that keep their logic in JavaScript. ES modules with top-level "
                   "<code>await</code>, values both ways (JSON and typed arrays included), Eskiu "
                   "closures as JavaScript functions, errors as a <code>Result</code> with their file, "
                   "line and column, and memory, stack and time limits.",
        "note_es": "Integra QuickJS, el motor que trae rquickjs 0.6.2 con los mismos parches, para juegos "
                   "y herramientas que guardan su lógica en JavaScript. Módulos ES con <code>await</code> "
                   "de nivel superior, valores en los dos sentidos (incluidos JSON y typed arrays), "
                   "closures de Eskiu como funciones de JavaScript, errores como un <code>Result</code> con "
                   "su archivo, línea y columna, y límites de memoria, pila y tiempo.",
        "needs_en": "Nothing to install: the first build downloads the prebuilt QuickJS library for "
                    "macOS, Linux, Windows, iOS or Android. Eskiu 0.15 or newer.",
        "needs_es": "Nada que instalar: el primer build descarga la biblioteca de QuickJS precompilada "
                    "para macOS, Linux, Windows, iOS o Android. Eskiu 0.15 o más nuevo.",
        "setup": [],
        "build": ["eskiuc build"],
    },
    {
        "name": "nds",
        "rev": "v0.3.0",
        "section": "Example",
        "tag_es": "Formatos de archivo de la Nintendo DS para Eskiu: ROMs, archivos NARC, sonido, "
                  "gráficos 2D, modelos 3D y partículas, y texto de juegos.",
        "note_en": "ROM file systems and overlays, NARC archives, LZ compression, SDAT sound with a "
                   "synthesizer of the DS sound driver, 2D graphics and 3D textures, in pure Eskiu. "
                   "NSBMD models and their joint, texture and material animations convert to glTF 2.0, "
                   "checked against apicula on 3,410 models, and SPL particle archives to an open JSON "
                   "format. "
                   "It was ported from the tools and engine of Tallgrass and checked against them.",
        "note_es": "Sistemas de archivos y overlays de ROMs, archivos NARC, compresión LZ, sonido "
                   "SDAT con un sintetizador del driver de sonido de la DS, gráficos 2D y texturas 3D, "
                   "todo en Eskiu puro. Los modelos NSBMD y sus animaciones de huesos, texturas y "
                   "materiales se convierten a glTF 2.0, comparados contra apicula en 3,410 modelos, y "
                   "los archivos de partículas SPL a un formato JSON abierto. "
                   "Se portó de las herramientas y el motor de Tallgrass y se "
                   "comparó contra ellos.",
        "needs_en": "Nothing to install.",
        "needs_es": "No hay que instalar nada.",
        "setup": [],
        "build": [],
    },
    {
        "name": "location",
        "repo": "ui-location",
        "rev": "v0.1.3",
        "section": None,
        "tag_es": "Dónde está el dispositivo, para apps hechas con eskiu-lang/ui.",
        "note_en": "A component calls <code>use_location</code> and gets the latest position while "
                   "it is mounted, measured by the platform's own service: CoreLocation on iOS, "
                   "LocationManager on Android and the browser's geolocation on the web. The first "
                   "use asks for permission, and the package brings the framework, the usage text "
                   "and the Android permissions through its <code>ui.toml</code>.",
        "note_es": "Un componente llama a <code>use_location</code> y recibe la última posición mientras "
                   "está montado, medida por el servicio de cada plataforma: CoreLocation en iOS, "
                   "LocationManager en Android y la geolocalización del navegador en la web. El primer "
                   "uso pide permiso, y el paquete trae el framework, el texto de uso y los permisos de "
                   "Android en su <code>ui.toml</code>.",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "notifications",
        "repo": "ui-notifications",
        "rev": "v0.1.3",
        "section": None,
        "tag_es": "Notificaciones locales para apps hechas con eskiu-lang/ui.",
        "note_en": "The app schedules a notification with a title and a body for a number of seconds "
                   "from now; the system shows it then, also when the app is closed, and tells the app "
                   "when the user opens it. UNUserNotificationCenter on iOS, AlarmManager and "
                   "NotificationManager on Android, the Notification API on the web.",
        "note_es": "La app programa una notificación con título y texto para dentro de unos segundos; "
                   "el sistema la muestra entonces, también con la app cerrada, y le avisa a la app cuando "
                   "el usuario la abre. UNUserNotificationCenter en iOS, AlarmManager y NotificationManager "
                   "en Android, la API Notification en la web.",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run android"],
    },
    {
        "name": "camera",
        "repo": "ui-camera",
        "rev": "v0.2.1",
        "section": None,
        "tag_es": "Fotos para apps hechas con eskiu-lang/ui: la galería, la cámara y una vista en vivo.",
        "note_en": "Pick a photo from the library, take one with the system's camera, or show the camera "
                   "live in a <code>CameraView</code> and capture it. Each photo comes back upright as a "
                   "JPEG that <code>Image</code> shows. PHPicker and AVFoundation on iOS, the photo picker "
                   "and Camera2 on Android, a file input and getUserMedia on the web. "
                   "<code>use_camera_frames</code> lends each live frame to Eskiu by pointer (YUV planes, "
                   "newest frame wins) for vision code or for <code>&lt;gpu/yuv&gt;</code> to draw.",
        "note_es": "Elige una foto de la galería, toma una con la cámara del sistema, o muestra la cámara en "
                   "vivo en un <code>CameraView</code> y captúrala. Cada foto regresa derecha como un JPEG que "
                   "<code>Image</code> muestra. PHPicker y AVFoundation en iOS, el selector de fotos y Camera2 "
                   "en Android, un input de archivo y getUserMedia en la web. "
                   "<code>use_camera_frames</code> le presta a Eskiu cada cuadro en vivo por puntero (planos "
                   "YUV, gana el más nuevo) para código de visión o para dibujarlo con "
                   "<code>&lt;gpu/yuv&gt;</code>.",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run android"],
    },
    {
        "name": "video",
        "rev": "v0.1.1",
        "ui": True,
        "section": "Use it",
        "tag_es": "Reproducción de video para apps hechas con eskiu-lang/ui, con los cuadros decodificados en Eskiu.",
        "note_en": "<code>use_video</code> plays files, URLs and app assets with the platform's own player "
                   "(AVPlayer on iOS, MediaPlayer on Android, the video element on the web), "
                   "<code>VideoView</code> shows it, and <code>use_video_frames</code> lends the decoded "
                   "frames to Eskiu in the same layout as the camera's, for <code>&lt;gpu/yuv&gt;</code> "
                   "to draw.",
        "note_es": "<code>use_video</code> reproduce archivos, URLs y recursos de la app con el reproductor de "
                   "cada plataforma (AVPlayer en iOS, MediaPlayer en Android, el elemento video en la web), "
                   "<code>VideoView</code> lo muestra, y <code>use_video_frames</code> le presta a Eskiu los "
                   "cuadros decodificados con el mismo formato que los de la cámara, para dibujarlos con "
                   "<code>&lt;gpu/yuv&gt;</code>.",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "ar",
        "rev": "v0.2.0",
        "ui": True,
        "section": None,
        "tag_es": "Realidad aumentada para apps hechas con eskiu-lang/ui: ARKit en iOS, ARCore en Android y WebXR en la web.",
        "note_en": "<code>use_ar_session</code> (<code>&lt;ar/ui&gt;</code>) gives each frame's camera pose, planes, anchors, hit tests, "
                   "image targets, light and depth, and the camera image in the same layout as the camera's frames, so "
                   "<code>&lt;gpu/yuv&gt;</code> draws the background and <a href=\"#scene\">scene</a> the "
                   "content. ARCore runs with no OpenGL context, the projection is computed in Eskiu "
                   "the same way on every platform, and in Chrome on Android a WebXR session gives the "
                   "same data.",
        "note_es": "<code>use_ar_session</code> (<code>&lt;ar/ui&gt;</code>) da en cada cuadro la pose de la cámara, planos, anclas, hit "
                   "tests, imágenes objetivo, luz y profundidad, y la imagen de la cámara con el mismo formato que los cuadros "
                   "de la cámara, así que <code>&lt;gpu/yuv&gt;</code> dibuja el fondo y "
                   "<a href=\"#scene\">scene</a> el contenido. ARCore corre sin contexto de OpenGL, la "
                   "proyección se calcula en Eskiu igual en todas las plataformas, y en Chrome en Android "
                   "una sesión de WebXR da los mismos datos.",
        "needs_en": "An app made with eskiu-lang/ui (ui new), on a phone with ARKit or ARCore.",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new), en un teléfono con ARKit o ARCore.",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "arscene",
        "section": None,
        "ui": True,
        "tag_es": "Une una sesión de ar con una escena de scene, como el navegador de AR de ViroReact.",
        "note_en": "<code>ArStage</code> puts each AR frame into a scene: the camera picture behind it, "
                   "the device as the camera with the session's projection, and the real world's depth "
                   "hiding virtual content. <code>ArPlacer</code> places content where the user taps "
                   "(ViroARPlaneSelector), <code>ArMarkers</code> follows tracked images and "
                   "<code>ArWorldMesh</code> turns the LiDAR mesh into occluders and physics bodies.",
        "note_es": "<code>ArStage</code> lleva cada cuadro de AR a una escena: la imagen de la cámara "
                   "detrás, el dispositivo como cámara con la proyección de la sesión, y la profundidad del "
                   "mundo real ocultando el contenido virtual. <code>ArPlacer</code> coloca contenido donde "
                   "el usuario toca (ViroARPlaneSelector), <code>ArMarkers</code> sigue imágenes y "
                   "<code>ArWorldMesh</code> convierte la malla de LiDAR en oclusores y cuerpos físicos.",
        "needs_en": "An app made with eskiu-lang/ui (ui new), on a phone with ARKit or ARCore; it brings ar and scene.",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new), en un teléfono con ARKit o ARCore; trae ar y scene.",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "canvas",
        "rev": "v0.1.1",
        "repo": "ui-canvas",
        "section": None,
        "tag_es": "Un canvas 2D para Eskiu, en una app de ui (la GPU en teléfonos, Canvas 2D en la web) o fuera de pantalla.",
        "note_en": "Sprites and pixel art drawn every display frame in a <code>CanvasView</code>: atlas "
                   "cells into rects with nearest or linear sampling, opacity, a tint, mirroring and a "
                   "transform stack, batched into one draw call per image. WebGPU through gpu on iOS "
                   "and Android, the browser's Canvas 2D on the web; PNG, JPEG and, on phones, WebP.",
        "note_es": "Sprites y pixel art dibujados en cada frame del display en un <code>CanvasView</code>: "
                   "celdas de un atlas en rects con muestreo nearest o lineal, opacidad, tinte, espejo y "
                   "una pila de transformaciones, en lotes de una llamada de dibujo por imagen. WebGPU "
                   "con gpu en iOS y Android, el Canvas 2D del navegador en la web; PNG, JPEG y, en "
                   "teléfonos, WebP.",
        "needs_en": "An app made with eskiu-lang/ui (ui new); the first build downloads wgpu-native.",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new); el primer build descarga wgpu-native.",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "svg",
        "rev": "v0.1.1",
        "repo": "ui-svg",
        "section": None,
        "tag_es": "Dibujos vectoriales desde texto SVG, para apps hechas con eskiu-lang/ui.",
        "note_en": "<code>Svg</code> draws an SVG document scaled to its box: paths with every command "
                   "and arcs, the basic shapes, groups, transforms, fill and stroke, and "
                   "<code>currentColor</code> from the style, so one icon takes any tint. The document "
                   "is parsed in Eskiu; Core Graphics, Android's Canvas or the browser draws it.",
        "note_es": "<code>Svg</code> dibuja un documento SVG escalado a su caja: paths con todos los "
                   "comandos y arcos, las figuras básicas, grupos, transformaciones, relleno y trazo, y "
                   "<code>currentColor</code> desde el estilo, así que un ícono toma cualquier tinte. El "
                   "documento se interpreta en Eskiu; lo dibuja Core Graphics, el Canvas de Android o el "
                   "navegador.",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run web"],
    },
    {
        "name": "pedometer",
        "rev": "v0.1.1",
        "repo": "ui-pedometer",
        "section": None,
        "tag_es": "Pasos caminados, en vivo y entre dos momentos, para apps de ui en iOS y Android.",
        "note_en": "<code>use_steps</code> counts the steps walked while the component is mounted and "
                   "<code>use_steps_between</code> the ones between two times, also those walked with "
                   "the app closed. CMPedometer on iOS; on Android the step counter sensor, with the "
                   "samples the package keeps to answer for past times.",
        "note_es": "<code>use_steps</code> cuenta los pasos mientras el componente está montado y "
                   "<code>use_steps_between</code> los que hay entre dos momentos, también los que se "
                   "dieron con la app cerrada. CMPedometer en iOS; en Android el sensor de pasos, con las "
                   "muestras que el paquete guarda para responder por momentos pasados.",
        "needs_en": "An app made with eskiu-lang/ui (ui new), on a device.",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new), en un dispositivo.",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
    {
        "name": "barcode",
        "rev": "v0.1.1",
        "repo": "ui-barcode",
        "section": None,
        "tag_es": "Códigos QR y de barras leídos en vivo con la cámara, para apps hechas con eskiu-lang/ui.",
        "note_en": "A <code>BarcodeScanner</code> shows the back camera and hands each code it reads to "
                   "its handler: QR, Aztec, Data Matrix, PDF417 and the 1D formats. AVFoundation on iOS, "
                   "ML Kit bundled with the app on Android, the browser's BarcodeDetector on the web.",
        "note_es": "Un <code>BarcodeScanner</code> muestra la cámara trasera y le pasa cada código que lee "
                   "a su handler: QR, Aztec, Data Matrix, PDF417 y los formatos 1D. AVFoundation en iOS, "
                   "ML Kit empaquetado con la app en Android, el BarcodeDetector del navegador en la web.",
        "needs_en": "An app made with eskiu-lang/ui (ui new), on a device with a camera.",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new), en un dispositivo con cámara.",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run android"],
    },
    {
        "name": "securestore",
        "rev": "v0.1.1",
        "repo": "ui-securestore",
        "section": None,
        "tag_es": "Secretos pequeños guardados por el almacenamiento seguro de la plataforma, para apps de ui.",
        "note_en": "<code>secure_set</code>, <code>secure_get</code> and <code>secure_remove</code> keep "
                   "a session token or a key: in the Keychain on iOS, encrypted with a key of the Android "
                   "Keystore on Android, and in <code>localStorage</code> on the web, which has no secure "
                   "store (<code>secure_is_secure</code> says so).",
        "note_es": "<code>secure_set</code>, <code>secure_get</code> y <code>secure_remove</code> guardan "
                   "un token de sesión o una llave: en el Keychain en iOS, cifrados con una llave del "
                   "Android Keystore en Android, y en <code>localStorage</code> en la web, que no tiene "
                   "almacenamiento seguro (<code>secure_is_secure</code> lo dice).",
        "needs_en": "An app made with eskiu-lang/ui (ui new).",
        "needs_es": "Una app hecha con eskiu-lang/ui (ui new).",
        "setup": ["ui new myapp && cd myapp"],
        "build": ["ui run ios"],
    },
]
