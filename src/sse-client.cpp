// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#include "sse-client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <obs-module.h>

#include <chrono>
#include <thread>

// ── libcurl write callback ────────────────────────────────────
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *client = static_cast<SseClient *>(userdata);
    if (!client->running_.load()) return 0;
    client->feed(ptr, size * nmemb);
    return size * nmemb;
}

// ── libcurl xferinfo callback ─────────────────────────────────
// Curl lo llama ~1 vez/segundo aunque no haya datos en vuelo.
// Retornar != 0 aborta el transfer inmediatamente — esto es lo que
// permite que stop() desbloquee curl_easy_perform() sin esperar
// al próximo heartbeat del backend.
static int curl_xfer_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto *client = static_cast<SseClient *>(clientp);
    return client->running_.load() ? 0 : 1;
}

// ── Constructor / destructor ──────────────────────────────────
SseClient::SseClient(std::string url, Callback callback, std::vector<std::string> headers)
    : url_(std::move(url)), callback_(std::move(callback)), headers_(std::move(headers))
{}

SseClient::~SseClient()
{
    stop();
}

void SseClient::start()
{
    running_ = true;
    thread_  = std::thread(&SseClient::run, this);
}

void SseClient::stop()
{
    running_ = false;
    stop_cv_.notify_all();   // despierta el sleep de reconexión si está esperando
    if (thread_.joinable())
        thread_.join();
}

// ── Hilo principal SSE ────────────────────────────────────────
void SseClient::run()
{
    while (running_) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, "Cache-Control: no-cache");
        for (const auto &h : headers_)
            headers = curl_slist_append(headers, h.c_str());

        curl_easy_setopt(curl, CURLOPT_URL,              url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,        this);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,          0L);   // sin timeout global
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   10L);
        // xferinfo: curl llama este callback ~1/s aunque no haya datos;
        // retornar 1 aborta curl_easy_perform() sin esperar el próximo heartbeat
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     this);

        blog(LOG_INFO, "[PraiseHim] SSE iniciando conexión a: %s", url_.c_str());
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        if (status == 401 || status == 403 || status == 404 || status == 402) {
            // No es un corte de red: reintentar cada 3 s no lo va a arreglar y solo le pega al
            // servidor. 401 = la cuenta se desconectó o el token ya no vale; 404 = el servicio no existe.
            blog(LOG_WARNING, "[PraiseHim] SSE rechazado por el servidor (HTTP %ld)", status);
        } else if (res != CURLE_OK) {
            blog(LOG_WARNING, "[PraiseHim] SSE error curl (%d): %s",
                 (int)res, curl_easy_strerror(res));
        } else {
            blog(LOG_INFO, "[PraiseHim] SSE conexión cerrada normalmente");
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        line_buf_.clear();
        event_data_.clear();

        if (running_) {
            // Sleep interruptible: stop() llama stop_cv_.notify_all() para
            // despertar este wait inmediatamente en lugar de esperar 3 segundos
            std::unique_lock<std::mutex> lk(stop_mutex_);
            bool rechazado = status == 401 || status == 403 || status == 404 || status == 402;
            stop_cv_.wait_for(lk, std::chrono::seconds(rechazado ? 30 : 3),
                              [this] { return !running_.load(); });
        }
    }
}

// ── Procesamiento de bytes entrantes ─────────────────────────
void SseClient::feed(const char *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\n') {
            // quitar \r si viene como CRLF
            if (!line_buf_.empty() && line_buf_.back() == '\r')
                line_buf_.pop_back();
            processLine(line_buf_);
            line_buf_.clear();
        } else {
            line_buf_ += c;
        }
    }
}

void SseClient::processLine(const std::string &line)
{
    if (line.empty()) {
        // Fin de evento — parsear lo acumulado en event_data_
        if (!event_data_.empty()) {
            try {
                auto j = nlohmann::json::parse(event_data_);
                callback_(SlideState::fromJson(j));
            } catch (const std::exception &e) {
                blog(LOG_WARNING, "[PraiseHim] JSON parse error: %s", e.what());
            }
            event_data_.clear();
        }
        return;
    }

    // Campo "data:" (con o sin espacio tras los dos puntos)
    if (line.rfind("data:", 0) == 0) {
        size_t offset = 5;
        if (offset < line.size() && line[offset] == ' ') ++offset;
        event_data_ = line.substr(offset);
    }
    // "event:", "id:", ":" (comentarios) se ignoran
}
