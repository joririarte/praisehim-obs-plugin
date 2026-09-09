# ─── Paso 1: cmake config oficial de OBS ─────────────────────────────────
find_package(obs-frontend-api CONFIG QUIET)
if(obs-frontend-api_FOUND)
    return()
endif()

# ─── Paso 2: fallback por plataforma ─────────────────────────────────────
if(WIN32)
    find_path(OBS_FE_INCLUDE_DIR
        NAMES obs-frontend-api.h
        PATHS
            "${OBS_SOURCE_DIR}/frontend/api"         # OBS 31+
            "${OBS_SOURCE_DIR}/libobs-frontend-api"  # OBS 30
            "${OBS_SOURCE_DIR}/UI/obs-frontend-api"  # OBS < 30
            "${OBS_SOURCE_DIR}/UI"
            "${OBS_SOURCE_DIR}/libobs"
            "${OBS_SOURCE_DIR}"
            "${OBS_DIR}/include"
        NO_DEFAULT_PATH
    )
    find_library(OBS_FE_LIBRARY
        NAMES obs-frontend-api
        PATHS
            "${OBS_DIR}/bin/64bit"
            "${OBS_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT OBS_FE_INCLUDE_DIR)
        message(FATAL_ERROR
            "obs-frontend-api.h no encontrado.\n"
            "Pasá -DOBS_SOURCE_DIR=<ruta/obs-studio> con la fuente de OBS.")
    endif()
    if(NOT OBS_FE_LIBRARY)
        message(FATAL_ERROR
            "obs-frontend-api.lib no encontrado en OBS_DIR='${OBS_DIR}'.")
    endif()
else()
    # Linux: buscar directamente (headers están junto con libobs)
    find_library(OBS_FE_LIBRARY
        NAMES obs-frontend-api
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
    )
    # Los headers de obs-frontend-api están en el mismo dir que libobs
    get_target_property(OBS_FE_INCLUDE_DIR OBS::libobs INTERFACE_INCLUDE_DIRECTORIES)

    if(NOT OBS_FE_LIBRARY)
        message(FATAL_ERROR "obs-frontend-api no encontrada. Instalá libobs-dev.")
    endif()
endif()

if(NOT TARGET OBS::obs-frontend-api)
    add_library(OBS::obs-frontend-api UNKNOWN IMPORTED GLOBAL)
    set_target_properties(OBS::obs-frontend-api PROPERTIES
        IMPORTED_LOCATION "${OBS_FE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_FE_INCLUDE_DIR}"
    )
endif()
set(obs-frontend-api_FOUND TRUE)
