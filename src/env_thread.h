#pragma once
#ifndef LEVIDB8_ENV_THREAD_H
#define LEVIDB8_ENV_THREAD_H

/*
 * 线程 API 封装
 */

#include <atomic>
#include <thread>

namespace levidb8 {
    class ReadWriteLock {
    private:
        std::atomic<size_t> _cnt{0};
        std::atomic<bool> _need_write{false};

        friend class RWLockReadGuard;

        friend class RWLockWriteGuard;
    };

    class RWLockWriteGuard;

    class RWLockReadGuard {
    private:
        ReadWriteLock * _lock{};

        friend class RWLockWriteGuard;

    public:
        RWLockReadGuard() noexcept = default;

        explicit RWLockReadGuard(ReadWriteLock * lock) noexcept;

        RWLockReadGuard(RWLockReadGuard && another) noexcept;

        RWLockReadGuard & operator=(RWLockReadGuard && another) noexcept;

        ~RWLockReadGuard() noexcept;

    public:
        void release() noexcept;

        static bool tryUpgrade(RWLockReadGuard * read_guard, RWLockWriteGuard * write_guard) noexcept;

    public:
        RWLockReadGuard(const RWLockReadGuard &) noexcept = delete;

        void operator=(const RWLockReadGuard &) noexcept = delete;
    };

    class RWLockWriteGuard {
    private:
        ReadWriteLock * _lock{};

        friend class RWLockReadGuard;

    public:
        RWLockWriteGuard() noexcept = default;

        explicit RWLockWriteGuard(ReadWriteLock * lock) noexcept;

        RWLockWriteGuard(RWLockWriteGuard && another) noexcept;

        RWLockWriteGuard & operator=(RWLockWriteGuard && another) noexcept;

        ~RWLockWriteGuard() noexcept;

    public:
        void release() noexcept;

        static void degrade(RWLockWriteGuard * write_guard, RWLockReadGuard * read_guard) noexcept;

    public:
        RWLockWriteGuard(const RWLockWriteGuard &) noexcept = delete;

        void operator=(const RWLockWriteGuard &) noexcept = delete;
    };
}

#endif //LEVIDB8_ENV_THREAD_H
