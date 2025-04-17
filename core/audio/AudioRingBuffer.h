//
// Created by Cascade on 2025-04-16.
// Inspired by various lock-free ring buffer implementations.
//

#pragma once

#include <vector>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <stdexcept>
#include <thread>
#include <chrono>

// Basic lock-free, single-producer, single-consumer ring buffer.
// Assumes T is trivially copyable.
template <typename T>
class AudioRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    // Size must be a power of two.
    explicit AudioRingBuffer(size_t size) :
        buffer_(size),
        capacity_(size),
        write_pos_(0),
        read_pos_(0) {
        if ((size & (size - 1)) != 0) {
            throw std::invalid_argument("Ring buffer size must be a power of two.");
        }
    }

    // Producer: Attempts to write data to the buffer. Returns true on success.
    // Non-blocking.
    bool try_write(const T& data) {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t next_write = (current_write + 1) & (capacity_ - 1);

        if (next_write == read_pos_.load(std::memory_order_acquire)) {
            // Buffer is full
            return false;
        }

        buffer_[current_write] = data;
        write_pos_.store(next_write, std::memory_order_release);
        return true;
    }

    // Producer: Writes data, potentially blocking/spinning if the buffer is full.
    void write(const T& data) {
        while (!try_write(data)) {
            // Spin or yield - simple spin for now
            std::this_thread::yield(); // Or consider std::this_thread::sleep_for for less CPU usage
        }
    }

    // Consumer: Attempts to read data from the buffer. Returns true on success.
    // Non-blocking.
    bool try_read(T& data) {
        size_t current_read = read_pos_.load(std::memory_order_relaxed);

        if (current_read == write_pos_.load(std::memory_order_acquire)) {
            // Buffer is empty
            return false;
        }

        data = buffer_[current_read];
        read_pos_.store((current_read + 1) & (capacity_ - 1), std::memory_order_release);
        return true;
    }

    // Consumer: Reads data, potentially blocking/spinning if the buffer is empty.
    T read() {
        T data;
        while (!try_read(data)) {
            // Spin or yield
            std::this_thread::yield(); // Or consider sleep
        }
        return data;
    }

    // Checks if the buffer is empty (Consumer perspective).
    bool is_empty() const {
        return read_pos_.load(std::memory_order_acquire) == write_pos_.load(std::memory_order_acquire);
    }

    // Checks if the buffer is full (Producer perspective).
    bool is_full() const {
        size_t next_write = (write_pos_.load(std::memory_order_acquire) + 1) & (capacity_ - 1);
        return next_write == read_pos_.load(std::memory_order_acquire);
    }

    // Returns the number of elements currently in the buffer.
    // Note: Can be slightly outdated in concurrent scenarios.
    size_t size() const {
        // Use relaxed order, potential slight inaccuracy is acceptable for size estimate.
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t read = read_pos_.load(std::memory_order_relaxed);
        if (write >= read) {
            return write - read;
        } else {
            return capacity_ - (read - write);
        }
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    std::vector<T> buffer_;
    const size_t capacity_; // Max elements - 1 usable slot

    // Aligning atomics can potentially improve performance on some architectures
    alignas(64) std::atomic<size_t> write_pos_;
    alignas(64) std::atomic<size_t> read_pos_;
};
