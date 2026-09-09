// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#include "praisehim-source.hpp"
#include "sse-client.hpp"

// La define CMakeLists desde `project(... VERSION)`; ver plugin-main.cpp.
#ifndef PH_VERSION
#define PH_VERSION "desconocida"
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <curl/curl.h>
#include <stb_image.h>

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QString>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <cstring>

// ═════════════════════════════════════════════════════════════
//  Utilidades de descarga con libcurl
// ═════════════════════════════════════════════════════════════

static size_t curl_buf_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    auto *buf = static_cast<std::vector<uint8_t> *>(ud);
    const uint8_t *p = static_cast<const uint8_t *>(ptr);
    buf->insert(buf->end(), p, p + size * nmemb);
    return size * nmemb;
}

std::vector<uint8_t> curl_download(const std::string &url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::vector<uint8_t> buf;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_buf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return buf;
}

// ═════════════════════════════════════════════════════════════
//  Carga de imágenes auxiliares
// ═════════════════════════════════════════════════════════════

static QImage load_local_image(const std::string &path, int target_w, int target_h)
{
    if (path.empty()) return {};
    QImage img(QString::fromStdString(path));
    if (img.isNull()) return {};
    return img.scaled(target_w, target_h,
                      Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
               .copy(0, 0, target_w, target_h);
}

void PraiseHimData::load_bg_image()
{
    bg_image_loaded = load_local_image(bg_image_path, (int)canvas_w, (int)canvas_h);
}

void PraiseHimData::load_logo_image()
{
    logo_image_loaded = load_local_image(logo_path, (int)canvas_w, (int)canvas_h);
}

QImage PraiseHimData::fetch_image(const std::string &full_url)
{
    auto raw = curl_download(full_url);
    if (raw.empty()) return {};

    int w, h, ch;
    unsigned char *data = stbi_load_from_memory(
        raw.data(), (int)raw.size(), &w, &h, &ch, 4);
    if (!data) return {};

    // stb da RGBA; QImage::Format_RGBA8888 tiene bytes R,G,B,A en memoria
    QImage tmp(data, w, h, QImage::Format_RGBA8888);
    QImage copy = tmp.copy();   // copia profunda antes de liberar
    stbi_image_free(data);

    // Escalar con letterbox al canvas (fondo negro)
    QImage canvas((int)canvas_w, (int)canvas_h, QImage::Format_ARGB32);
    canvas.fill(Qt::black);
    QImage scaled = copy.scaled((int)canvas_w, (int)canvas_h,
                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&canvas);
    int ox = ((int)canvas_w - scaled.width())  / 2;
    int oy = ((int)canvas_h - scaled.height()) / 2;
    p.drawImage(ox, oy, scaled);
    p.end();
    return canvas;
}

// ═════════════════════════════════════════════════════════════
//  Envío de frame al buffer compartido
// ═════════════════════════════════════════════════════════════

void PraiseHimData::push_frame(QImage &&img)
{
    if (img.isNull()) {
        blog(LOG_WARNING, "[PraiseHim] push_frame: imagen nula, ignorando");
        return;
    }

    // OBS_SOURCE_ASYNC_VIDEO: entregamos el frame raw directamente
    QImage bgra = img.convertToFormat(QImage::Format_ARGB32);
    blog(LOG_INFO, "[PraiseHim] push_frame: %dx%d", bgra.width(), bgra.height());

    obs_source_frame frame = {};
    frame.format     = VIDEO_FORMAT_BGRA;
    frame.width      = (uint32_t)bgra.width();
    frame.height     = (uint32_t)bgra.height();
    frame.timestamp  = os_gettime_ns();
    frame.data[0]    = bgra.bits();
    frame.linesize[0]= (uint32_t)bgra.bytesPerLine();
    obs_source_output_video(source, &frame);
}

// ═════════════════════════════════════════════════════════════
//  Conversión de color OBS → QColor
// ═════════════════════════════════════════════════════════════
// OBS almacena los colores del selector en orden 0xAABBGGRR
// (bytes R,G,B,A en memoria). QColor::fromRgba espera 0xAARRGGBB
// (ARGB), por lo que interpretar directamente intercambia R y B
// (el azul se vería rojo). Extraemos cada canal explícitamente.
static inline QColor obs_to_qcolor(uint32_t c)
{
    return QColor((int)( c        & 0xFF),   // R
                  (int)((c >>  8) & 0xFF),   // G
                  (int)((c >> 16) & 0xFF),   // B
                  (int)((c >> 24) & 0xFF));  // A
}

// El color del resaltado se elige sin canal alfa (igual que el fondo sólido):
// la transparencia sale del slider de opacidad, y en 0 no se dibuja nada.
static inline QColor highlight_qcolor(uint32_t color, int opacity)
{
    QColor c = obs_to_qcolor(color);
    c.setAlpha(std::clamp(opacity, 0, 100) * 255 / 100);
    return c;
}

// ═════════════════════════════════════════════════════════════
//  Layout del texto (necesario para el resaltado por línea)
// ═════════════════════════════════════════════════════════════
// QPainter::drawText() no expone dónde quedó cada línea, así que el wrap se
// hace a mano con QTextLayout: el mismo layout que se mide es el que se dibuja,
// y con él se conoce el rectángulo exacto de cada línea para pintarle el fondo.
// Un layout por párrafo, porque QTextLayout no corta en '\n'.
struct WrappedText {
    std::vector<std::unique_ptr<QTextLayout>> paragraphs;
    qreal height = 0;
};

static WrappedText wrap_text(const QFont &font, const QString &text,
                             qreal width, Qt::Alignment align)
{
    WrappedText out;
    QTextOption opt(align);
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    qreal y = 0;
    const QStringList paragraphs = text.split('\n');
    for (const QString &p : paragraphs) {
        auto layout = std::make_unique<QTextLayout>(p, font);
        layout->setTextOption(opt);
        layout->beginLayout();
        while (true) {
            QTextLine line = layout->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout->endLayout();
        out.paragraphs.push_back(std::move(layout));
    }
    out.height = y;
    return out;
}

static void draw_wrapped(QPainter &p, const WrappedText &wt,
                         const QPointF &origin, const QColor &color)
{
    p.setPen(color);
    for (const auto &layout : wt.paragraphs)
        layout->draw(&p, origin);
}

// Fondo por línea: se usa el ancho natural de cada línea (no el del cuadro),
// que es lo que da el efecto de resaltado siguiendo la letra.
static void fill_line_highlights(QPainter &p, const WrappedText &wt,
                                 const QPointF &origin, const QColor &color,
                                 qreal pad_x, qreal pad_y, qreal radius)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (const auto &layout : wt.paragraphs) {
        for (int i = 0; i < layout->lineCount(); ++i) {
            QRectF r = layout->lineAt(i).naturalTextRect().translated(origin);
            if (r.width() < 1.0) continue;   // renglón en blanco: nada que resaltar
            r.adjust(-pad_x, -pad_y, pad_x, pad_y);
            p.drawRoundedRect(r, radius, radius);
        }
    }
    p.setBrush(Qt::NoBrush);
}

// ═════════════════════════════════════════════════════════════
//  Renderizado modo texto
// ═════════════════════════════════════════════════════════════

void PraiseHimData::render_text_frame(const SlideState &s)
{
    const int W = (int)canvas_w;
    const int H = (int)canvas_h;
    const int H_PAD = 48; // margen horizontal interior del cuadro

    QImage frame(W, H, QImage::Format_ARGB32);
    frame.fill(Qt::transparent);

    QString qtext = QString::fromStdString(s.slideText);

    // En canciones ignoramos los saltos de línea que envía PraiseHim:
    // el texto fluye y se ajusta al cuadro. En biblia se respetan.
    if (s.slideType == "songs") {
        qtext.replace('\n', ' ');
        qtext = qtext.simplified();   // colapsa espacios y recorta
    }

    // Con el logo activo PraiseHim sigue mandando el texto del slide, pero acá no va:
    // mandar el logo limpia el texto igual que limpiar.
    const bool has_text = s.live && !s.black && !s.logo && !qtext.isEmpty();

    // "Ocultar automáticamente": sin texto no se dibuja nada, ni siquiera el fondo.
    // El frame transparente es lo que hace desaparecer el render sin ocultar la fuente.
    if (auto_hide && !has_text) {
        push_frame(std::move(frame));
        return;
    }

    QPainter painter(&frame);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // ── Geometría del cuadro fijo ─────────────────────────────
    // Tamaño configurable (% del lienzo); la posición vertical
    // configurada define el CENTRO del cuadro. Centrado horizontal.
    int box_w = std::clamp(box_w_pct, 10, 100) * W / 100;
    int box_h = std::min(H, std::clamp(box_h_pct, 5, 100) * H / 100);
    int box_x = (W - box_w) / 2;

    int y_center;
    switch (text_pos) {
    case POS_UPPER:  y_center = (int)(H * 0.18f); break;
    case POS_MIDDLE: y_center = H / 2;             break;
    case POS_LOWER:  y_center = (int)(H * 0.82f); break;
    case POS_CUSTOM: y_center = (int)(H * custom_y / 100.0f); break;
    }
    int box_top = std::clamp(y_center - box_h / 2, 0, H - box_h);
    int box_bot = box_top + box_h;

    // ── Fondo (tamaño fijo del cuadro) ────────────────────────
    if (bg_type == BG_SOLID) {
        QColor c = obs_to_qcolor(bg_color);
        c.setAlpha(std::clamp(bg_opacity, 0, 100) * 255 / 100);
        painter.fillRect(box_x, box_top, box_w, box_h, c);
    } else if (bg_type == BG_IMAGE && !bg_image_loaded.isNull()) {
        // Mostrar solo la franja de la imagen pre-escalada bajo el cuadro
        painter.drawImage(QRect(box_x, box_top, box_w, box_h),
                          bg_image_loaded,
                          QRect(box_x, box_top, box_w, box_h));
    }
    // BG_NONE → transparente (no se dibuja nada)

    // Sin texto (limpiar, logo, o nada en vivo) queda solo el fondo
    if (!has_text) {
        painter.end();
        push_frame(std::move(frame));
        return;
    }

    // ── Área útil del texto dentro del cuadro ─────────────────
    const int inner_x = box_x + H_PAD;
    const int inner_w = std::max(1, box_w - 2 * H_PAD);

    QFont font(font_face.empty() ? "Sans Serif" : QString::fromStdString(font_face));
    font.setBold(font_bold);
    font.setItalic(font_italic);

    // Espacio reservado para la citación (biblia). Se dimensiona con el
    // tamaño configurado (máximo) para no subestimar el espacio.
    QString qcit = QString::fromStdString(s.citation);
    QFont cit_font = font;
    cit_font.setPointSize(std::max(12, font_size * 3 / 4));
    int cit_h = qcit.isEmpty() ? 0 : QFontMetrics(cit_font).height() + 8;

    const int inner_top = box_top + bg_padding;
    const int avail_h   = std::max(1, box_h - 2 * bg_padding - cit_h);

    // ── Auto-ajuste: mayor tamaño ≤ configurado que quepa ─────
    const int MIN_PT = 12;
    int fit_size = std::max(MIN_PT, font_size);
    WrappedText wrapped;
    while (true) {
        font.setPointSize(fit_size);
        wrapped = wrap_text(font, qtext, inner_w, Qt::AlignHCenter);
        if (wrapped.height <= avail_h || fit_size <= MIN_PT) break;
        --fit_size;
    }

    // Centrado vertical del bloque dentro del área útil
    int text_top = inner_top + std::max(0, (int)(avail_h - wrapped.height) / 2);
    const QPointF origin(inner_x, text_top);

    // ── Resaltado por línea (debajo del texto y del borde) ────
    const QColor hl = highlight_qcolor(highlight_color, highlight_opacity);
    if (hl.alpha() > 0)
        fill_line_highlights(painter, wrapped, origin, hl,
                             fit_size * 0.30, fit_size * 0.08, fit_size * 0.12);

    // ── Texto con outline opcional ────────────────────────────
    if (outline_on && outline_width > 0) {
        const QColor oc = obs_to_qcolor(outline_color);
        for (int dy = -outline_width; dy <= outline_width; ++dy) {
            for (int dx = -outline_width; dx <= outline_width; ++dx) {
                if (dx == 0 && dy == 0) continue;
                draw_wrapped(painter, wrapped, origin + QPointF(dx, dy), oc);
            }
        }
    }

    draw_wrapped(painter, wrapped, origin, obs_to_qcolor(text_color));

    // ── Citación (esquina inferior del cuadro) ────────────────
    if (!qcit.isEmpty()) {
        cit_font.setItalic(true);
        painter.setFont(cit_font);
        QRect cit_rect(inner_x, box_bot - bg_padding - cit_h, inner_w, cit_h);

        // La cita comparte el resaltado del texto: es una sola línea, así que
        // alcanza con el rectángulo que ocupa dentro de su caja.
        if (hl.alpha() > 0) {
            const int cit_pt = cit_font.pointSize();
            QRectF r = QFontMetrics(cit_font).boundingRect(
                cit_rect, Qt::AlignRight | Qt::AlignVCenter, qcit);
            r.adjust(-cit_pt * 0.30, -cit_pt * 0.08, cit_pt * 0.30, cit_pt * 0.08);
            painter.setPen(Qt::NoPen);
            painter.setBrush(hl);
            painter.drawRoundedRect(r, cit_pt * 0.12, cit_pt * 0.12);
            painter.setBrush(Qt::NoBrush);
        }

        if (outline_on && outline_width > 0) {
            painter.setPen(obs_to_qcolor(outline_color));
            for (int dy = -outline_width; dy <= outline_width; ++dy) {
                for (int dx = -outline_width; dx <= outline_width; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    painter.drawText(cit_rect.translated(dx, dy),
                                     Qt::AlignRight | Qt::AlignVCenter, qcit);
                }
            }
        }

        painter.setPen(obs_to_qcolor(text_color));
        painter.drawText(cit_rect, Qt::AlignRight | Qt::AlignVCenter, qcit);
    }

    painter.end();
    push_frame(std::move(frame));
}

// ═════════════════════════════════════════════════════════════
//  Renderizado modo multimedia
// ═════════════════════════════════════════════════════════════

void PraiseHimData::render_multimedia_frame(const SlideState &s)
{
    const int W = (int)canvas_w;
    const int H = (int)canvas_h;

    // El logo de PraiseHim gana sobre la imagen del slide: cuando el presentador lo
    // manda, sale al aire el logo de la organización (el mismo que ven las pantallas),
    // no el archivo local configurado en el plugin — ese queda para "sin contenido".
    if (s.logo && !s.logoImageUrl.empty()) {
        std::string full_url = server_url + s.logoImageUrl;
        if (full_url != current_logo_url) {
            QImage img = fetch_image(full_url);
            if (!img.isNull()) {
                current_logo_img = img;
                current_logo_url = full_url;
            }
        }
        if (!current_logo_img.isNull()) {
            push_frame(QImage(current_logo_img)); // copia
            return;
        }
        // Sin logo descargable cae al fallback de abajo (negro o logo local)
    }

    // Decidir qué mostrar
    if (s.live && !s.black && !s.logo && !s.imageUrl.empty()) {
        // Cargar imagen si cambió la URL
        std::string full_url = server_url + s.imageUrl;
        if (full_url != current_img_url) {
            QImage img = fetch_image(full_url);
            if (!img.isNull()) {
                current_img     = img;
                current_img_url = full_url;
            }
        }
        if (!current_img.isNull()) {
            push_frame(QImage(current_img)); // copia
            return;
        }
    }

    // Fallback: negro o logo
    if (offline_type == OFFLINE_LOGO && !logo_image_loaded.isNull()) {
        push_frame(QImage(logo_image_loaded));
        return;
    }

    // Negro
    QImage black(W, H, QImage::Format_ARGB32);
    black.fill(Qt::black);
    push_frame(std::move(black));
}

// ═════════════════════════════════════════════════════════════
//  Hilo de render
// ═════════════════════════════════════════════════════════════

void PraiseHimData::render_frame()
{
    // Con la fuente oculta OBS descarta los frames que entregamos, así que
    // renderizar sería tirar CPU —y, en multimedia, ancho de banda bajando
    // imágenes que nadie ve—. El estado sigue llegando por SSE y quedando en
    // `state`; al volver a mostrarse, ph_show pide el render con lo último.
    if (!visible.load()) return;

    SlideState snap;
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        snap = state;
    }

    if (mode == MODE_TEXT)
        render_text_frame(snap);
    else
        render_multimedia_frame(snap);
}

