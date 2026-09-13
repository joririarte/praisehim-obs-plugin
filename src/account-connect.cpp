// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

// Los sockets van primero: en Windows, winsock2.h tiene que entrar antes que cualquier windows.h que
// traigan los headers de OBS o de Qt.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using ph_socket = SOCKET;
static const ph_socket PH_SIN_SOCKET = INVALID_SOCKET;
static void ph_cerrar_socket(ph_socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using ph_socket = int;
static const ph_socket PH_SIN_SOCKET = -1;
static void ph_cerrar_socket(ph_socket s) { close(s); }
#endif

#include "account-connect.hpp"

#ifndef PH_VERSION
#define PH_VERSION "desconocida"
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QString>
#include <QSysInfo>
#include <QUrl>

#include <chrono>
#include <cstring>
#include <mutex>


// ═════════════════════════════════════════════════════════════
//  Utilidades
// ═════════════════════════════════════════════════════════════

static std::string base64url(const QByteArray &bytes)
{
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals).toStdString();
}

static std::string aleatorio(int n)
{
    QByteArray bytes(n, 0);
    for (int i = 0; i < n; ++i)
        bytes[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return base64url(bytes);
}

static std::string codificar(const std::string &s)
{
    return QUrl::toPercentEncoding(QString::fromStdString(s)).toStdString();
}

static std::string decodificar(const std::string &s)
{
    return QUrl::fromPercentEncoding(QByteArray::fromStdString(s)).toStdString();
}

static std::string sin_barra_final(std::string url)
{
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::string ph_nombre_dispositivo()
{
    QString host = QSysInfo::machineHostName();
    return "OBS Studio" + (host.isEmpty() ? std::string() : " · " + host.toStdString());
}

static size_t curl_str_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    static_cast<std::string *>(ud)->append(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

// Un pedido JSON corto. Devuelve el status HTTP, o 0 si no se pudo llegar.
static long http_json(const char *metodo, const std::string &url, const std::string &cuerpo,
                      const std::string &token, std::string &respuesta)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!cuerpo.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth;
    if (!token.empty()) {
        auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,  metodo);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "obs-praisehim/" PH_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_str_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &respuesta);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        20L);
    if (!cuerpo.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    cuerpo.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)cuerpo.size());
    }

    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

// ═════════════════════════════════════════════════════════════
//  Flujo de conexión
// ═════════════════════════════════════════════════════════════

AccountConnectFlow::~AccountConnectFlow()
{
    cerrar();
}

void AccountConnectFlow::cerrar()
{
    if (sock_ != -1) {
        ph_cerrar_socket(static_cast<ph_socket>(sock_));
        sock_ = -1;
    }
#ifdef _WIN32
    if (wsa_) {
        WSACleanup();
        wsa_ = false;
    }
#endif
}

bool AccountConnectFlow::preparar(const std::string &server_url, const std::string &dispositivo,
                                  std::string &url_navegador, std::string &error)
{
    server_url_ = sin_barra_final(server_url);
    if (server_url_.empty()) {
        error = "Falta la URL del servidor";
        return false;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "No se pudo abrir la conexión local";
        return false;
    }
    wsa_ = true;
#endif

    ph_socket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == PH_SIN_SOCKET) {
        error = "No se pudo abrir la conexión local";
        return false;
    }
    sock_ = static_cast<long long>(s);

    // Solo 127.0.0.1 y un puerto que elige el sistema: nadie de la red puede contestarle al plugin,
    // y no hay un puerto fijo que otro programa pueda tener ocupado.
    sockaddr_in dir{};
    dir.sin_family      = AF_INET;
    dir.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dir.sin_port        = 0;
    if (bind(s, reinterpret_cast<sockaddr *>(&dir), sizeof(dir)) != 0 || listen(s, 4) != 0) {
        error = "No se pudo abrir la conexión local";
        cerrar();
        return false;
    }
    socklen_t largo = sizeof(dir);
    if (getsockname(s, reinterpret_cast<sockaddr *>(&dir), &largo) != 0) {
        error = "No se pudo abrir la conexión local";
        cerrar();
        return false;
    }
    port_ = ntohs(dir.sin_port);

    verifier_     = aleatorio(48);   // 64 caracteres: dentro de los 43-128 que pide PKCE
    state_        = aleatorio(24);
    redirect_uri_ = "http://127.0.0.1:" + std::to_string(port_) + "/callback";
    QByteArray hash = QCryptographicHash::hash(QByteArray::fromStdString(verifier_),
                                               QCryptographicHash::Sha256);

    url_navegador = server_url_ + "/obs/conectar"
        + "?redirect_uri="          + codificar(redirect_uri_)
        + "&state="                 + state_
        + "&code_challenge="        + base64url(hash)
        + "&code_challenge_method=S256"
        + "&dispositivo="           + codificar(dispositivo);
    return true;
}

