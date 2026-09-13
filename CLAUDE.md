# OBS Plugin Agent — obs-praisehim

## Versión y publicación

⚠️ **La versión vive en un solo lugar: `project(obs-praisehim VERSION ...)` de `CMakeLists.txt`.** De ahí sale la define `PH_VERSION` que usa el C++, y el plugin la manda al servidor al conectarse (`&v=` en la URL del SSE) para que la app pueda avisar cuando quedó viejo. **Se sube en el mismo commit que el tag `obs-vX.Y.Z`**: el workflow compara ambos y falla si difieren. Esa guarda existe porque ya se separaron — los releases llegaron a `obs-v2.2.0` con CMakeLists todavía en `1.0.0`.

El plugin es **GPL-2.0-or-later** (enlaza libobs) y su código se publica, junto con los binarios, en el repositorio **público** `praisehim-obs-plugin`. Este monorepo sigue siendo la fuente de verdad; el público es destino de publicación y recibe un *snapshot* por release, sin historia. Ver `.github/workflows/release-obs.yml`.

## Stack
C++17 · Qt6 (QPainter, QImage, QFont) · libobs · libcurl · nlohmann/json · stb_image

## Requisitos
OBS Studio 29+ · CMake 3.22+ · Qt 6.x (misma versión que usa tu OBS) · libcurl · compilador C++17.

## Compilar e instalar

**Linux, con OBS ya instalado desde el PPA oficial (caso común para desarrollo):** no instalar `libobs-dev` de los repos — desinstalaría OBS. Los headers/cmake configs ya están en `/usr/include/obs/` y `/usr/lib/.../cmake/`.
```bash
sudo apt install cmake libsimde-dev libcurl4-openssl-dev qt6-base-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOCAL_INSTALL=ON
cmake --build build -j$(nproc)
cmake --install build   # con LOCAL_INSTALL=ON no hace falta sudo
```
`-DLOCAL_INSTALL=ON` instala en `~/.config/obs-studio/plugins/obs-praisehim/` (directorio de usuario, detectado automáticamente al reiniciar OBS).

**Linux, sin OBS instalado:** `sudo apt install cmake libobs-dev libcurl4-openssl-dev qt6-base-dev` y los mismos pasos de cmake (sin conflicto en este caso). ⚠️ `libobs-dev` tiene que ser de **OBS 28 o posterior**: el de Ubuntu 22.04 es 27.2 y no compila (faltan `OBS_TEXT_INFO` y `obs_property_text_set_info_type`). Ahí, y en el workflow de release, se usa el paquete `obs-studio` del PPA oficial, que trae los headers (OBS 30.2 en jammy). Si OBS está en ruta no estándar, agregar `-DOBS_DIR=/ruta/a/obs-studio/install`.

**Windows / macOS:** ver `README.md` (requiere pasar `-DOBS_DIR` apuntando a la instalación de OBS).

```bash
# Re-compilar después de cambios
cmake --build build -j$(nproc) && cmake --install build

# Ver logs del plugin en OBS
grep -i praisehim ~/.config/obs-studio/logs/"$(ls -t ~/.config/obs-studio/logs/ | head -1)"
```

## Arquitectura

### Flujo de datos
```
Backend SSE  →  SseClient (hilo)  →  on_state()  →  work_cv.notify
                                                           ↓
                                              render_worker_loop (hilo)
                                                           ↓
                                              render_frame() → push_frame()
                                                           ↓
                                         obs_source_output_video()  →  OBS
```

### Tipo de fuente OBS
- `OBS_SOURCE_ASYNC_VIDEO`: la fuente empuja frames raw con `obs_source_output_video`
- No usa `video_render` ni texturas OpenGL/D3D
- Las dimensiones las infiere OBS del primer frame recibido

### Visibilidad (show / hide)
OBS **descarta los frames de una fuente que no se está renderizando**. Sin manejar esto,
ocultar la fuente con el ojo de la lista de Sources y volver a mostrarla dejaba en pantalla
el slide anterior, aunque el estado nuevo hubiera llegado por SSE.

`ph_show`/`ph_hide` mantienen el flag `visible` (atomic, arranca en `true`):
- `render_frame()` corta apenas entra si no está visible — no gasta QPainter ni descarga
  imágenes que nadie va a ver. El estado igual sigue llegando y guardándose en `state`.
- `ph_show` llama a `request_render()` para reemitir el frame con lo último recibido.
  **No consulta al backend**: el estado nunca se perdió, solo el frame.