void PraiseHimData::render_worker_loop()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lk(work_mutex);
            work_cv.wait(lk, [this] { return has_work || shutting_down; });
            if (shutting_down) break;
            has_work = false;
        }
        try {
            render_frame();
        } catch (const std::exception &e) {
            blog(LOG_ERROR, "[PraiseHim] render_frame excepción: %s", e.what());
        } catch (...) {
            blog(LOG_ERROR, "[PraiseHim] render_frame excepción desconocida");
        }
    }
}

void PraiseHimData::on_state(const SlideState &s)
{
    blog(LOG_INFO, "[PraiseHim] on_state: live=%d black=%d type='%s' text='%.40s'",
         s.live, s.black, s.slideType.c_str(), s.slideText.c_str());
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        state = s;
    }
    request_render();
}

// Despierta al hilo de render para que reemita un frame con el estado actual.
void PraiseHimData::request_render()
{
    {
        std::lock_guard<std::mutex> lk(work_mutex);
        has_work = true;
    }
    work_cv.notify_one();
}

// ═════════════════════════════════════════════════════════════
//  Reconexión SSE
// ═════════════════════════════════════════════════════════════

void PraiseHimData::reconnect_sse()
{
    if (sse) {
        sse->stop();
        sse.reset();
    }

    if (server_url.empty() || obs_token.empty()) {
        blog(LOG_INFO, "[PraiseHim] reconnect_sse: url='%s' token='%s' — saltando (vacío)",
             server_url.c_str(), obs_token.c_str());
        return;
    }

    // La versión viaja en la URL y no en un header: el header obligaría a tocar el CORS del
    // controlador y la config de nginx, y esto es un dato de diagnóstico, no una credencial. El
    // servidor la usa para avisarle al presentador que su plugin quedó viejo.
    std::string url = server_url + "/api/obs/state?token=" + obs_token
                    + "&v=" + PH_VERSION;
    blog(LOG_INFO, "[PraiseHim] Conectando SSE a: %s", url.c_str());
    sse = std::make_unique<SseClient>(
        url,
        [this](const SlideState &s) { on_state(s); }
    );
    sse->start();
}

