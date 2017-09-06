#include "compress.h"
#include "crc32c.h"
#include "log_reader.h"
#include "log_writer.h"
#include "varint.h"

namespace LeviDB {
    namespace LogReader {
        void defaultReporter(const Exception & e) {
            throw e;
        };

        static inline bool isRecordFull(char t) noexcept {
            return (t & (0b11 << 2)) == (0b00 << 2);
        }

        static inline bool isRecordFirst(char t) noexcept {
            return (t & (0b11 << 2)) == (0b01 << 2);
        }

        static inline bool isRecordMiddle(char t) noexcept {
            return (t & (0b11 << 2)) == (0b10 << 2);
        }

        static inline bool isRecordLast(char t) noexcept {
            return (t & (0b11 << 2)) == (0b11 << 2);
        }

        static inline bool isRecordCompress(char t) noexcept {
            return (t & (1 << 4)) == (1 << 4);
        }

        static inline bool isRecordDel(char t) noexcept {
            return (t & (1 << 5)) == (1 << 5);
        }

        class RawIterator : public SimpleIterator<Slice> {
        private:
            RandomAccessFile * _dst;
            Slice _item;
            uint32_t _cursor;
            bool _eof = false;
            char _backing_store[LogWriterConst::block_size_]{};

        public:
            RawIterator(RandomAccessFile * dst, uint32_t offset)
                    : _dst(dst), _cursor(offset) { next(); }

            DELETE_MOVE(RawIterator);
            DELETE_COPY(RawIterator);

            ~RawIterator() noexcept override = default;

            EXPOSE(_cursor);

            bool valid() const override {
                return !_eof;
            };

            Slice item() const override {
                return _item;
            };

            void next() override {
                size_t block_offset = _cursor % LogWriterConst::block_size_;
                bool pad = (_item.size() != 0
                            && (isRecordFull(_item.back()) || isRecordLast(_item.back()))
                            && (block_offset & 1) == 1);
                _cursor += static_cast<uint32_t>(pad);
                block_offset += static_cast<size_t>(pad);
                size_t remaining_bytes = LogWriterConst::block_size_ - block_offset;

                // skip trailer
                if (block_offset != 0 && remaining_bytes < LogWriterConst::header_size_) {
                    _cursor += remaining_bytes;
                    return next();
                }

                try {
                    char buf[LogWriterConst::header_size_]{};
                    if (_dst->read(_cursor, LogWriterConst::header_size_, buf).size() < LogWriterConst::header_size_) {
                        _eof = true;
                        return;
                    };
                    _cursor += LogWriterConst::header_size_;
                    remaining_bytes -= LogWriterConst::header_size_;

                    uint16_t length;
                    memcpy(&length, buf + 4/* checksum */+ 1/* type */, sizeof(length));
                    if (length > remaining_bytes) {
                        throw Exception::corruptionException("bad record length");
                    }

                    if (_dst->read(_cursor, length, _backing_store).size() < length) {
                        _eof = true;
                        return;
                    };
                    _cursor += length;

                    uint32_t calc_checksum = CRC32C::extend(CRC32C::value(buf + 4, 1 + sizeof(length)),
                                                            _backing_store, length);
                    uint32_t store_checksum;
                    memcpy(&store_checksum, buf, sizeof(store_checksum));
                    if (calc_checksum != store_checksum) {
                        throw Exception::corruptionException("checksum mismatch");
                    }

                    // store meta char
                    _backing_store[length] = buf[4];
                    _item = Slice(_backing_store, length + 1);
                } catch (const Exception & e) { // anything wrong? drop and report
                    if (e.isIOError()) {
                        _eof = true;
                    }
                    throw e;
                }
            };
        };

        std::unique_ptr<SimpleIterator<Slice>>
        makeRawIterator(RandomAccessFile * data_file, uint32_t offset) {
            return std::make_unique<RawIterator>(data_file, offset);
        }

        // RawIterator 的结尾是 meta char, 掩盖掉然后传给解压器
        class IteratorTrimLastChar : public SimpleIterator<Slice> {
        private:
            std::unique_ptr<SimpleIterator<Slice>> _raw_iter;

