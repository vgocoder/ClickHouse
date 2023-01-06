#pragma once

#include <boost/noncopyable.hpp>
#include <Interpreters/Cache/FileCacheKey.h>
#include <Interpreters/Cache/Guards.h>

#include <IO/WriteBufferFromFile.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>
#include <IO/OpenedFileCache.h>
#include <base/getThreadId.h>
#include <list>
#include <queue>


namespace Poco { class Logger; }

namespace CurrentMetrics
{
extern const Metric CacheFileSegments;
}

namespace DB
{

class FileCache;
class ReadBufferFromFileBase;

class FileSegment;
using FileSegmentPtr = std::shared_ptr<FileSegment>;
using FileSegments = std::list<FileSegmentPtr>;
struct KeyTransaction;
using KeyTransactionPtr = std::shared_ptr<KeyTransaction>;
struct KeyTransactionCreator;
using KeyTransactionCreatorPtr = std::unique_ptr<KeyTransactionCreator>;

/*
 * FileSegmentKind is used to specify the eviction policy for file segments.
 */
enum class FileSegmentKind
{
    /* `Regular` file segment is still in cache after usage, and can be evicted
     * (unless there're some holders).
     */
    Regular,

    /* `Persistent` file segment can't be evicted from cache,
     * it should be removed manually.
     */
    Persistent,

    /* `Temporary` file segment is removed right after releasing.
     * Also corresponding files are removed during cache loading (if any).
     */
    Temporary,
};

String toString(FileSegmentKind kind);

struct CreateFileSegmentSettings
{
    FileSegmentKind kind = FileSegmentKind::Regular;
    bool unbounded = false;

    CreateFileSegmentSettings() = default;

    explicit CreateFileSegmentSettings(FileSegmentKind kind_, bool unbounded_ = false)
        : kind(kind_), unbounded(unbounded_)
    {}
};

class FileSegment : private boost::noncopyable, public std::enable_shared_from_this<FileSegment>
{

friend class FileCache;
friend struct FileSegmentsHolder;
friend class FileSegmentRangeWriter;
friend class StorageSystemFilesystemCache;
friend struct KeyTransaction;

public:
    using Key = FileCacheKey;
    using RemoteFileReaderPtr = std::shared_ptr<ReadBufferFromFileBase>;
    using LocalCacheWriterPtr = std::unique_ptr<WriteBufferFromFile>;
    using Downloader = std::string;
    using DownloaderId = std::string;

    enum class State
    {
        DOWNLOADED,
        /**
         * When file segment is first created and returned to user, it has state EMPTY.
         * EMPTY state can become DOWNLOADING when getOrSetDownaloder is called successfully
         * by any owner of EMPTY state file segment.
         */
        EMPTY,
        /**
         * A newly created file segment never has DOWNLOADING state until call to getOrSetDownloader
         * because each cache user might acquire multiple file segments and reads them one by one,
         * so only user which actually needs to read this segment earlier than others - becomes a downloader.
         */
        DOWNLOADING,
        /**
         * Space reservation for a file segment is incremental, i.e. downloader reads buffer_size bytes
         * from remote fs -> tries to reserve buffer_size bytes to put them to cache -> writes to cache
         * on successful reservation and stops cache write otherwise. Those, who waited for the same file
         * segment, will read downloaded part from cache and remaining part directly from remote fs.
         */
        PARTIALLY_DOWNLOADED_NO_CONTINUATION,
        /**
         * If downloader did not finish download of current file segment for any reason apart from running
         * out of cache space, then download can be continued by other owners of this file segment.
         */
        PARTIALLY_DOWNLOADED,
        /**
         * If file segment cannot possibly be downloaded (first space reservation attempt failed), mark
         * this file segment as out of cache scope.
         */
        SKIP_CACHE,
    };

    FileSegment(
        size_t offset_,
        size_t size_,
        const Key & key_,
        KeyTransactionCreatorPtr key_transaction_creator,
        FileCache * cache_,
        State download_state_,
        const CreateFileSegmentSettings & create_settings);

    State state() const;

    static String stateToString(FileSegment::State state);

    /// Represents an interval [left, right] including both boundaries.
    struct Range
    {
        size_t left;
        size_t right;

        Range(size_t left_, size_t right_) : left(left_), right(right_) {}

        bool operator==(const Range & other) const { return left == other.left && right == other.right; }

        size_t size() const { return right - left + 1; }

        String toString() const { return fmt::format("[{}, {}]", std::to_string(left), std::to_string(right)); }
    };

