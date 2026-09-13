cmake_minimum_required(VERSION 3.22)

# **Esta versión es la fuente única y tiene que coincidir con el tag `obs-vX.Y.Z` que la publica.**
# Estuvo en 1.0.0 mientras los releases iban por 2.2.0, así que el número que el plugin creía tener
# estaba tres versiones atrasado — inservible para decidir si hay que actualizar. El workflow de
# release ahora compara ambos y falla si difieren, para que no vuelva a separarse en silencio.
project(obs-praisehim VERSION 4.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Dependencias externas ─────────────────────────────────────
include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json stb)

# ── OBS (apuntar con -DOBS_DIR=/ruta si no está en el PATH) ──
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(OBS_DIR "" CACHE PATH "Raíz de la instalación OBS o árbol cmake install de obs-studio")
if(OBS_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${OBS_DIR}")
endif()

find_package(libobs          REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(CURL REQUIRED)

# ── Target ────────────────────────────────────────────────────
add_library(obs-praisehim MODULE
    src/plugin-main.cpp
    src/sse-client.cpp
    src/praisehim-source.cpp
    src/stb_image_impl.cpp
    src/account-connect.cpp
)

# Sin prefijo "lib" para que el archivo se llame obs-praisehim.so/.dll
set_target_properties(obs-praisehim PROPERTIES PREFIX "")

# La versión llega al código en vez de estar escrita a mano en un literal, que es como se había
# desfasado: el log decía 1.0.0 con el plugin publicado en 2.2.0.
target_compile_definitions(obs-praisehim PRIVATE PH_VERSION="${PROJECT_VERSION}")

target_include_directories(obs-praisehim PRIVATE
    "${stb_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(obs-praisehim PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# "Conectar cuenta" escucha en 127.0.0.1 con sockets del sistema (account-connect.cpp).
if(WIN32)
    target_link_libraries(obs-praisehim PRIVATE ws2_32)
endif()

# ── Locale ────────────────────────────────────────────────────
set(PLUGIN_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")

# ── Install ───────────────────────────────────────────────────
include(GNUInstallDirs)

# LOCAL_INSTALL=ON → instala en ~/.config/obs-studio/plugins/obs-praisehim/
# (estructura que OBS detecta automáticamente, igual al release de GitHub).
# No requiere sudo ni symlinks. Uso: cmake --install build (sin sudo)
option(LOCAL_INSTALL "Instalar en el directorio de plugins de usuario de OBS" OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LOCAL_INSTALL)
    set(_user_plugin_dir "$ENV{HOME}/.config/obs-studio/plugins/obs-praisehim")
    install(TARGETS obs-praisehim
        LIBRARY DESTINATION "${_user_plugin_dir}/bin/64bit"
    )
    install(DIRECTORY "${PLUGIN_DATA_DIR}/"
        DESTINATION "${_user_plugin_dir}/data"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(TARGETS obs-praisehim
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    )
    install(DIRECTORY "${PLUGIN_DATA_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-praisehim"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    install(TARGETS obs-praisehim
        LIBRARY DESTINATION "obs-plugins/64bit"
    )
    install(DIRECTORY "${PLUGIN_DATA_DIR}/"
        DESTINATION "data/obs-plugins/obs-praisehim"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(obs-praisehim PROPERTIES
        BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "obs-praisehim"
    )
    install(TARGETS obs-praisehim
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    )
    install(DIRECTORY "${PLUGIN_DATA_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-praisehim"
    )
endif()
