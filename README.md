# obs-praisehim

Plugin para OBS Studio que conecta con el sistema de presentación PraiseHim y renderiza letras de canciones, versículos bíblicos, diapositivas y anuncios directamente como fuente nativa en OBS.

## Descarga

Los binarios de cada versión están en los [Releases](https://github.com/joririarte/praisehim-obs-plugin/releases/latest) del repositorio público del plugin, y la app los ofrece desde la vista **OBS Studio**. Compilar desde el código, como se explica más abajo, solo hace falta para desarrollar.

## Licencia

**GPL-2.0-or-later** — ver [LICENSE](LICENSE). No es una elección: el plugin enlaza contra libobs, que es GPLv2, y esa es la condición para poder ser un plugin de OBS. Por eso el código de este directorio se publica junto con cada binario.

## Requisitos

- OBS Studio 29+
- CMake 3.22+
- Qt 6.x (misma versión que usa tu OBS)
- libcurl
- Compilador con C++17

## Compilar

### Linux — OBS instalado desde el PPA oficial (recomendado)

Si ya tenés OBS Studio instalado desde el PPA de obsproject, los headers y cmake configs ya están en el sistema (`/usr/include/obs/` y `/usr/lib/.../cmake/`). **No instales `libobs-dev`** desde los repos de Ubuntu/Debian — ese paquete desinstalaría tu OBS.

Solo instalá las dependencias que faltan:

```bash
sudo apt install cmake libsimde-dev libcurl4-openssl-dev qt6-base-dev
```

> `libsimde-dev` es requerida por el cmake config de OBS aunque no se use directamente en el plugin.

Luego compilá e instalá:

```bash
cd obs-plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOCAL_INSTALL=ON
cmake --build build -j$(nproc)
cmake --install build
```

CMake encuentra OBS automáticamente en `/usr/lib/x86_64-linux-gnu/cmake/`. No hace falta `-DOBS_DIR`.

Con `LOCAL_INSTALL=ON` el plugin se instala en `~/.config/obs-studio/plugins/obs-praisehim/`, que es el directorio de plugins de usuario que OBS detecta automáticamente. No se necesita `sudo` ni crear symlinks — al reiniciar OBS aparece **PraiseHim Source** directamente en el menú Agregar Fuente.

### Linux — OBS NO instalado previamente

Si no tenés OBS instalado, instalá todo junto. En este caso `libobs-dev` no entra en conflicto:

```bash
sudo apt install cmake libobs-dev libcurl4-openssl-dev qt6-base-dev
cd obs-plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOCAL_INSTALL=ON
cmake --build build -j$(nproc)
cmake --install build
```

Si OBS está en una ruta no estándar, especificala con `-DOBS_DIR`:

```bash
cmake -B build -DOBS_DIR=/ruta/a/obs-studio/install -DCMAKE_BUILD_TYPE=Release
```

### Windows

```bat
cmake -B build ^
  -DOBS_DIR="C:/Program Files/obs-studio" ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix "C:/Program Files/obs-studio"
```

### macOS

```bash
cmake -B build \
  -DOBS_DIR=/Applications/OBS.app/Contents \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

## Uso

1. Abrí OBS Studio
2. En el panel de **Fuentes**, hacé clic en `+` → **PraiseHim Source**
3. Configurá:
   - **URL del servidor**: `http://localhost` (o la IP donde corre PraiseHim)
   - **Token OBS**: el UUID que aparece en la vista *OBS Studio* del panel web de PraiseHim
   - **Modo**: elegí *Texto* o *Multimedia* según lo que quieras mostrar
4. Ajustá el estilo, posición y fondo según tu preferencia
5. La fuente se actualiza en tiempo real vía SSE cada vez que el presentador avanza una diapositiva

## Comportamiento

| Estado del presentador | Modo Texto | Modo Multimedia |
|---|---|---|
| `live: false` | Solo fondo configurado | Negro o logo configurado |
| `live: true`, `black: true` | Solo fondo | Negro o logo |
| `live: true`, con contenido | Texto + fondo | Imagen de la diapositiva |

## Debug / logs

Para ver los mensajes del plugin en los logs de OBS:

```bash
grep -i praisehim ~/.config/obs-studio/logs/"$(ls -t ~/.config/obs-studio/logs/ | head -1)"
```

O desde OBS: **Ayuda → Archivos de log → Ver log actual** y buscá "PraiseHim".

## Notas de desarrollo

- La descarga de imágenes (modo multimedia) es síncrona en el hilo de render, lo que introduce un breve delay en el primer slide de cada presentación. En conexiones de red local (<LAN>) esto es imperceptible.
- El plugin soporta múltiples instancias en escenas distintas con configuraciones independientes.
- El formato de color interno es ARGB (`0xAARRGGBB`), compatible con `QColor::fromRgba`.