    static String getCallerId();

    String getInfoForLog() const;

    /**
     * ========== Methods to get file segment's constant state ==================
     */

    const Range & range() const { return segment_range; }

    const Key & key() const { return file_key; }

    size_t offset() const { return range().left; }

    FileSegmentKind getKind() const { return segment_kind; }
    bool isPersistent() const { return segment_kind == FileSegmentKind::Persistent; }

    using UniqueId = std::pair<FileCacheKey, size_t>;
    UniqueId getUniqueId() const { return std::pair(key(), offset()); }

    String getPathInLocalCache() const { return file_path; }

    /**
     * ========== Methods for _any_ file segment's owner ========================
     */

    String getOrSetDownloader();

    bool isDownloader() const;

    DownloaderId getDownloader() const;

    /// Wait for the change of state from DOWNLOADING to any other.
    State wait();

    bool isDownloaded() const;

    size_t getHitsCount() const { return hits_count; }

    size_t getRefCount() const { return ref_count; }

    void incrementHitsCount() { ++hits_count; }

    size_t getCurrentWriteOffset() const;

    size_t getFirstNonDownloadedOffset() const;

    size_t getDownloadedSize() const;

    /// Now detached status can be used in the following cases:
    /// 1. there is only 1 remaining file segment holder
    ///    && it does not need this segment anymore
    ///    && this file segment was in cache and needs to be removed
    /// 2. in read_from_cache_if_exists_otherwise_bypass_cache case to create NOOP file segments.
    /// 3. removeIfExists - method which removes file segments from cache even though
    ///    it might be used at the moment.

    /// If file segment is detached it means the following:
    /// 1. It is not present in FileCache, e.g. will not be visible to any cache user apart from
    /// those who acquired shared pointer to this file segment before it was detached.
    /// 2. Detached file segment can still be hold by some cache users, but it's state became
    /// immutable at the point it was detached, any non-const / stateful method will throw an
    /// exception.
    void detach(const FileSegmentGuard::Lock &, const KeyTransaction &);

    static FileSegmentPtr getSnapshot(const FileSegmentPtr & file_segment);

    bool isDetached() const;

    bool isCompleted() const { return is_completed; }

    void assertCorrectness() const;

    /**
     * ========== Methods for _only_ file segment's `downloader` ==================
     */

    /// Try to reserve exactly `size` bytes.
    /// Returns true if reservation was successful, false otherwise.
    bool reserve(size_t size_to_reserve);

    /// Try to reserve at max `size_to_reserve` bytes.
    /// Returns actual size reserved. It can be less than size_to_reserve in non strict mode.
    /// In strict mode throws an error on attempt to reserve space too much space.
    size_t tryReserve(size_t size_to_reserve, bool strict = false);

    /// Write data into reserved space.
    void write(const char * from, size_t size, size_t offset);

    void setBroken();

    void complete();

    /// Complete file segment's part which was last written.
    void completePartAndResetDownloader();

    void resetDownloader();

    RemoteFileReaderPtr getRemoteFileReader();

    RemoteFileReaderPtr extractRemoteFileReader();

    void setRemoteFileReader(RemoteFileReaderPtr remote_file_reader_);

    void resetRemoteFileReader();

    FileSegmentGuard::Lock lock() const { return segment_guard.lock(); }

    void setDownloadedSize(size_t delta);

private:
    size_t getFirstNonDownloadedOffsetUnlocked(const FileSegmentGuard::Lock &) const;
    size_t getCurrentWriteOffsetUnlocked(const FileSegmentGuard::Lock &) const;
    size_t getDownloadedSizeUnlocked(const FileSegmentGuard::Lock &) const;

    String getInfoForLogUnlocked(const FileSegmentGuard::Lock &) const;

    String getDownloaderUnlocked(const FileSegmentGuard::Lock &) const;
    void resetDownloaderUnlocked(const FileSegmentGuard::Lock &);
    void resetDownloadingStateUnlocked(const FileSegmentGuard::Lock &);

    void setDownloadState(State state, const FileSegmentGuard::Lock &);
    void setDownloadedSizeUnlocked(size_t delta, const FileSegmentGuard::Lock &);

    void setDownloadedUnlocked(const FileSegmentGuard::Lock &);
    void setDownloadFailedUnlocked(const FileSegmentGuard::Lock &);

    /// Finalized state is such a state that does not need to be completed (with complete()).
    bool hasFinalizedStateUnlocked(const FileSegmentGuard::Lock &) const;