Cubre además el cambio de escena y el preview del diálogo de propiedades, que pasan por los
mismos callbacks. El cache `current_img_url`/`current_img` hace que al mostrar no haya
descarga si el presentador no cambió de página mientras estuvo oculta.

### Conectar cuenta (`account-connect.cpp`, #42 fase 5)
Segunda forma de conexión, junto al token manual (`conn_type`: `CONN_ACCOUNT` / `CONN_TOKEN`). Es OAuth
para escritorio —**loopback + PKCE**—, detallado en `docs/control-remoto/analisis.md` §15 y en
`ObsAccountService` del backend:
- `AccountConnectFlow::preparar` abre un socket en `127.0.0.1:0` (puerto que elige el sistema) y arma
  `<server>/obs/conectar?redirect_uri=http://127.0.0.1:<p>/callback&state&code_challenge(S256)&dispositivo`.
  El navegador se abre con `QDesktopServices::openUrl` **desde el callback del botón** (hilo de UI).
- `esperar_y_canjear` corre en `account_thread`: `select` de 500 ms para poder cancelar, contesta 404 a
  lo que no sea `/callback` (el favicon), **ignora un `state` ajeno** y sigue esperando, y canjea en
  `POST /api/obs/cuenta/token` con el verifier. Sockets del sistema y no Qt Network, que OBS no garantiza
  en todas las plataformas; en Windows `winsock2.h` va antes que cualquier otro include y enlaza `ws2_32`.
- ⚠️ **La cuenta es una por instalación**, en `plugin_config/obs-praisehim/cuenta.json`
  (`ph_cuenta_actual/guardar/olvidar`), **no en los ajustes de la fuente**: el *Cancelar* del diálogo
  de propiedades limpia y restaura los ajustes del momento en que se abrió, y con el token ahí se perdía
  la conexión recién hecha. Cada fuente guarda solo `servicio_id` (0 = el por defecto).
- El resultado vuelve al hilo de UI con `obs_queue_task(OBS_TASK_UI, …)` y una referencia **débil** a la
  fuente (si la borraron mientras se esperaba, no se toca nada). Ahí se guarda la cuenta, se reconectan
  **todas** las fuentes PraiseHim (`obs_enum_sources`) y `obs_source_update_properties` refresca el
  diálogo abierto. `ph_destroy` cancela y hace join del hilo antes de liberar.
- Con cuenta, el SSE va a `/api/obs/state?servicio=<id>` con `Authorization: Bearer pho_…` en un
  **header** (no en la URL). Una fuente guardada antes de esta versión, sin `conn_type` y con token,
  sigue por token (`conn_type_de`).
- Al crear la fuente se refresca la lista de servicios en segundo plano: un 401 ahí es que la cuenta se
  revocó (o el usuario pasó a músico) y se olvida.

### SSE Client
- Hilo dedicado con libcurl (`curl_easy_perform` bloqueante)
- Para interrumpir al cerrar: el write callback retorna 0 cuando `!running_`
- El backend envía heartbeat cada 15s para evitar que curl quede bloqueado indefinidamente
- Parseo de líneas SSE: solo procesa `data:`, ignora `event:`, `id:`, comentarios
- Cabeceras extra por constructor (el `Authorization` de la cuenta). Un 401/402/403/404 no es un corte de
  red: reintenta cada 30 s en vez de 3

### Rendering (QPainter en hilo worker)
- `QImage(W, H, Format_ARGB32)` → `QPainter` → `convertToFormat(Format_ARGB32)`
- En little-endian, `Format_ARGB32` en memoria = BGRA = `VIDEO_FORMAT_BGRA` de OBS
- **Orden de bytes de los colores de OBS**: `obs_data`/`_color_alpha` entrega `0xAABBGGRR` (R,G,B,A en memoria), pero `QColor::fromRgba` espera `0xAARRGGBB` — interpretarlo directo intercambia R y B. Convertir siempre con la helper `obs_to_qcolor` (desarma byte a byte), no pasar el `uint32_t` crudo a `QColor`.
- Render text mode: **caja de texto de tamaño fijo** (`box_w_pct`/`box_h_pct`, % del lienzo, centrada horizontalmente y posicionada por `y_center`); el texto se achica hasta entrar en la caja si desbordaría. En canciones los saltos de línea se colapsan a espacio (`qtext.replace('\n', ' ')`) y el texto fluye; en biblia se respetan. Fondo sólido con opacidad propia (ver abajo) + resaltado por línea + texto con outline opcional + cita (la cita respeta outline y resaltado).
- **El wrap se hace a mano con `QTextLayout`, no con `drawText(rect, flags)`**: hace falta la geometría de cada línea para pintarle el resaltado detrás, y `drawText` no la expone. `wrap_text()` arma un layout por párrafo (QTextLayout no corta en `\n`) y devuelve la altura total; el mismo layout que se midió en el auto-ajuste es el que se dibuja (`draw_wrapped`), así resaltado y texto no pueden desalinearse. El outline redibuja ese layout desplazado, igual que antes.
- **Resaltado de línea** (`highlight_color` + `highlight_opacity`, sección Tipografía): rectángulo redondeado por línea usando `QTextLine::naturalTextRect()` — el ancho sigue a la letra, no a la caja. Opacidad `0` (el default) = no se dibuja nada, así que las escenas existentes no cambian. Es configuración **local del plugin**, igual que el resto de la tipografía: los campos `highlightColor`/`highlightOpacity` que PraiseHim manda por SSE son para las pantallas web y el plugin los ignora.
- Render multimedia: `stb_image` decodifica JPEG descargado con libcurl → letterbox negro