        public:
            explicit IteratorTrimLastChar(std::unique_ptr<SimpleIterator<Slice>> && raw_iter) noexcept
                    : _raw_iter(std::move(raw_iter)) {}

            DEFAULT_MOVE(IteratorTrimLastChar);
            DELETE_COPY(IteratorTrimLastChar);

            ~IteratorTrimLastChar() noexcept override = default;

            bool valid() const override {
                return _raw_iter->valid();
            };

            Slice item() const override {
                return {_raw_iter->item().data(), _raw_iter->item().size() - 1};
            };

            void next() override {
                _raw_iter->next();
            }
        };

        // 将压缩的流解压
        class UncompressIterator : public SimpleIterator<Slice> {
        private:
            RawIterator * _raw_iter_ob;
            std::unique_ptr<SimpleIterator<Slice>> _decode_iter;
            std::vector<uint8_t> _buffer;
            bool _valid = false;

        public:
            explicit UncompressIterator(std::unique_ptr<SimpleIterator<Slice>> && raw_iter)
                    : _raw_iter_ob(static_cast<RawIterator *>(raw_iter.get())),
                      _decode_iter(Compressor::makeDecodeIterator(
                              std::make_unique<IteratorTrimLastChar>(std::move(raw_iter)))) { next(); }

            DEFAULT_MOVE(UncompressIterator);
            DELETE_COPY(UncompressIterator);

            ~UncompressIterator() noexcept override = default;

            bool valid() const override {
                return _valid;
            };

            Slice item() const override {
                return {_buffer.data(), _buffer.size()};
            };

            void next() override {
                _buffer.clear();
                _valid = _decode_iter->valid();

                char type = _raw_iter_ob->item().back();
                uint32_t cursor = _raw_iter_ob->immut_cursor();
                while (_raw_iter_ob->immut_cursor() == cursor && _decode_iter->valid()) { // 必须解压完当前 block
                    _buffer.insert(_buffer.end(),
                                   reinterpret_cast<const uint8_t *>(_decode_iter->item().data()),
                                   reinterpret_cast<const uint8_t *>(
                                           _decode_iter->item().data() + _decode_iter->item().size()));
                    _decode_iter->next();
                }
                _buffer.emplace_back(charToUint8(type));
            }
        };

        class RecordIteratorBase {
        private:
            mutable std::unique_ptr<SimpleIterator<Slice>> _raw_iter;
            mutable std::vector<uint8_t> _buffer;
            mutable char _prev_type = 0; // dummy FULL
            mutable bool _meet_all = false;

        protected:
            explicit RecordIteratorBase(std::unique_ptr<SimpleIterator<Slice>> && raw_iter)
                    : _raw_iter(std::move(raw_iter)) {}

            EXPOSE(_raw_iter);

            EXPOSE(_buffer);

            EXPOSE(_meet_all);

            void ensureDataLoad(uint32_t length) const {
                while (length > _buffer.size() && !_meet_all && _raw_iter->valid()) {
                    fetchData();
                }
                if (_buffer.size() < length) {
                    throw Exception::IOErrorException("EOF");
                }
            }

        private:
            void fetchData() const {
                dependencyCheck(_prev_type, _raw_iter->item().back());
                _prev_type = _raw_iter->item().back();

                _buffer.insert(_buffer.end(),
                               reinterpret_cast<const uint8_t *>(_raw_iter->item().data()),
                               reinterpret_cast<const uint8_t *>(
                                       _raw_iter->item().data() + _raw_iter->item().size() - 1));
                if (!_meet_all) {
                    _raw_iter->next();
                }
            }

            void dependencyCheck(char type_a, char type_b) const {
                _meet_all = (isRecordFull(type_b) || isRecordLast(type_b));

                if (isRecordFull(type_a) || isRecordLast(type_a)) { // prev is completed
                    if (isRecordFull(type_b) || isRecordFirst(type_b)) {
                        return;
                    }
                }

                if (isRecordFirst(type_a) || isRecordMiddle(type_a)) { // prev is starting
                    if (isRecordMiddle(type_b) || isRecordLast(type_b)) {
                        // same compress, same del
                        if (((type_a ^ type_b) & (0b11 << 4)) == 0) {
                            return;
                        }
                    }
                }

                throw Exception::corruptionException("fragmented record");
            }
        };

