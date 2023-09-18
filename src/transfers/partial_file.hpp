#pragma once
#include <memory>
#include <variant>
#include <optional>
#include <array>
#include <stdint.h>

#include <common/unique_file_ptr.hpp>

namespace transfers {

/// Partial File manages a FatFS file that can be read & written at the same time.
///
/// - The file is always contiguous on the drive.
///     - This makes a lot of things easier, but we won't be able to use all the space on the drive if it's fragmented.
/// - The file, once created, can be read by standard means (fread etc)
///     - To observe which parts of the file are valid for reading, use the `get_valid_head` and `get_valid_tail` methods.
/// - To write to the file, use the `write`, `seek` and `flush` methods.
///     - The file remembers which parts of the file are valid. But there are limitations in order to keep the implementation simple.
///     - It remembers up to 2 valid independent parts. No more.
///     - One of them is called the "head", which is a part starting at offset 0.
///     - Second one is called the "tail" and it's a part starting somewhere in the middle of the
///         file (gradually growing to the end of the file).
///     - Creating a third valid part (by writing somewhere in between the head and the tail, for example) is not allowed.
///     - Therefore, every write should either extend the head or the tail.
///     - At some point, when the head and the tail meet, they are merged (tail is extended to the start of the file
///         and head is extended to the end of the file).
///     - Writing uses low-level USB functions and has basic buffering implemented by this class. Some requirements:
///         - seek() is allowed only to the start of a sector
///         - consecutive writes gradually fill the sector
///         - when a sector is fully written to, it's flushed to the drive
///         - seek()  to a different sector while the current one hasn't been fully written to will discard the currently buffered data
class PartialFile {
public:
    static const size_t SECTOR_SIZE = 512;

    struct ValidPart {
        size_t start;
        size_t end; // exclusive

        void merge(const ValidPart &other) {
            // this:  oooox
            // other:     oooox
            if (other.start <= end && other.end > end) {
                // extend to the right
                end = other.end;
            }
            // this:        oooox
            // other:   oooox
            // other:      ox
            if (other.start < start && other.end >= start) {
                // extend to the left
                start = other.start;
            }
        }
    };

    struct State {
        std::optional<ValidPart> valid_head;
        std::optional<ValidPart> valid_tail;
        size_t total_size;

        size_t get_valid_size() const {
            size_t bytes_head = valid_head.has_value() ? valid_head->end - valid_head->start : 0;
            size_t bytes_tail = valid_tail.has_value() ? valid_tail->end - valid_tail->start : 0;
            size_t bytes_overlap = 0;
            if (valid_head.has_value() && valid_tail.has_value() && valid_head->end > valid_tail->start) {
                bytes_overlap = valid_head->end - valid_tail->start;
            }
            return bytes_head + bytes_tail - bytes_overlap;
        }

        size_t get_percent_valid() const {
            // Needs to be calculated in float because (100 * size) overflows size_t
            return total_size ? static_cast<float>(get_valid_size()) * 100.0f / total_size : 0;
        }

        void extend_head(size_t bytes) {
            if (valid_head.has_value()) {
                valid_head->end += bytes;
            } else {
                valid_head = { 0, bytes };
            }
        }
    };

private:
    using DriveNbr = uint8_t;
    using SectorNbr = uint32_t;
    using SectorData = std::array<uint8_t, SECTOR_SIZE>;

    struct Sector {
        SectorNbr nbr;
        SectorData data;
    };

    /// USB drive number (LUN)
    DriveNbr drive;

    /// USB sector number where the first data of the file are located
    SectorNbr first_sector_nbr;

    /// Write buffer for the active sector the user is writing to
    std::optional<Sector> current_sector;

    /// Offset ("ftell") within the file where the user will write next
    size_t current_offset;

    /// Valid parts of the file
    State state;

    /// Last reported progress over logs
    int last_progress_percent;

    /// Translate file offset to sector number
    SectorNbr get_sector_nbr(size_t offset);

    /// Translate sector number to file offset
    size_t get_offset(SectorNbr sector_nbr);

    /// Write given sector over USB to the FatFS drive
    bool write_sector(const Sector &sector);

    /// Extend the valid_head and/or valid_tail to include the new_part
    void extend_valid_part(ValidPart new_part);

    /// Keeping a read-only open file.
    ///
    /// This is to lock the file in place so somebody doesn't accidentally
    /// delete it or mess with it in a different way.
    ///
    /// (Using fd instead of FILE * here because it's more lightweight and we
    /// don't actually _use_ it for anything).
    int file_lock;

public:
    PartialFile(DriveNbr drive, SectorNbr first_sector, State state, int file_lock);

    ~PartialFile();

    using Ptr = std::shared_ptr<PartialFile>;

    /// Try to create a new partial file of preallocated size
    static std::variant<const char *, PartialFile::Ptr> create(const char *path, size_t size);

    /// Open existing partial file
    ///
    /// state.total_size is updated according to what is found on the disk and overwritten.
    static std::variant<const char *, PartialFile::Ptr> open(const char *path, State state);

    /// Convert an open FILE * into this.
    ///
    /// state.total_size is updated according to what is found on the disk and overwritten.
    static std::variant<const char *, PartialFile::Ptr> convert(const char *path, unique_file_ptr file, State state);

    /// Seek to a given offset within the file
    bool seek(size_t offset);

    /// Write data to the file at current offset
    bool write(const uint8_t *data, size_t size);

    /// Flush the current sector to the USB drive
    bool sync();

    /// Get the final size of the file
    size_t final_size() const { return state.total_size; }

    /// Get the valid part of the file starting at offset 0
    std::optional<ValidPart> get_valid_head() const { return state.valid_head; }

    /// Get the valid part of the file starting passed the head
    std::optional<ValidPart> get_valid_tail() const { return state.valid_tail.has_value() ? state.valid_tail : std::nullopt; }

    /// Check if the file has valid data at [0, bytes) range
    bool has_valid_head(size_t bytes) const;

    /// Check if the file has valid data at [file_size - bytes, file_size) range
    bool has_valid_tail(size_t bytes) const;

    State get_state() const { return state; }

    void print_progress();
};

} // namespace transfers