// ═════════════════════════════════════════════════════════════
//  Callbacks de obs_source_info
// ═════════════════════════════════════════════════════════════

static const char *ph_get_name(void *)
{
    return obs_module_text("PraiseHimSource");
}

static uint32_t ph_get_width(void *data)
{
    return static_cast<PraiseHimData *>(data)->canvas_w;
}

static uint32_t ph_get_height(void *data)
{
    return static_cast<PraiseHimData *>(data)->canvas_h;
}

// ── Valores por defecto ───────────────────────────────────────
static void ph_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int   (settings, "mode",          MODE_TEXT);
    obs_data_set_default_string(settings, "server_url",    "http://localhost");
    obs_data_set_default_string(settings, "obs_token",     "");
    obs_data_set_default_int   (settings, "canvas_w",      1920);
    obs_data_set_default_int   (settings, "canvas_h",      1080);

    // Texto
    obs_data_set_default_int   (settings, "text_pos",      POS_LOWER);
    obs_data_set_default_double(settings, "custom_y",      80.0);
    obs_data_set_default_int   (settings, "box_w_pct",     100);
    obs_data_set_default_int   (settings, "box_h_pct",     35);
    obs_data_set_default_string(settings, "font_face",     "");
    obs_data_set_default_int   (settings, "font_size",     52);
    obs_data_set_default_bool  (settings, "font_bold",     false);
    obs_data_set_default_bool  (settings, "font_italic",   false);
    obs_data_set_default_int   (settings, "text_color",    (int64_t)0xFFFFFFFFu);
    obs_data_set_default_int   (settings, "highlight_color",   (int64_t)0xFF000000u);
    obs_data_set_default_int   (settings, "highlight_opacity", 0);
    obs_data_set_default_bool  (settings, "outline_on",    false);
    obs_data_set_default_int   (settings, "outline_color", (int64_t)0xFF000000u);
    obs_data_set_default_int   (settings, "outline_width", 2);
    obs_data_set_default_int   (settings, "bg_type",       BG_SOLID);
    obs_data_set_default_int   (settings, "bg_color",      (int64_t)0xFF000000u);
    obs_data_set_default_int   (settings, "bg_opacity",    80);
    obs_data_set_default_string(settings, "bg_image_path", "");
    obs_data_set_default_int   (settings, "bg_padding",    12);
    obs_data_set_default_bool  (settings, "auto_hide",     false);

    // Multimedia
    obs_data_set_default_int   (settings, "offline_type",  OFFLINE_BLACK);
    obs_data_set_default_string(settings, "logo_path",     "");
}