        class RecordIterator : public RecordIteratorBase, public Iterator<Slice, std::string> {
        private:
            uint32_t _k_len = 0;
            uint8_t _k_from = 0;
            bool _done = true;
            bool _del;

        public:
            explicit RecordIterator(std::unique_ptr<SimpleIterator<Slice>> && raw_iter)
                    : RecordIteratorBase(std::move(raw_iter)), _del(isRecordDel(immut_raw_iter()->item().back())) {}

            DEFAULT_MOVE(RecordIterator);
            DELETE_COPY(RecordIterator);

            ~RecordIterator() noexcept override = default;

        public:
            bool valid() const override { return !_done; };

            void seekToFirst() override {
                if (_k_len == 0) {
                    assert(_k_from == 0);
                    while (true) {
                        ensureDataLoad(++_k_from);
                        if (decodeVarint32(reinterpret_cast<const char *>(immut_buffer().data()),
                                           reinterpret_cast<const char *>(immut_buffer().data() + _k_from),
                                           &_k_len) != nullptr) {
                            break;
                        }
                    }
                }
                _done = false;
            };

            void seekToLast() override {
                seekToFirst();
            };

            void seek(const Slice & target) override {
                seekToFirst();
                _done = SliceComparator{}(key(), target);
            };

            void next() override {
                _done = true;
            };

            void prev() override {
                next();
            };

            Slice key() const override {
                ensureDataLoad(_k_from + _k_len);
                return {&immut_buffer()[_k_from], _k_len};
            };

            std::string value() const override {
                while (!immut_meet_all()) {
                    ensureDataLoad(static_cast<uint32_t>(immut_buffer().size() + 1));
                }
                return std::string(reinterpret_cast<const char *>(&immut_buffer()[_k_from + _k_len]),
                                   reinterpret_cast<const char *>(&immut_buffer().back() + 1))
                       + static_cast<char>(_del);
            };
        };

        class RecordIteratorCompress : public RecordIteratorBase, public Iterator<Slice, std::string> {
        protected:
            typedef std::pair<uint32_t, uint32_t> from_to;
            typedef std::pair<from_to, from_to> kv_pair;

            std::vector<kv_pair> _rep;
            std::vector<kv_pair>::const_iterator _cursor;

        public:
            explicit RecordIteratorCompress(std::unique_ptr<SimpleIterator<Slice>> && raw_iter)
                    : RecordIteratorBase(std::make_unique<UncompressIterator>(std::move(raw_iter))),
                      _cursor(_rep.cend()) {}

            DELETE_MOVE(RecordIteratorCompress);
            DELETE_COPY(RecordIteratorCompress);

            ~RecordIteratorCompress() noexcept override = default;

        public:
            bool valid() const override { return _cursor != _rep.cend(); };

            void seekToFirst() override {
                if (_rep.empty()) {
                    assert(immut_buffer().empty());

                    uint16_t meta_len;
                    ensureDataLoad(sizeof(meta_len));
                    memcpy(&meta_len, &immut_buffer()[0], sizeof(meta_len));

                    ensureDataLoad(sizeof(meta_len) + meta_len);
                    std::vector<from_to> ranges;
                    const auto * p = reinterpret_cast<const char *>(&immut_buffer()[sizeof(meta_len)]);
                    const auto * limit = reinterpret_cast<const char *>(&immut_buffer()[sizeof(meta_len) + meta_len]);

                    uint32_t offset = sizeof(meta_len) + meta_len;
                    while (p != limit) {
                        uint32_t val;
                        p = decodeVarint32(p, limit, &val);
                        if (p == nullptr) {
                            throw Exception::corruptionException("meta area of CompressRecord broken");
                        }
                        ranges.emplace_back(from_to(offset, offset + val));
                        offset += val;
                    }
                    size_t half = ranges.size() / 2;
                    for (int i = 0; i < half; ++i) {
                        _rep.emplace_back(kv_pair(ranges[i], ranges[i + half]));
                    }
                }

                _cursor = _rep.cbegin();
            };