// Lo que ve el navegador al volver. Autocontenida: nada remoto, y el usuario solo tiene que volver a OBS.
static std::string pagina(bool ok, const std::string &mensaje)
{
    std::string html =
        "<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\"><title>PraiseHim</title>"
        "<style>body{font-family:system-ui,sans-serif;background:#f3f1f8;color:#1b1730;display:grid;"
        "place-items:center;min-height:100vh;margin:0}div{background:#fff;border-radius:20px;padding:32px 36px;"
        "max-width:420px;box-shadow:0 20px 60px rgba(60,50,100,.15)}h1{font-size:1.25rem;margin:0 0 8px}"
        "p{margin:0;line-height:1.45;color:#4a4560}</style></head><body><div><h1>";
    html += ok ? "OBS Studio quedó conectado" : "No se pudo conectar OBS Studio";
    html += "</h1><p>" + mensaje + "</p></div></body></html>";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
           "Cache-Control: no-store\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
}

static void enviar(ph_socket c, const std::string &s)
{
    size_t enviado = 0;
    while (enviado < s.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;   // que un navegador que cerró no mate a OBS con SIGPIPE
#else
        const int flags = 0;
#endif
        auto n = send(c, s.data() + enviado, (int)(s.size() - enviado), flags);
        if (n <= 0) break;
        enviado += (size_t)n;
    }
}

// Espera hasta `ms` a que el socket tenga algo para leer.
static bool listo_para_leer(ph_socket s, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select((int)s + 1, &set, nullptr, nullptr, &tv) > 0;
}