// Sube al obs_properties_t raíz. Los modified_callback reciben las
// propiedades del grupo donde vive el control; desde la raíz,
// obs_properties_get busca recursivamente dentro de todos los grupos,
// así que normalizando aquí un solo callback sirve para toda la lista.
static obs_properties_t *root_props(obs_properties_t *props)
{
    obs_properties_t *parent;
    while ((parent = obs_properties_get_parent(props)) != nullptr)
        props = parent;
    return props;
}

static inline void set_vis(obs_properties_t *root, const char *name, bool v)
{
    obs_property_set_visible(obs_properties_get(root, name), v);
}

// Aplica la visibilidad COMPLETA y coherente de todos los controles
// (y grupos) condicionales según la configuración actual. Se usa tanto
// al construir las propiedades como en cada callback: OBS dispara los
// modified_callback al abrir el diálogo y en orden arbitrario, así que
// cada callback debe dejar el estado global consistente.
static void apply_visibility(obs_properties_t *props, obs_data_t *settings)
{
    obs_properties_t *root = root_props(props);

    const int  mode    = (int)obs_data_get_int(settings, "mode");
    const int  pos     = (int)obs_data_get_int(settings, "text_pos");
    const bool outline = obs_data_get_bool(settings, "outline_on");
    const int  bg      = (int)obs_data_get_int(settings, "bg_type");
    const int  offline = (int)obs_data_get_int(settings, "offline_type");

    const bool is_text = (mode == MODE_TEXT);
    const bool is_mm   = (mode == MODE_MULTIMEDIA);

    // Secciones completas según el modo
    set_vis(root, "location_group",   is_text);
    set_vis(root, "typography_group", is_text);
    set_vis(root, "background_group", is_text);
    set_vis(root, "multimedia_group", is_mm);

    // Sub-visibilidad dentro de cada sección
    set_vis(root, "custom_y",      pos == POS_CUSTOM);
    set_vis(root, "outline_color", outline);
    set_vis(root, "outline_width", outline);
    set_vis(root, "bg_color",      bg == BG_SOLID);
    set_vis(root, "bg_opacity",    bg == BG_SOLID);
    set_vis(root, "bg_image_path", bg == BG_IMAGE);
    set_vis(root, "logo_path",     offline == OFFLINE_LOGO);
}