            void seekToLast() override {
                seekToFirst();
                _cursor = --_rep.cend();
            };

            void seek(const Slice & target) override {
                if (_cursor >= _rep.cbegin() && _cursor < _rep.cend() && target == key()) {
                    return;
                }
                seekToFirst();
                while (valid() && SliceComparator{}(key(), target)) {
                    next();
                }
            };

            void next() override {
                ++_cursor;
            };

            void prev() override {
                if (_cursor == _rep.cbegin()) {
                    _cursor = _rep.cend();
                } else {
                    --_cursor;
                }
            };

            Slice key() const override {
                const auto kv = *_cursor;
                const auto k_from_to = kv.first;
                ensureDataLoad(k_from_to.second);
                return {&immut_buffer()[k_from_to.first], k_from_to.second - k_from_to.first};
            };

            std::string value() const override {
                const auto kv = *_cursor;
                const auto v_from_to = kv.second;
                ensureDataLoad(v_from_to.second);
                return std::string(reinterpret_cast<const char *>(&immut_buffer()[v_from_to.first]),
                                   v_from_to.second - v_from_to.first)
                       + static_cast<char>(false);
            };
        };

        std::unique_ptr<kv_iter_t>
        makeIterator(RandomAccessFile * data_file, uint32_t offset) {
            auto raw_iter = makeRawIterator(data_file, offset);
            if (isRecordCompress(raw_iter->item().back())) {
                return std::make_unique<RecordIteratorCompress>(std::move(raw_iter));
            }
            return std::make_unique<RecordIterator>(std::move(raw_iter));
        }

        bool isRecordIteratorCompress(kv_iter_t * it) noexcept {
            return dynamic_cast<RecordIteratorCompress *>(it) != nullptr;
        };

        static inline bool isBatchFull(char t) noexcept {
            return (t & 0b11) == 0b00;
        }

        static inline bool isBatchFirst(char t) noexcept {
            return (t & 0b11) == 0b01;
        }

        static inline bool isBatchMiddle(char t) noexcept {
            return (t & 0b11) == 0b10;
        }

        static inline bool isBatchLast(char t) noexcept {
            return (t & 0b11) == 0b11;
        }

        // 确保 batch dependency 的 RawIterator
        class RawIteratorBatchChecked : public SimpleIterator<Slice> {
        private:
            std::unique_ptr<RawIterator> _raw_iter;

            std::vector<uint32_t> _disk_offsets;
            std::vector<std::vector<uint8_t>> _cache;
            std::vector<std::vector<uint8_t>>::const_iterator _cache_cursor;
            char _prev_type = 0; // dummy FULL

        public:
            explicit RawIteratorBatchChecked(std::unique_ptr<RawIterator> && raw_iter)
                    : _raw_iter(std::move(raw_iter)), _cache_cursor(_cache.cend()) {
                if (_raw_iter != nullptr) { // safe swapping
                    next();
                }
            }

            RawIteratorBatchChecked(RawIteratorBatchChecked && rhs) noexcept {
                operator=(std::move(rhs));
            }

            RawIteratorBatchChecked & operator=(RawIteratorBatchChecked && rhs) noexcept {
                auto nth = rhs._cache_cursor == rhs._cache.cend() ? -1 : rhs._cache_cursor - rhs._cache.cbegin();
                std::swap(_raw_iter, rhs._raw_iter);
                std::swap(_cache, rhs._cache);
                std::swap(_cache_cursor, rhs._cache_cursor);
                std::swap(_prev_type, rhs._prev_type);
                _cache_cursor = nth != -1 ? _cache.cbegin() + nth : _cache.cend();
                return *this;
            }

            DELETE_COPY(RawIteratorBatchChecked);

            ~RawIteratorBatchChecked() noexcept override = default;

            bool valid() const override {
                return _cache_cursor != _cache.cend();
            };

            Slice item() const override {
                return {(*_cache_cursor).data(), (*_cache_cursor).size()};
            };

