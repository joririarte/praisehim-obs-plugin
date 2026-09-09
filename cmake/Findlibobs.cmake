# ─── Paso 1: cmake config oficial de OBS (cuando está disponible) ────────
find_package(libobs CONFIG QUIET)
if(libobs_FOUND)
    return()
endif()

# ─── Paso 2: fallback por plataforma ─────────────────────────────────────
if(WIN32)
    # El instalador de OBS incluye obs.lib en bin/64bit pero NO headers.
    # Los headers deben venir de la fuente de OBS (OBS_SOURCE_DIR).
    find_path(LIBOBS_INCLUDE_DIR
        NAMES obs-module.h
        PATHS
            "${OBS_SOURCE_DIR}/libobs"
            "${OBS_DIR}/include/obs"
            "${OBS_DIR}/include"
        NO_DEFAULT_PATH
    )
    find_library(LIBOBS_LIBRARY
        NAMES obs
        PATHS
            "${OBS_DIR}/bin/64bit"
            "${OBS_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT LIBOBS_INCLUDE_DIR)
        message(FATAL_ERROR
            "obs-module.h no encontrado.\n"
            "Pasá -DOBS_SOURCE_DIR=<ruta/obs-studio> con la fuente de OBS.")
    endif()
    if(NOT LIBOBS_LIBRARY)
        message(FATAL_ERROR
            "obs.lib no encontrado en OBS_DIR='${OBS_DIR}'.\n"
            "Asegurate de que OBS Studio esté instalado correctamente.")
    endif()
else()
    # Linux: pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_LIBOBS REQUIRED libobs)

    find_path(LIBOBS_INCLUDE_DIR
        NAMES obs-module.h
        HINTS ${PC_LIBOBS_INCLUDE_DIRS}
        PATH_SUFFIXES obs
    )
    find_library(LIBOBS_LIBRARY
        NAMES obs
        HINTS ${PC_LIBOBS_LIBRARY_DIRS}
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
    )

    if(NOT LIBOBS_INCLUDE_DIR)
        message(FATAL_ERROR "obs-module.h no encontrado. Instalá libobs-dev.")
    endif()
    if(NOT LIBOBS_LIBRARY)
        message(FATAL_ERROR "librería obs no encontrada. Instalá libobs-dev.")
    endif()
    set(libobs_VERSION "${PC_LIBOBS_VERSION}")
endif()

if(NOT TARGET OBS::libobs)
    add_library(OBS::libobs UNKNOWN IMPORTED GLOBAL)
    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_LOCATION "${LIBOBS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
    )
endif()
set(libobs_FOUND TRUE)