static bool cb_visibility(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
    apply_visibility(props, settings);
    return true;
}

// ── Definición de propiedades ─────────────────────────────────
static obs_properties_t *ph_get_properties(void *data)
{
    auto *d = static_cast<PraiseHimData *>(data);
    obs_properties_t *props = obs_properties_create();

    // ── Modo (fuera de grupos, gobierna qué secciones se ven) ──
    obs_property_t *p_mode = obs_properties_add_list(
        props, "mode", obs_module_text("Mode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p_mode, obs_module_text("ModeText"),       MODE_TEXT);
    obs_property_list_add_int(p_mode, obs_module_text("ModeMultimedia"), MODE_MULTIMEDIA);
    obs_property_set_modified_callback(p_mode, cb_visibility);

    // ── Sección: Conexión ─────────────────────────────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_properties_add_text(g, "server_url", obs_module_text("ServerUrl"), OBS_TEXT_DEFAULT);
        obs_properties_add_text(g, "obs_token",  obs_module_text("OBSToken"),  OBS_TEXT_PASSWORD);
        obs_properties_add_group(props, "conn_group", obs_module_text("Connection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Sección: Lienzo ───────────────────────────────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_properties_add_int(g, "canvas_w", obs_module_text("CanvasWidth"),  1, 7680, 1);
        obs_properties_add_int(g, "canvas_h", obs_module_text("CanvasHeight"), 1, 4320, 1);
        obs_properties_add_group(props, "canvas_group", obs_module_text("CanvasSection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Sección: Ubicación del texto ──────────────────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_property_t *p_pos = obs_properties_add_list(
            g, "text_pos", obs_module_text("TextPosition"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(p_pos, obs_module_text("PosUpper"),  POS_UPPER);
        obs_property_list_add_int(p_pos, obs_module_text("PosMiddle"), POS_MIDDLE);
        obs_property_list_add_int(p_pos, obs_module_text("PosLower"),  POS_LOWER);
        obs_property_list_add_int(p_pos, obs_module_text("PosCustom"), POS_CUSTOM);
        obs_property_set_modified_callback(p_pos, cb_visibility);
        obs_properties_add_float_slider(
            g, "custom_y", obs_module_text("CustomYPercent"), 0.0, 100.0, 0.5);
        obs_properties_add_int_slider(g, "box_w_pct", obs_module_text("BoxWidthPercent"),  10, 100, 1);
        obs_properties_add_int_slider(g, "box_h_pct", obs_module_text("BoxHeightPercent"),  5, 100, 1);
        obs_properties_add_int(g, "bg_padding", obs_module_text("BackgroundPadding"), 0, 64, 1);
        obs_properties_add_group(props, "location_group", obs_module_text("LocationSection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Sección: Tipografía ───────────────────────────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_properties_add_text(g, "font_face", obs_module_text("FontFamily"), OBS_TEXT_DEFAULT);
        obs_properties_add_int (g, "font_size", obs_module_text("FontSize"), 8, 300, 1);
        obs_properties_add_bool(g, "font_bold",   obs_module_text("FontBold"));
        obs_properties_add_bool(g, "font_italic", obs_module_text("FontItalic"));
        obs_properties_add_color_alpha(g, "text_color", obs_module_text("TextColor"));

        obs_properties_add_color(g, "highlight_color", obs_module_text("HighlightColor"));
        obs_properties_add_int_slider(g, "highlight_opacity",
                                      obs_module_text("HighlightOpacity"), 0, 100, 1);

        obs_property_t *p_out = obs_properties_add_bool(
            g, "outline_on", obs_module_text("OutlineEnabled"));
        obs_property_set_modified_callback(p_out, cb_visibility);
        obs_properties_add_color_alpha(g, "outline_color", obs_module_text("OutlineColor"));
        obs_properties_add_int(g, "outline_width", obs_module_text("OutlineWidth"), 1, 20, 1);
        obs_properties_add_group(props, "typography_group", obs_module_text("TypographySection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Sección: Fondo ────────────────────────────────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_property_t *p_bg = obs_properties_add_list(
            g, "bg_type", obs_module_text("BackgroundType"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(p_bg, obs_module_text("BgNone"),  BG_NONE);
        obs_property_list_add_int(p_bg, obs_module_text("BgSolid"), BG_SOLID);
        obs_property_list_add_int(p_bg, obs_module_text("BgImage"), BG_IMAGE);
        obs_property_set_modified_callback(p_bg, cb_visibility);

        obs_properties_add_color(g, "bg_color",   obs_module_text("BackgroundColor"));
        obs_properties_add_int_slider(g, "bg_opacity",
                                      obs_module_text("BackgroundOpacity"), 0, 100, 1);
        obs_properties_add_path(g, "bg_image_path", obs_module_text("BackgroundImage"),
                                OBS_PATH_FILE, "Images (*.png *.jpg *.jpeg *.bmp *.webp)", nullptr);
        obs_properties_add_bool(g, "auto_hide", obs_module_text("AutoHide"));
        obs_properties_add_group(props, "background_group", obs_module_text("BackgroundSection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Sección: Multimedia (sin contenido activo) ────────────
    {
        obs_properties_t *g = obs_properties_create();
        obs_property_t *p_off = obs_properties_add_list(
            g, "offline_type", obs_module_text("OfflineDisplay"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(p_off, obs_module_text("OfflineBlack"), OFFLINE_BLACK);
        obs_property_list_add_int(p_off, obs_module_text("OfflineLogo"),  OFFLINE_LOGO);
        obs_property_set_modified_callback(p_off, cb_visibility);
        obs_properties_add_path(g, "logo_path", obs_module_text("LogoImage"),
                                OBS_PATH_FILE, "Images (*.png *.jpg *.jpeg *.bmp *.webp)", nullptr);
        obs_properties_add_group(props, "multimedia_group", obs_module_text("MultimediaSection"),
                                 OBS_GROUP_NORMAL, g);
    }

    // ── Visibilidad inicial ───────────────────────────────────
    // Los modified_callback pueden dispararse al abrir el diálogo en
    // orden arbitrario; aplicamos el estado completo desde los ajustes
    // guardados para que los controles condicionales arranquen bien.
    obs_data_t *settings = obs_source_get_settings(d->source);
    apply_visibility(props, settings);
    obs_data_release(settings);

    return props;
}

// ── Aplicar configuración ─────────────────────────────────────
static void ph_update(void *data, obs_data_t *settings)
{
    auto *d = static_cast<PraiseHimData *>(data);

    std::string new_url   = obs_data_get_string(settings, "server_url");
    std::string new_token = obs_data_get_string(settings, "obs_token");

    d->mode       = (SourceMode)obs_data_get_int(settings, "mode");
    d->canvas_w   = (uint32_t)std::max(1LL, obs_data_get_int(settings, "canvas_w"));
    d->canvas_h   = (uint32_t)std::max(1LL, obs_data_get_int(settings, "canvas_h"));

    // Texto
    d->text_pos     = (TextPosition)obs_data_get_int(settings, "text_pos");
    d->custom_y     = (float)obs_data_get_double(settings, "custom_y");
    d->box_w_pct    = (int)obs_data_get_int(settings, "box_w_pct");
    d->box_h_pct    = (int)obs_data_get_int(settings, "box_h_pct");
    d->font_face    = obs_data_get_string(settings, "font_face");
    d->font_size    = (int)obs_data_get_int(settings, "font_size");
    d->font_bold    = obs_data_get_bool(settings, "font_bold");
    d->font_italic  = obs_data_get_bool(settings, "font_italic");
    d->text_color   = (uint32_t)obs_data_get_int(settings, "text_color");
    d->highlight_color   = (uint32_t)obs_data_get_int(settings, "highlight_color");
    d->highlight_opacity = (int)obs_data_get_int(settings, "highlight_opacity");
    d->outline_on   = obs_data_get_bool(settings, "outline_on");
    d->outline_color= (uint32_t)obs_data_get_int(settings, "outline_color");
    d->outline_width= (int)obs_data_get_int(settings, "outline_width");
    d->bg_type      = (BgType)obs_data_get_int(settings, "bg_type");
    d->bg_color     = (uint32_t)obs_data_get_int(settings, "bg_color");
    d->bg_opacity   = (int)obs_data_get_int(settings, "bg_opacity");
    d->bg_padding   = (int)obs_data_get_int(settings, "bg_padding");
    d->auto_hide    = obs_data_get_bool(settings, "auto_hide");

    std::string new_bg   = obs_data_get_string(settings, "bg_image_path");
    if (new_bg != d->bg_image_path) {
        d->bg_image_path = new_bg;
        d->load_bg_image();
    }

    // Multimedia
    d->offline_type = (OfflineType)obs_data_get_int(settings, "offline_type");
    std::string new_logo = obs_data_get_string(settings, "logo_path");
    if (new_logo != d->logo_path) {
        d->logo_path = new_logo;
        d->load_logo_image();
    }

    // Reconectar SSE si cambió la URL o el token
    if (new_url != d->server_url || new_token != d->obs_token) {
        d->server_url = new_url;
        d->obs_token  = new_token;
        d->reconnect_sse();
    }

    // Forzar re-render con la configuración nueva
    d->on_state(d->state);
}

// ── Crear fuente ──────────────────────────────────────────────
static void *ph_create(obs_data_t *settings, obs_source_t *source)
{
    auto *d    = new PraiseHimData();
    d->source  = source;

    // Arrancar hilo de render
    d->render_thread = std::thread(&PraiseHimData::render_worker_loop, d);

    ph_update(d, settings);

    // Renderizar frame inicial
    d->on_state(d->state);

    return d;
}

// ── Mostrar / ocultar fuente ──────────────────────────────────
// El ojo de la lista de Sources (obs_sceneitem_set_visible), el cambio de escena
// y el preview del diálogo de propiedades pasan todos por estos callbacks.
//
// Sin esto, ocultar la fuente y volver a mostrarla dejaba en pantalla el slide
// anterior: el estado nuevo llegaba igual por SSE, pero OBS descarta los frames
// de una fuente que no se está renderizando, así que al reaparecer mostraba el
// último frame que había alcanzado a procesar. No hace falta consultar al backend
// —el estado nunca se perdió—, alcanza con reemitir el frame.
static void ph_show(void *data)
{
    auto *d = static_cast<PraiseHimData *>(data);
    d->visible.store(true);
    d->request_render();
}

static void ph_hide(void *data)
{
    auto *d = static_cast<PraiseHimData *>(data);
    d->visible.store(false);
}

// ── Destruir fuente ───────────────────────────────────────────
static void ph_destroy(void *data)
{
    auto *d = static_cast<PraiseHimData *>(data);

    // Detener SSE
    if (d->sse) {
        d->sse->stop();
        d->sse.reset();
    }

    // Detener hilo de render
    {
        std::lock_guard<std::mutex> lk(d->work_mutex);
        d->shutting_down = true;
    }
    d->work_cv.notify_all();
    if (d->render_thread.joinable())
        d->render_thread.join();

    delete d;
}

// ── Render de video (hilo gráfico de OBS) ─────────────────────
// Con OBS_SOURCE_ASYNC_VIDEO no se necesita video_render —
// los frames se entregan via obs_source_output_video en push_frame.

// ═════════════════════════════════════════════════════════════
//  Registro de la fuente
// ═════════════════════════════════════════════════════════════

obs_source_info praisehim_source_info = {};

void register_praisehim_source()
{
    praisehim_source_info.id           = "praisehim_source";
    praisehim_source_info.type         = OBS_SOURCE_TYPE_INPUT;
    praisehim_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    praisehim_source_info.icon_type    = OBS_ICON_TYPE_SLIDESHOW;
    praisehim_source_info.get_name     = ph_get_name;
    praisehim_source_info.create       = ph_create;
    praisehim_source_info.destroy      = ph_destroy;
    praisehim_source_info.show         = ph_show;
    praisehim_source_info.hide         = ph_hide;
    // get_width / get_height no son necesarios con ASYNC_VIDEO;
    // OBS los infiere del primer frame recibido.
    praisehim_source_info.get_properties = ph_get_properties;
    praisehim_source_info.get_defaults   = ph_get_defaults;
    praisehim_source_info.update         = ph_update;
}
