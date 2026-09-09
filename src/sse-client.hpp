// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parte del plugin PraiseHim para OBS Studio. Es GPL porque enlaza contra libobs, que lo
// es: no es una elección nuestra sino la condición para poder ser un plugin de OBS.
// El texto completo está en LICENSE, junto a este directorio.

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "slide-state.hpp"

class SseClient {
public:
    using Callback = std::function<void(const SlideState &)>;

    SseClient(std::string url, Callback callback);
    ~SseClient();

    void start();
    void stop();

    // Llamado desde los callbacks estáticos de libcurl (hilo SSE)
    void feed(const char *data, size_t len);

    // Accesible desde los callbacks estáticos de libcurl
    std::atomic<bool> running_{false};

private:
    void        run();
    void        processLine(const std::string &line);

    std::string        url_;
    Callback           callback_;
    std::thread        thread_;

    // Buffer de línea en curso y de campo "data:" del evento actual
    std::string line_buf_;
    std::string event_data_;

    // Para interrumpir el sleep de reconexión al llamar stop()
    std::mutex              stop_mutex_;
    std::condition_variable stop_cv_;
};
