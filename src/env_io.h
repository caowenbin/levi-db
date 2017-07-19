#ifndef LEVIDB_ENV_IO_H
#define LEVIDB_ENV_IO_H

/*
 * IO API 封装
 */

#include "slice.h"
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace LeviDB {
    namespace IOEnv {
        enum OpenMode {
            R_M,
            W_M,
            A_M,
            RP_M,
            WP_M,
            AP_M,
        };
        static constexpr int page_size = 4 * 1024; // 4KB

        uint64_t getFileSize(const std::string & fname);
    }

    class FileOpen {
    public:
        int _fd;

        FileOpen(const std::string & fname, IOEnv::OpenMode mode);

        ~FileOpen() noexcept { if (_fd > 0) close(_fd); };
    };

    class FileFopen {
    public:
        FILE * _f;

        FileFopen(const std::string & fname, IOEnv::OpenMode mode);

        ~FileFopen() noexcept { if (_f != NULL) fclose(_f); }
    };

    class MmapFile {
    private:
        void * _mmaped_region;
        uint64_t _length;
        std::string _filename;
        FileOpen _file;

    public:
        MmapFile(const std::string & fname);

        ~MmapFile() noexcept { munmap(_mmaped_region, _length); }

        Slice read(uint64_t offset, size_t n) const noexcept;

        void write(uint64_t offset, const Slice & data);

        void grow();
    };

    class AppendableFile {
    private:
        uint64_t _length;
        std::string _filename;
        FileFopen _ffile;

    public:
        AppendableFile(const std::string & fname);

        ~AppendableFile() noexcept {}

        void append(const Slice & data);

        void flush();

        void sync();
    };

    class RandomAccessFile {
    private:
        std::string _filename;
        FileOpen _file;

    public:
        RandomAccessFile(const std::string & fname);

        ~RandomAccessFile() noexcept {}

        Slice read(uint64_t offset, size_t n, char * scratch) const;
    };
}

#endif //LEVIDB_ENV_IO_H