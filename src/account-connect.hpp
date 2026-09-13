// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#pragma once

// "Conectar cuenta" (#42 fase 5, docs/control-remoto/analisis.md §15).
//
// Es el flujo de OAuth para aplicaciones de escritorio —loopback + PKCE, RFC 8252 y RFC 7636—:
// el plugin escucha en 127.0.0.1 en un puerto libre, abre el navegador en la página de PraiseHim, el
// usuario inicia sesión y confirma, y la página redirige a ese puerto con un código de un solo uso
// que acá se canjea por el token de la cuenta. El token nunca pasa por el navegador, y otro programa
// de la PC que vea el código no puede canjearlo sin el verifier, que no sale de este proceso.

#include <atomic>
#include <string>
#include <vector>

struct PhServicio {
    long long   id = 0;
    std::string nombre;
    bool        es_default = false;
};

struct PhCuenta {
    bool        ok = false;
    long        http_status = 0;   // 0 = no se pudo llegar al servidor
    std::string organizacion;
    std::string usuario;
    bool        obs_incluido = true;
    std::vector<PhServicio> servicios;
};

class AccountConnectFlow {
public:
    struct Resultado {
        bool        ok = false;
        std::string token;
        std::string organizacion;
        std::string usuario;
        std::string error;         // para mostrar en las propiedades
    };

    AccountConnectFlow() = default;
    ~AccountConnectFlow();
    AccountConnectFlow(const AccountConnectFlow &) = delete;
    AccountConnectFlow &operator=(const AccountConnectFlow &) = delete;

    // Abre el puerto y arma la dirección que hay que abrir en el navegador. No bloquea.
    bool preparar(const std::string &server_url, const std::string &dispositivo,
                  std::string &url_navegador, std::string &error);

    // Bloquea (va en un hilo aparte) hasta que el navegador vuelva, se cancele o venza el tiempo.
    Resultado esperar_y_canjear(const std::atomic<bool> &cancelar, int segundos = 300);

private:
    void cerrar();

    long long   sock_ = -1;
    int         port_ = 0;
    bool        wsa_ = false;
    std::string server_url_;
    std::string verifier_;
    std::string state_;
    std::string redirect_uri_;
};

// Los servicios de la organización para el selector. `http_status` 401 = la cuenta se desconectó.
PhCuenta ph_cuenta_servicios(const std::string &server_url, const std::string &token);

// "Desconectar": revoca el token en el servidor. Si no hay red no importa, se olvida igual.
void ph_cuenta_desconectar(const std::string &server_url, const std::string &token);

// Nombre con el que el dispositivo aparece en la página de conexión.
std::string ph_nombre_dispositivo();

// ── La cuenta conectada ────────────────────────────────────────
//
// Una por instalación de OBS, compartida por todas las fuentes PraiseHim, en
// plugin_config/obs-praisehim/cuenta.json — no en los ajustes de la fuente. Dos motivos: se conecta
// una sola vez aunque haya una fuente de texto y otra multimedia, y el "Cancelar" del diálogo de
// propiedades restaura los ajustes que la fuente tenía al abrirlo, así que guardada ahí la conexión
// recién hecha se perdía. Cada fuente elige su servicio, que sí es un ajuste normal.
struct PhCuentaGuardada {
    std::string server_url;        // el servidor que emitió el token
    std::string token;
    std::string organizacion;
    std::string usuario;
    bool        obs_incluido = true;
    std::vector<PhServicio> servicios;
};

PhCuentaGuardada ph_cuenta_actual();
void ph_cuenta_guardar(const PhCuentaGuardada &cuenta);
void ph_cuenta_olvidar();
