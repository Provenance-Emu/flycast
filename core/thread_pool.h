#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

// Forward declarations
class Emulator;

namespace flycast {

/// A thread pool for executing tasks in parallel
class ThreadPool {
public:
    /// Constructor
    /// @param threads Number of worker threads to create
    ThreadPool(size_t threads = std::thread::hardware_concurrency()) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { 
                            return stop || !tasks.empty(); 
                        });
                        
                        if (stop && tasks.empty()) {
                            return;
                        }
                        
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    
                    task();
                    active_tasks--;
                }
            });
        }
    }
    
    /// Destructor
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        
        condition.notify_all();
        
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    
    /// Enqueue a task to be executed by the thread pool
    /// @param f Function to execute
    /// @param args Arguments to pass to the function
    /// @return Future for the result of the function
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> result = task->get_future();
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            
            if (stop) {
                throw std::runtime_error("Cannot enqueue on stopped ThreadPool");
            }
            
            tasks.emplace([task]() { (*task)(); });
            active_tasks++;
        }
        
        condition.notify_one();
        return result;
    }
    
    /// Check if the thread pool has any active tasks
    /// @return True if there are active tasks, false otherwise
    bool hasActiveTasks() const {
        return active_tasks > 0;
    }
    
    /// Wait for all tasks to complete
    void waitForCompletion() {
        while (active_tasks > 0) {
            std::this_thread::yield();
        }
    }
    
private:
    /// Worker threads
    std::vector<std::thread> workers;
    
    /// Task queue
    std::queue<std::function<void()>> tasks;
    
    /// Mutex for task queue
    std::mutex queue_mutex;
    
    /// Condition variable for worker threads
    std::condition_variable condition;
    
    /// Flag to stop the thread pool
    bool stop;
    
    /// Counter for active tasks
    std::atomic<int> active_tasks{0};
};

} // namespace flycast