            void next() override {
                if (_cache_cursor == _cache.cend() || ++_cache_cursor == _cache.cend()) {
                    _cache.clear();
                    _cache_cursor = _cache.cend();
                    if (!_raw_iter->valid()) {
                        return;
                    }

                    do {
                        dependencyCheck(_prev_type, _raw_iter->item().back());
                        _prev_type = _raw_iter->item().back();

                        if (isRecordFull(_prev_type) || isRecordFirst(_prev_type)) {
                            _disk_offsets.emplace_back(_raw_iter->immut_cursor()
                                                       - (_raw_iter->item().size() - 1/* meta char */)
                                                       - LogWriterConst::header_size_);
                        } else {
                            _disk_offsets.emplace_back(_disk_offsets.back());
                        }

                        _cache.emplace_back(std::vector<uint8_t>(
                                reinterpret_cast<const uint8_t *>(_raw_iter->item().data()),
                                reinterpret_cast<const uint8_t *>(_raw_iter->item().data() + _raw_iter->item().size())
                        ));
                        _raw_iter->next(); // 多读一页

                        if (isBatchFull(_prev_type) || isBatchLast(_prev_type)) {
                            _cache_cursor = _cache.cbegin();
                            return;
                        }
                    } while (_raw_iter->valid());
                    // exit as EOF(not valid)
                }
            }

            uint32_t diskOffset() const noexcept {
                return _disk_offsets[_cache_cursor - _cache.cbegin()];
            }

        private:
            void dependencyCheck(char type_a, char type_b) const {
                if (isBatchFull(type_a) || isBatchLast(type_a)) { // prev is completed
                    if (isBatchFull(type_b) || isBatchFirst(type_b)) {
                        return;
                    }
                }

                if (isBatchFirst(type_a) || isBatchMiddle(type_a)) { // prev is starting
                    if (isBatchMiddle(type_b) || isBatchLast(type_b)) {
                        return;
                    }
                }

                throw Exception::corruptionException("fragmented batch");
            }
        };

        class TableIterator : public SimpleIterator<std::pair<Slice, std::string>> {
        private:
            RawIteratorBatchChecked * _raw_iter_batch_ob;
            std::unique_ptr<kv_iter_t> _kv_iter;

            friend class TableIteratorOffset;

            friend class TableRecoveryIterator;

        public:
            explicit TableIterator(std::unique_ptr<RawIteratorBatchChecked> && raw_iter_batch)
                    : _raw_iter_batch_ob(raw_iter_batch.get()),
                      _kv_iter(makeKVIter(std::move(raw_iter_batch))) { // transfer ownership
                _kv_iter->seekToFirst();
            }

            DEFAULT_MOVE(TableIterator);
            DELETE_COPY(TableIterator);

            ~TableIterator() noexcept override = default;

            bool valid() const override {
                return _kv_iter->valid();
            };

            std::pair<Slice, std::string> item() const override {
                return {_kv_iter->key(), _kv_iter->value()};
            };

            void next() override {
                _kv_iter->next();
                if (!_kv_iter->valid() && _raw_iter_batch_ob->valid()) { // 切换到下个 kv_iter
                    auto it = std::make_unique<RawIteratorBatchChecked>(nullptr);
                    std::swap(*it, *_raw_iter_batch_ob);
                    do { // 再次 next 以保持 invariant, 因为 record iterator 在 meet all 之后不会有副作用
                        it->next();
                        if (it->valid()) { // 确保 seek 到 next record
                            if (isRecordFull(it->item().back()) || isRecordFirst(it->item().back())) {
                                _raw_iter_batch_ob = it.get();
                                _kv_iter = makeKVIter(std::move(it));
                                _kv_iter->seekToFirst();
                                return;
                            }
                        } else {
                            break;
                        }
                    } while (true);
                    // exit as EOF(not valid)
                }
            }

        private:
            static std::unique_ptr<kv_iter_t> makeKVIter(std::unique_ptr<RawIteratorBatchChecked> && p) {
                if (isRecordCompress(p->item().back())) {
                    return std::make_unique<RecordIteratorCompress>(std::move(p));
                }
                return std::make_unique<RecordIterator>(std::move(p));
            }
        };