    bool isDetached(const FileSegmentGuard::Lock &) const { return is_detached; }
    void detachAssumeStateFinalized(const FileSegmentGuard::Lock &);
    [[noreturn]] void throwIfDetachedUnlocked(const FileSegmentGuard::Lock &) const;

    void assertDetachedStatus(const FileSegmentGuard::Lock &) const;
    void assertNotDetached() const;
    void assertNotDetachedUnlocked(const FileSegmentGuard::Lock &) const;
    void assertIsDownloaderUnlocked(const std::string & operation, const FileSegmentGuard::Lock &) const;
    void assertCorrectnessUnlocked(const FileSegmentGuard::Lock &) const;

    KeyTransactionPtr createKeyTransaction(bool assert_exists = true) const;

    /// completeWithoutStateUnlocked() is called from destructor of FileSegmentsHolder.
    /// Function might check if the caller of the method
    /// is the last alive holder of the segment. Therefore, completion and destruction
    /// of the file segment pointer must be done under the same cache mutex.
    void completeUnlocked(KeyTransaction & key_transaction);

    void completePartAndResetDownloaderUnlocked(const FileSegmentGuard::Lock & segment_lock);
    bool isDownloaderUnlocked(const FileSegmentGuard::Lock & segment_lock) const;

    void wrapWithCacheInfo(
        Exception & e, const String & message, const FileSegmentGuard::Lock & segment_lock) const;

    Range segment_range;

    State download_state;

    /// The one who prepares the download
    DownloaderId downloader_id;

    RemoteFileReaderPtr remote_file_reader;
    LocalCacheWriterPtr cache_writer;

    /// downloaded_size should always be less or equal to reserved_size
    size_t downloaded_size = 0;
    size_t reserved_size = 0;

    /// global locking order rule:
    /// 1. cache lock
    /// 2. segment lock

    mutable FileSegmentGuard segment_guard;
    KeyTransactionCreatorPtr key_transaction_creator;
    std::condition_variable cv;

    /// Protects downloaded_size access with actual write into fs.
    /// downloaded_size is not protected by download_mutex in methods which
    /// can never be run in parallel to FileSegment::write() method
    /// as downloaded_size is updated only in FileSegment::write() method.
    /// Such methods are identified by isDownloader() check at their start,
    /// e.g. they are executed strictly by the same thread, sequentially.
    mutable std::mutex download_mutex;

    Key file_key;
    const std::string file_path;
    FileCache * cache;

    Poco::Logger * log;

    /// "detached" file segment means that it is not owned by cache ("detached" from cache).
    /// In general case, all file segments are owned by cache.
    bool is_detached = false;
    std::atomic<bool> is_completed = false;

    bool is_downloaded = false;

    std::atomic<size_t> hits_count = 0; /// cache hits.
    std::atomic<size_t> ref_count = 0; /// Used for getting snapshot state

    FileSegmentKind segment_kind;

    /// Size of the segment is not known until it is downloaded and can be bigger than max_file_segment_size.
    bool is_unbound = false;

    CurrentMetrics::Increment metric_increment{CurrentMetrics::CacheFileSegments};
};

struct FileSegmentsHolder : private boost::noncopyable
{
    FileSegmentsHolder() = default;

    explicit FileSegmentsHolder(FileSegments && file_segments_) : file_segments(std::move(file_segments_)) {}

    ~FileSegmentsHolder();

    bool empty() const { return file_segments.empty(); }

    size_t size() const { return file_segments.size(); }

    String toString();

    void popFront() { completeAndPopFrontImpl(); }

    FileSegment & front() { return *file_segments.front(); }

    FileSegment & back() { return *file_segments.back(); }

    FileSegment & add(FileSegmentPtr && file_segment)
    {
        file_segments.push_back(file_segment);
        return *file_segments.back();
    }

    FileSegments::iterator begin() { return file_segments.begin(); }
    FileSegments::iterator end() { return file_segments.end(); }

    FileSegments::const_iterator begin() const { return file_segments.begin(); }
    FileSegments::const_iterator end() const { return file_segments.end(); }

    void moveTo(FileSegmentsHolder & holder)
    {
        holder.file_segments.insert(holder.file_segments.end(), file_segments.begin(), file_segments.end());
        file_segments.clear();
    }

private:
    FileSegments file_segments{};

    FileSegments::iterator completeAndPopFrontImpl();
};

using FileSegmentsHolderPtr = std::unique_ptr<FileSegmentsHolder>;

}