AccountConnectFlow::Resultado AccountConnectFlow::esperar_y_canjear(const std::atomic<bool> &cancelar, int segundos)
{
    Resultado r;
    if (sock_ == -1) {
        r.error = "No se pudo abrir la conexión local";
        return r;
    }
    ph_socket s = static_cast<ph_socket>(sock_);
    auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(segundos);

    while (!cancelar.load()) {
        if (std::chrono::steady_clock::now() > limite) {
            r.error = "Se agotó el tiempo esperando la confirmación en el navegador";
            break;
        }
        if (!listo_para_leer(s, 500)) continue;

        ph_socket c = accept(s, nullptr, nullptr);
        if (c == PH_SIN_SOCKET) continue;

        // Solo la primera línea del pedido importa: "GET /callback?code=...&state=... HTTP/1.1".
        std::string pedido;
        char buf[2048];
        auto hasta = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (pedido.find("\r\n") == std::string::npos && pedido.size() < 8192
               && std::chrono::steady_clock::now() < hasta) {
            if (!listo_para_leer(c, 250)) continue;
            auto n = recv(c, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
            pedido.append(buf, (size_t)n);
        }

        std::string linea = pedido.substr(0, pedido.find("\r\n"));
        std::string ruta;
        if (linea.rfind("GET ", 0) == 0) {
            auto fin = linea.find(' ', 4);
            ruta = linea.substr(4, fin == std::string::npos ? std::string::npos : fin - 4);
        }
        if (ruta.rfind("/callback", 0) != 0) {
            // El navegador también pide /favicon.ico: se contesta y se sigue esperando.
            enviar(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            ph_cerrar_socket(c);
            continue;
        }

        std::string code, state, error;
        auto q = ruta.find('?');
        if (q != std::string::npos) {
            std::string query = ruta.substr(q + 1);
            size_t pos = 0;
            while (pos <= query.size()) {
                auto amp = query.find('&', pos);
                std::string par = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                auto eq = par.find('=');
                std::string k = par.substr(0, eq);
                std::string v = eq == std::string::npos ? "" : decodificar(par.substr(eq + 1));
                if (k == "code") code = v;
                else if (k == "state") state = v;
                else if (k == "error") error = v;
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }

        // Un `state` distinto no es de esta conexión: puede ser un pedido armado por otro. No se
        // canjea nada y se sigue esperando el verdadero.
        if (state != state_) {
            enviar(c, pagina(false, "Este enlace no corresponde a la conexión que se abrió desde OBS."));
            ph_cerrar_socket(c);
            continue;
        }

        if (!error.empty() || code.empty()) {
            r.error = "Se canceló la conexión en el navegador";
            enviar(c, pagina(false, "Cancelaste la conexión. Podés cerrar esta pestaña."));
            ph_cerrar_socket(c);
            break;
        }

        nlohmann::json cuerpo = {{"code", code}, {"codeVerifier", verifier_}, {"redirectUri", redirect_uri_}};
        std::string resp;
        long status = http_json("POST", server_url_ + "/api/obs/cuenta/token", cuerpo.dump(), "", resp);
        if (status == 200) {
            try {
                auto j = nlohmann::json::parse(resp);
                r.token        = j.value("token", "");
                r.organizacion = j["organizacion"].is_string() ? j["organizacion"].get<std::string>() : "";
                r.usuario      = j["usuario"].is_string() ? j["usuario"].get<std::string>() : "";
                r.ok           = !r.token.empty();
            } catch (...) {
                r.ok = false;
            }
        }
        if (r.ok) {
            enviar(c, pagina(true, "Volvé a OBS Studio y elegí el servicio en las propiedades de la fuente. "
                                   "Podés cerrar esta pestaña."));
        } else {
            r.error = status == 0 ? "No se pudo llegar al servidor para terminar la conexión"
                                  : "El servidor rechazó la conexión (" + std::to_string(status) + ")";
            enviar(c, pagina(false, "Volvé a OBS Studio y probá de nuevo con \"Conectar cuenta\"."));
        }
        ph_cerrar_socket(c);
        break;
    }

    if (cancelar.load() && r.error.empty() && !r.ok) r.error = "Conexión cancelada";
    cerrar();
    return r;
}

// ═════════════════════════════════════════════════════════════
//  Servicios y desconexión
// ═════════════════════════════════════════════════════════════

PhCuenta ph_cuenta_servicios(const std::string &server_url, const std::string &token)
{
    PhCuenta c;
    std::string resp;
    c.http_status = http_json("GET", sin_barra_final(server_url) + "/api/obs/cuenta/servicios", "", token, resp);
    if (c.http_status != 200) return c;
    try {
        auto j = nlohmann::json::parse(resp);
        auto str = [](const nlohmann::json &o, const char *k) {
            return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
        };
        c.organizacion = str(j, "organizacion");
        c.usuario      = str(j, "usuario");
        c.obs_incluido = !j.contains("obsIncluido") || !j["obsIncluido"].is_boolean() || j["obsIncluido"].get<bool>();
        if (j.contains("servicios") && j["servicios"].is_array()) {
            for (auto &s : j["servicios"]) {
                PhServicio sv;
                sv.id         = s.contains("id") && s["id"].is_number_integer() ? s["id"].get<long long>() : 0;
                sv.nombre     = str(s, "nombre");
                sv.es_default = s.contains("esDefault") && s["esDefault"].is_boolean() && s["esDefault"].get<bool>();
                if (sv.id > 0) c.servicios.push_back(sv);
            }
        }
        c.ok = true;
    } catch (...) {
        c.ok = false;
    }
    return c;
}

void ph_cuenta_desconectar(const std::string &server_url, const std::string &token)
{
    std::string resp;
    http_json("POST", sin_barra_final(server_url) + "/api/obs/cuenta/desconectar", "{}", token, resp);
}

// ═════════════════════════════════════════════════════════════
//  Cuenta guardada
// ═════════════════════════════════════════════════════════════

static std::mutex       g_cuenta_mutex;
static bool             g_cuenta_cargada = false;
static PhCuentaGuardada g_cuenta;

static std::string ruta_cuenta(bool crear_directorio)
{
    char *ruta = obs_module_config_path("cuenta.json");
    if (!ruta) return {};
    std::string r = ruta;
    bfree(ruta);
    if (crear_directorio) {
        char *dir = obs_module_config_path("");
        if (dir) {
            os_mkdirs(dir);
            bfree(dir);
        }
    }
    return r;
}

PhCuentaGuardada ph_cuenta_actual()
{
    std::lock_guard<std::mutex> lk(g_cuenta_mutex);
    if (g_cuenta_cargada) return g_cuenta;
    g_cuenta_cargada = true;

    std::string ruta = ruta_cuenta(false);
    char *txt = ruta.empty() ? nullptr : os_quick_read_utf8_file(ruta.c_str());
    if (!txt) return g_cuenta;
    try {
        auto j = nlohmann::json::parse(txt);
        auto str = [&](const char *k) {
            return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
        };
        g_cuenta.server_url   = str("serverUrl");
        g_cuenta.token        = str("token");
        g_cuenta.organizacion = str("organizacion");
        g_cuenta.usuario      = str("usuario");
        g_cuenta.obs_incluido = !j.contains("obsIncluido") || !j["obsIncluido"].is_boolean() || j["obsIncluido"].get<bool>();
        if (j.contains("servicios") && j["servicios"].is_array()) {
            for (auto &s : j["servicios"]) {
                PhServicio sv;
                sv.id         = s.value("id", 0LL);
                sv.nombre     = s.contains("nombre") && s["nombre"].is_string() ? s["nombre"].get<std::string>() : "";
                sv.es_default = s.value("esDefault", false);
                if (sv.id > 0) g_cuenta.servicios.push_back(sv);
            }
        }
    } catch (...) {
        blog(LOG_WARNING, "[PraiseHim] cuenta.json ilegible: se ignora");
        g_cuenta = PhCuentaGuardada{};
    }
    bfree(txt);
    return g_cuenta;
}

void ph_cuenta_guardar(const PhCuentaGuardada &cuenta)
{
    nlohmann::json servicios = nlohmann::json::array();
    for (auto &s : cuenta.servicios)
        servicios.push_back({{"id", s.id}, {"nombre", s.nombre}, {"esDefault", s.es_default}});
    nlohmann::json j = {
        {"serverUrl", cuenta.server_url}, {"token", cuenta.token},
        {"organizacion", cuenta.organizacion}, {"usuario", cuenta.usuario},
        {"obsIncluido", cuenta.obs_incluido}, {"servicios", servicios},
    };
    std::string txt = j.dump(2);

    std::lock_guard<std::mutex> lk(g_cuenta_mutex);
    g_cuenta = cuenta;
    g_cuenta_cargada = true;
    std::string ruta = ruta_cuenta(true);
    if (ruta.empty() || !os_quick_write_utf8_file_safe(ruta.c_str(), txt.c_str(), txt.size(), false, "tmp", nullptr))
        blog(LOG_WARNING, "[PraiseHim] No se pudo guardar la cuenta conectada");
}

void ph_cuenta_olvidar()
{
    ph_cuenta_guardar(PhCuentaGuardada{});
}
