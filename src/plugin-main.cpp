// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#include <obs-module.h>
#include "praisehim-source.hpp"

// La define CMakeLists a partir de `project(... VERSION)`. El fallback existe para que un build
// hecho a mano fuera de CMake no rompa, pero deja una marca reconocible en el log en vez de mentir
// con un número que parecería válido.
#ifndef PH_VERSION
#define PH_VERSION "desconocida"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-praisehim", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "PraiseHim — fuente de presentación para iglesias";
}

bool obs_module_load(void)
{
    register_praisehim_source();
    obs_register_source(&praisehim_source_info);
    blog(LOG_INFO, "[PraiseHim] Plugin v%s cargado", PH_VERSION);
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[PraiseHim] Plugin descargado");
}
