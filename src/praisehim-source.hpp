// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#pragma once

#include <obs-module.h>

#include <QImage>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "slide-state.hpp"

class SseClient;

// ── Enumeraciones de configuración ───────────────────────────
enum SourceMode   { MODE_TEXT = 0, MODE_MULTIMEDIA = 1 };
enum TextPosition { POS_UPPER = 0, POS_MIDDLE = 1, POS_LOWER = 2, POS_CUSTOM = 3 };
enum BgType       { BG_NONE = 0, BG_SOLID = 1, BG_IMAGE = 2 };
enum OfflineType  { OFFLINE_BLACK = 0, OFFLINE_LOGO = 1 };
// Cómo se conecta al servidor: con la cuenta (fase 5 de #42) o pegando el token OBS de un servicio.
enum ConnType     { CONN_ACCOUNT = 0, CONN_TOKEN = 1 };

// ── Datos de la fuente ────────────────────────────────────────
struct PraiseHimData {

    obs_source_t *source = nullptr;

    // Conexión
    ConnType    conn_type = CONN_ACCOUNT;
    std::string server_url;   // ej. "http://localhost"
    std::string obs_token;
    long long   servicio_id = 0;   // 0 = el servicio por defecto de la organización
    // Lo que definió la conexión SSE actual: si cambia algo de esto, hay que reconectar.
    std::string conn_firma;

    // "Conectar cuenta": el hilo que espera al navegador o actualiza la lista de servicios.
    std::thread         account_thread;
    std::atomic<bool>   account_cancel{false};
    std::atomic<bool>   account_busy{false};
    std::mutex          account_msg_mutex;
    std::string         account_msg;        // último aviso para mostrar en las propiedades
    bool                account_msg_error = false;

    // Canvas
    uint32_t canvas_w = 1920;
    uint32_t canvas_h = 1080;

    // Modo
    SourceMode mode = MODE_TEXT;

    // ── Configuración modo texto ──────────────────────────────
    TextPosition text_pos    = POS_LOWER;
    float        custom_y    = 80.0f;   // porcentaje 0-100
    float        custom_x    = 50.0f;   // porcentaje centro horizontal 0-100

    int          box_w_pct   = 100;     // ancho del cuadro de texto, % del lienzo
    int          box_h_pct   = 35;      // alto del cuadro de texto, % del lienzo

    std::string  font_face;
    int          font_size   = 52;
    bool         font_bold   = false;
    bool         font_italic = false;

    uint32_t text_color    = 0xFFFFFFFFu; // ARGB
    bool     outline_on    = false;
    uint32_t outline_color = 0xFF000000u;
    int      outline_width = 2;

    // Resaltado detrás de cada línea escrita (contraste sobre fondos con imagen).
    // La opacidad arranca en 0 = transparente, o sea sin cambio respecto de no tenerlo.
    uint32_t highlight_color   = 0xFF000000u; // RGB; la opacidad se controla aparte
    int      highlight_opacity = 0;           // 0-100 %

    // Al limpiar o mandar el logo desde PraiseHim no se dibuja nada (ni texto ni
    // fondo) en vez de dejar el fondo vacío en pantalla. Solo aplica a modo texto.
    bool     auto_hide  = false;

    BgType   bg_type    = BG_SOLID;
    uint32_t bg_color   = 0xFF000000u; // color sólido (RGB; la opacidad se controla aparte)
    int      bg_opacity = 80;          // opacidad del fondo sólido, 0-100 %
    std::string bg_image_path;
    int      bg_padding = 12;          // px arriba y abajo del texto

    QImage   bg_image_loaded;          // pre-escalada al canvas

    // ── Configuración modo multimedia ─────────────────────────
    OfflineType offline_type = OFFLINE_BLACK;
    std::string logo_path;
    QImage      logo_image_loaded;

    // ── Estado en tiempo de ejecución ─────────────────────────
    SlideState  state;
    std::mutex  state_mutex;

    // ¿La fuente se está mostrando en alguna vista? OBS descarta los frames de
    // una fuente oculta, así que mientras esté en false no vale la pena renderizar.
    // Arranca en true para que el comportamiento sea el de siempre hasta que OBS
    // avise explícitamente que se ocultó.
    std::atomic<bool> visible{true};

    // URL e imagen actual (multimedia)
    std::string current_img_url;
    QImage      current_img;

    // Logo de la organización descargado del backend (cache por URL, igual que current_img)
    std::string current_logo_url;
    QImage      current_logo_img;

    // ── Hilo de render ────────────────────────────────────────
    std::thread              render_thread;
    std::mutex               work_mutex;
    std::condition_variable  work_cv;
    bool                     has_work  = false;
    bool                     shutting_down = false;

    // SSE
    std::unique_ptr<SseClient> sse;

    // ── Métodos ───────────────────────────────────────────────
    void reconnect_sse();
    void reconnect_if_changed();
    void stop_account_worker();
    void set_account_msg(const std::string &msg, bool error);
    void on_state(const SlideState &s);
    void request_render();
    void render_worker_loop();
    void render_frame();
    void render_text_frame(const SlideState &s);
    void render_multimedia_frame(const SlideState &s);
    void load_bg_image();
    void load_logo_image();
    QImage fetch_image(const std::string &full_url);
    void push_frame(QImage &&img);
};

// Función de descarga con libcurl (usada también para imágenes)
std::vector<uint8_t> curl_download(const std::string &url);

// Registro de la fuente en OBS
extern obs_source_info praisehim_source_info;
void register_praisehim_source();
