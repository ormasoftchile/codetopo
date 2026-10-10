#pragma once

#include "db/connection.h"
#include <filesystem>
#include <string>
#include <system_error>

#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

namespace codetopo {

// Fast Copy-on-Write (CoW) file cloning.
// On macOS APFS uses ::clonefile() for instant, zero-byte block sharing.
// On Linux uses FICLONE ioctl for Btrfs/XFS/ZFS reflinks.
// Gracefully falls back to standard file copy if CoW is unsupported or cross-device.
inline bool clone_file_cow(const std::filesystem::path& src, const std::filesystem::path& dst) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(src, ec) || ec) return false;

    // Ensure parent directory of destination exists
    if (dst.has_parent_path()) {
        fs::create_directories(dst.parent_path(), ec);
        if (ec) return false;
    }

    // Remove existing destination file (clonefile/FICLONE require dst not exist / be truncated)
    if (fs::exists(dst, ec)) {
        fs::remove(dst, ec);
    }

#if defined(__APPLE__)
    int rc = ::clonefile(src.c_str(), dst.c_str(), 0);
    if (rc == 0) return true;
    // On failure (e.g. cross-volume, EXDEV, ENOTSUP), fall through to standard copy
#elif defined(__linux__) && defined(FICLONE)
    int src_fd = ::open(src.c_str(), O_RDONLY);
    if (src_fd >= 0) {
        int dst_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd >= 0) {
            int ret = ::ioctl(dst_fd, FICLONE, src_fd);
            ::close(dst_fd);
            ::close(src_fd);
            if (ret == 0) return true;
        } else {
            ::close(src_fd);
        }
    }
#endif

    // Universal fallback: standard streaming file copy
    return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) && !ec;
}

// Atomically clones an index SQLite database and its WAL file (if present)
// to a new location, resetting any locks/shared memory, and checkpointing
// the destination database to ensure self-contained consistency.
inline bool clone_database_cow(const std::filesystem::path& src_db,
                               const std::filesystem::path& dst_db) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(src_db, ec) || ec) return false;

    // 1. Ensure parent directory exists
    if (dst_db.has_parent_path()) {
        fs::create_directories(dst_db.parent_path(), ec);
        if (ec) return false;
    }

    // 2. Clone main database file
    if (!clone_file_cow(src_db, dst_db)) {
        return false;
    }

    // 3. Clone WAL file if present and non-empty
    fs::path src_wal = src_db.string() + "-wal";
    fs::path dst_wal = dst_db.string() + "-wal";
    if (fs::exists(src_wal, ec) && !ec && fs::file_size(src_wal, ec) > 0 && !ec) {
        clone_file_cow(src_wal, dst_wal);
    } else if (fs::exists(dst_wal, ec)) {
        fs::remove(dst_wal, ec);
    }

    // 4. Ensure no stale destination -shm or .lock remains
    fs::path dst_shm = dst_db.string() + "-shm";
    if (fs::exists(dst_shm, ec)) fs::remove(dst_shm, ec);
    fs::path dst_lock = dst_db.string() + ".lock";
    if (fs::exists(dst_lock, ec)) fs::remove(dst_lock, ec);

    // 5. Open destination database and fold any copied WAL pages into the main file
    try {
        Connection conn(dst_db.string());
        conn.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (...) {
        // Non-fatal if checkpoint fails; SQLite will replay WAL on next open
    }

    return true;
}

} // namespace codetopo
