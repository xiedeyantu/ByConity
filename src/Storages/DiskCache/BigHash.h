#pragma once

#include <chrono>
#include <istream>
#include <memory>
#include <ostream>
#include <utility>

#include <Core/Types.h>
#include <IO/ReadBuffer.h>
#include <Storages/DiskCache/BloomFilter.h>
#include <Storages/DiskCache/Bucket.h>
#include <Storages/DiskCache/Buffer.h>
#include <Storages/DiskCache/CacheEngine.h>
#include <Storages/DiskCache/Device.h>
#include <Storages/DiskCache/HashKey.h>
#include <Storages/DiskCache/Types.h>
#include <Poco/AtomicCounter.h>
#include <Common/SharedMutex.h>
#include <common/strong_typedef.h>
#include <common/types.h>

namespace DB::HybridCache
{
// BigHash is a small item flash-based cache engine. It divides the device into
// a series of buckets.
//
// Each item is hashed to a bucket according to its key. When full, we
// evict the items in their insertion order.
class BigHash : public CacheEngine
{
public:
    struct Config
    {
        UInt32 bucket_size{4 * 1024};

        // range of device for BigHash access, [cache_start_offset, cache_start_offset + cache_size)
        UInt64 cache_start_offset{};
        UInt64 cache_size{};
        Device * device{nullptr};

        ExpiredCheck check_expired;
        DestructorCallback destructor_callback;

        // optional bloom filter to reduce IO
        std::unique_ptr<BloomFilter> bloom_filters;

        UInt64 numBuckets() const { return cache_size / bucket_size; }

        Config & validate();
    };

    explicit BigHash(Config && config);
    BigHash(const BigHash &) = delete;
    BigHash & operator=(const BigHash &) = delete;
    ~BigHash() override = default;

    UInt64 getSize() const override { return bucket_size_ * num_buckets_; }

    bool couldExist(HashedKey key) override;

    UInt64 estimateWriteSize(HashedKey, BufferView) const override;

    Status lookup(HashedKey key, Buffer & value) override;

    Status insert(HashedKey key, BufferView value) override;

    Status remove(HashedKey key) override;

    void flush() override;

    void reset() override;

    void persist(std::ostream * os) override;

    bool recover(std::istream * is) override;

    UInt64 getMaxItemSize() const override;

    std::pair<Status, std::string> getRandomAlloc(Buffer & value) override;

private:
    Poco::Logger * log = &Poco::Logger::get("BigHashEngine");

    STRONG_TYPEDEF(UInt32, BucketId)

    struct ValidConfigTag
    {
    };
    BigHash(Config && config, ValidConfigTag);

    Buffer readBucket(BucketId bucket_id);
    bool writeBucket(BucketId bucket_id, Buffer buffer);

    // Hold the lock during the entire operation.
    // The corresponding r/w bucket lock.
    SharedMutex & getMutex(BucketId bucket_id) const { return mutex_[bucket_id.toUnderType() & (kNumMutexes - 1)]; }

    BucketId getBucketId(HashedKey key) const { return BucketId{static_cast<UInt32>(key.keyHash() % num_buckets_)}; }

    UInt64 getBucketOffset(BucketId bucket_id) const { return cache_base_offset_ + bucket_size_ * bucket_id.toUnderType(); }

    void bfRebuild(BucketId bucket_id, const Bucket * bucket);
    bool bfReject(BucketId bucket_id, UInt64 key_hash) const;

    // Use birthday paradox to estimate number of mutexes given number of parallel
    // queries and desired probability of lock collision.
    static constexpr size_t kNumMutexes = 16 * 1024;

    // Serialization format version.
    static constexpr UInt32 kFormatVersion = 10;

    const ExpiredCheck check_expired_{};
    const DestructorCallback destructor_callback_{};
    const UInt64 bucket_size_{};
    const UInt64 cache_base_offset_{};
    const UInt64 num_buckets_{};
    std::unique_ptr<BloomFilter> bloom_filters_;
    std::chrono::nanoseconds generation_time_{};
    //    Device & device_;
    Device & device_;
    std::unique_ptr<SharedMutex[]> mutex_;

    mutable Poco::AtomicCounter item_count_;
    mutable Poco::AtomicCounter used_size_bytes_;

    static_assert((kNumMutexes & (kNumMutexes - 1)) == 0, "number of mutexes must be power of two");
};
}
