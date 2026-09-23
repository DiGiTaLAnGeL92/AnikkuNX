#pragma once

#include <borealis.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/**
 * Piccolo pool di thread per le richieste di rete.
 * brls::async usa un solo thread: con le copertine da scaricare la UI resterebbe in coda.
 */
class ThreadPool {
  public:
    static ThreadPool& instance() {
        static ThreadPool pool(4);
        return pool;
    }

    void run(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
    }

    /** Svuota la coda (usato quando si esce da una schermata con molte copertine in attesa). */
    void clearPending() {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.clear();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            tasks.clear();
        }
        cv.notify_all();
        for (auto& t : workers)
            if (t.joinable()) t.join();
        workers.clear();
    }

  private:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [this] { return stopping || !tasks.empty(); });
                        if (stopping) return;
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    try {
                        task();
                    } catch (const std::exception& e) {
                        brls::Logger::error("Errore in un task: {}", e.what());
                    }
                }
            });
        }
    }

    std::vector<std::thread> workers;
    std::deque<std::function<void()>> tasks;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
};

/** Token di vita: le callback asincrone controllano che la schermata esista ancora. */
using AliveToken = std::shared_ptr<bool>;

inline AliveToken makeAlive() { return std::make_shared<bool>(true); }

/**
 * Esegue `work` in background e poi `done` (o `fail`) sul thread della UI,
 * solo se `alive` e' ancora vero.
 */
template <typename T>
void runAsync(AliveToken alive, std::function<T()> work, std::function<void(T)> done,
              std::function<void(const std::string&)> fail = nullptr) {
    std::weak_ptr<bool> weak = alive;
    ThreadPool::instance().run([weak, work, done, fail] {
        {
            auto a = weak.lock();
            if (!a || !*a) return;
        }
        try {
            T result = work();
            brls::sync([weak, done, result]() mutable {
                auto a = weak.lock();
                if (a && *a) done(std::move(result));
            });
        } catch (const std::exception& e) {
            std::string msg = e.what();
            brls::sync([weak, fail, msg] {
                auto a = weak.lock();
                if (!a || !*a) return;
                if (fail)
                    fail(msg);
                else
                    brls::Application::notify(msg);
            });
        }
    });
}
