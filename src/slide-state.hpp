// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#pragma once

#include <string>
#include <nlohmann/json.hpp>

struct SlideState {
    std::string slideText;
    bool        black         = false;
    bool        logo          = false;
    bool        live          = false;
    std::string resourceTitle;
    std::string citation;
    std::string slideType;   // "songs" | "bible" | "presentation" | "announcement"
    std::string imageUrl;    // ruta relativa, ej. "/api/media/xxx/page/1"
    std::string logoImageUrl; // logo de la organización, ruta relativa

    static SlideState fromJson(const nlohmann::json &j)
    {
        // j.value() lanza type_error si el campo existe pero es null JSON.
        // Esta lambda devuelve el default en ese caso.
        auto str = [&](const char *key, const char *def = "") -> std::string {
            auto it = j.find(key);
            if (it == j.end() || it->is_null()) return def;
            return it->get<std::string>();
        };
        auto boolean = [&](const char *key, bool def = false) -> bool {
            auto it = j.find(key);
            if (it == j.end() || it->is_null()) return def;
            return it->get<bool>();
        };

        SlideState s;
        s.slideText     = str("slideText");
        s.black         = boolean("black");
        s.logo          = boolean("logo");
        s.live          = boolean("live");
        s.resourceTitle = str("resourceTitle");
        s.citation      = str("citation");
        s.slideType     = str("slideType", "songs");
        s.imageUrl      = str("imageUrl");
        s.logoImageUrl  = str("logoImageUrl");
        return s;
    }

    bool isMultimedia() const
    {
        return slideType == "presentation" || slideType == "announcement";
    }
};