        std::unique_ptr<SimpleIterator<std::pair<Slice/* K */, std::string/* V */>>>
        makeTableIterator(RandomAccessFile * data_file) {
            return std::make_unique<TableIterator>(
                    std::make_unique<RawIteratorBatchChecked>(
                            std::make_unique<RawIterator>(data_file, 0)));
        };

        class TableIteratorOffset : public SimpleIterator<std::pair<Slice, uint32_t>> {
        private:
            TableIterator _table;

            friend class TableRecoveryIterator;

        public:
            explicit TableIteratorOffset(std::unique_ptr<RawIteratorBatchChecked> && raw_iter_batch)
                    : _table(std::move(raw_iter_batch)) {}

            DEFAULT_MOVE(TableIteratorOffset);
            DELETE_COPY(TableIteratorOffset);

            ~TableIteratorOffset() noexcept override = default;

            bool valid() const override {
                return _table.valid();
            };

            std::pair<Slice, uint32_t> item() const override {
                return {_table._kv_iter->key(), _table._raw_iter_batch_ob->diskOffset()};
            };

            void next() override {
                _table.next();
            }
        };

        std::unique_ptr<SimpleIterator<std::pair<Slice/* K */, uint32_t/* offset */>>>
        makeTableIteratorOffset(RandomAccessFile * data_file) {
            return std::make_unique<TableIteratorOffset>(
                    std::make_unique<RawIteratorBatchChecked>(
                            std::make_unique<RawIterator>(data_file, 0)));
        };

        class TableRecoveryIterator : public SimpleIterator<std::pair<Slice, uint32_t>> {
        private:
            RandomAccessFile * _data_file;
            mutable std::unique_ptr<TableIteratorOffset> _t;
            reporter_t _reporter;

        public:
            explicit TableRecoveryIterator(RandomAccessFile * data_file, reporter_t reporter) noexcept
                    : _data_file(data_file), _reporter(std::move(reporter)) {
                try {
                    _t = std::make_unique<TableIteratorOffset>(
                            std::make_unique<RawIteratorBatchChecked>(
                                    std::make_unique<RawIterator>(data_file, 0)));
                } catch (const Exception & e) {
                    _reporter(e);
                }
            }

            DEFAULT_MOVE(TableRecoveryIterator);
            DELETE_COPY(TableRecoveryIterator);

            ~TableRecoveryIterator() noexcept override = default;

            bool valid() const override {
                return _t != nullptr && _t->valid();
            };

            std::pair<Slice, uint32_t> item() const override {
                try {
                    return _t->item();
                } catch (const Exception & e) {
                    handle(e);
                    return {};
                }
            };

            void next() override {
                try {
                    _t->next();
                } catch (const Exception & e) {
                    handle(e);
                }
            }

        private:
            void handle(const Exception & e) const noexcept {
                // must success, otherwise we are done in the constructor
                uint32_t curr_disk_offset = _t->_table._raw_iter_batch_ob->diskOffset();
                _reporter(Exception::invalidArgumentException(e.toString(), std::to_string(curr_disk_offset)));

                while (true) {
                    // skip to next block
                    curr_disk_offset += (LogWriterConst::block_size_ - curr_disk_offset % LogWriterConst::block_size_);

                    try { // resync
                        auto raw_it = std::make_unique<RawIterator>(_data_file, curr_disk_offset);
                        while (raw_it->valid()) {
                            if (isBatchFull(raw_it->item().back()) || isBatchFirst(raw_it->item().back())) {
                                _t = std::make_unique<TableIteratorOffset>(
                                        std::make_unique<RawIteratorBatchChecked>(std::move(raw_it)));
                                return;
                            }
                            raw_it->next();
                        }
                        break;
                    } catch (const Exception & exception) {
                        _reporter(exception);
                        if (exception.isIOError()) {
                            break;
                        }
                    }
                }
                _t = nullptr;
            }
        };

        std::unique_ptr<SimpleIterator<std::pair<Slice/* K */, uint32_t/* offset */>>>
        makeTableRecoveryIterator(RandomAccessFile * data_file, reporter_t reporter) noexcept {
            return std::make_unique<TableRecoveryIterator>(data_file, std::move(reporter));
        };
    }
}