### JSON null safety
`j.value("field", default)` de nlohmann lanza `type_error` si el campo existe pero es null.
Siempre usar la lambda `str()`/`boolean()` definida en `SlideState::fromJson`.

## Modos
| Estado en PraiseHim | Modo texto | Modo texto + "Ocultar automáticamente" | Modo multimedia |
|---|---|---|---|
| Nada en vivo (`live=false`) | Solo fondo configurado | Nada (frame transparente) | Negro o logo local |
| Limpiar (`slideText` vacío) | Solo fondo | Nada | Sin cambios (sigue la imagen) |
| Logo (`logo=true`) | Solo fondo | Nada | Logo de la **organización** |
| Slide con texto | Texto + fondo | Texto + fondo | Imagen del backend |

Detalles que no se leen de la tabla:

- **`black` es rama muerta**: el frontend manda `black: false` siempre (`usePresenterState.js`),
  en las cuatro salidas. "Limpiar" viaja como `slideText` vacío, no como `black: true`. La
  condición del plugin lo sigue contemplando porque no cuesta nada, pero no construir encima.
- **Con logo, PraiseHim igual manda el texto del slide** — solo lo vacía al limpiar o en
  media. Que el logo limpie el texto lo decide el plugin (`!s.logo` en `has_text`), no el
  backend; así las pantallas no se ven afectadas.
- **El logo de multimedia es el de la organización** (`logoImageUrl`, ruta relativa,
  descargada y cacheada en `current_logo_img` igual que `current_img`), no el archivo local
  de la propiedad "Imagen logo" — ese sigue siendo para "sin contenido activo". Si la org no
  tiene logo o falla la descarga, cae a ese fallback. Se dibuja con letterbox (no recorta,
  a diferencia del logo local, que usa `KeepAspectRatioByExpanding` + crop).
- **"Ocultar automáticamente"** (`auto_hide`, propiedad en la sección Fondo, solo modo texto)
  emite un frame **transparente**: hace desaparecer el render sin ocultar la fuente. No
  confundir con `show`/`hide`, que es el estado de visibilidad de la fuente en OBS.

## Propiedades OBS (obs_properties)
- Visibilidad condicional con `obs_property_set_modified_callback`
- `obs_properties_add_color_alpha` para colores con canal alfa
- `obs_properties_add_font` NO se usa; fuente separada en texto + tamaño + bold/italic
- El fondo sólido usa **color RGB opaco (`bg_color`, alfa 0xFF) + un slider de opacidad aparte** (`bg_opacity`, 0-100 %, visible solo con `BG_SOLID`), no el canal alfa del color. El resaltado de línea sigue el mismo patrón (`highlight_color` + `highlight_opacity`, default 0).
- Tamaño de la caja de texto: sliders `box_w_pct` (10-100) y `box_h_pct` (5-100), % del lienzo.

## Contrato con el backend
- `slideType` del `SlideState` es `"songs" | "bible" | "presentation" | "announcement"` (default `"songs"`) — coincide con `item.type` del frontend; cuidado que NO es `"song"` en singular.
- `imageUrl` y `logoImageUrl` son **rutas relativas** (`/api/media/...`, `/api/backgrounds/...`): hay que anteponerles `server_url`.
- El SSE entrega **todos** los estados, incluidos los `logo == true`. Antes el backend los filtraba y el plugin no se enteraba de que el presentador había mandado el logo (ver `docs/ultimo-estado-presentacion.md`).

## GitHub Actions
- Workflow en `.github/workflows/release.yml`
- Trigger: `git tag v1.0.0 && git push origin v1.0.0`
- Produce ZIPs para Linux, Windows y macOS en GitHub Releases
