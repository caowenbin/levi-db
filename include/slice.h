#pragma once
#ifndef LEVIDB8_SLICE_H
#define LEVIDB8_SLICE_H

/*
 * 数据简单封装类
 */

#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

namespace levidb8 {
    class Slice {
    private:
        const char * _data = "";
        size_t _size = 0;
        bool _owned = false;

    public:
        Slice() noexcept = default;

        Slice(const Slice & another) noexcept : _data(another._data), _size(another._size) {}

        Slice & operator=(const Slice & another) noexcept {
            this->~Slice();
            _data = another._data;
            _size = another._size;
            return *this;
        }

        Slice(Slice && another) noexcept : _data(another._data), _size(another._size), _owned(another._owned) {
            another._data = "";
            another._size = 0;
            another._owned = false;
        }

        Slice & operator=(Slice && another) noexcept {
            std::swap(_data, another._data);
            std::swap(_size, another._size);
            std::swap(_owned, another._owned);
            return *this;
        }

        ~Slice() noexcept {
            if (_owned) {
                free(const_cast<char *>(_data));
            }
        }

    public:
        Slice(const char * d, size_t n) noexcept : _data(d), _size(n) {}

        template<typename T>
        Slice(const T * s, size_t n) noexcept
                : Slice(reinterpret_cast<const char *>(s), n) {
            static_assert(sizeof(T) == sizeof(char), "args mismatch");
        }

        template<typename T, typename = decltype(T{}.data())>
        Slice(const T & s) noexcept : Slice(s.data(), s.size()) {}

        template<typename T, typename = std::enable_if_t<std::is_convertible<T, const char *>::value>>
        Slice(T s) noexcept : _data(s), _size(strlen(s)) {}

        template<size_t L>
        explicit Slice(const char (& s)[L]) noexcept : _data(s), _size(L - 1) {}

        const char * data() const noexcept { return _data; }

        size_t size() const noexcept { return _size; }

        bool owned() const noexcept { return _owned; }

        std::string toString() const noexcept { return {_data, _size}; }

        char operator[](size_t n) const noexcept {
            assert(n < _size);
            return _data[n];
        }

        char back() const noexcept {
            assert(_size > 0);
            return _data[_size - 1];
        }

        bool operator==(const Slice & another) const noexcept {
            return _size == another._size && memcmp(_data, another._data, _size) == 0;
        }

        bool operator!=(const Slice & another) const noexcept { return !operator==(another); }

    public:
        static Slice nullSlice() noexcept { return Slice(nullptr, 0); }

        static Slice pinnableSlice(const char * d, size_t n) noexcept {
            Slice res(d, n);
            res._owned = true;
            return res;
        }
    };

    struct SliceComparator {
        using is_transparent = std::true_type;

        bool operator()(const std::string & a, const std::string & b) const noexcept {
            return a < b;
        }

        bool operator()(const Slice & a, const Slice & b) const noexcept {
            int r = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
            if (r == 0) {
                return a.size() < b.size();
            }
            return r < 0;
        }

        bool operator()(const std::string & a, const Slice & b) const noexcept {
            return operator()(Slice(a), b);
        }

        bool operator()(const Slice & a, const std::string & b) const noexcept {
            return operator()(a, Slice(b));
        }
    };
}

#endif //LEVIDB8_SLICE_H
