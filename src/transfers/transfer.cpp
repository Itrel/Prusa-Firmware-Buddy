#include "transfer.hpp"
#include "changed_path.hpp"
#include "download.hpp"

// TODO: use slot and poll from gui?
#include "gui/gui_media_events.hpp"

#include <common/timing.h>
#include <common/crc32.h>
#include <common/filename_type.hpp>
#include <common/bsod.h>
#include <common/unique_dir_ptr.hpp>
#include <common/print_utils.hpp>
#include <common/stat_retry.hpp>
#include <common/lfn.h>
#include <state/printer_state.hpp>
#include <option/has_human_interactions.h>

#include <log.h>
#include <type_traits>
#include <variant>
#include <optional>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

LOG_COMPONENT_REF(transfers);

using namespace transfers;
using std::is_same_v;
using std::optional;

Transfer::PlainGcodeDownloadOrder::PlainGcodeDownloadOrder(const PartialFile &file) {
    if (file.has_valid_head(HeadSize)) {
        if (file.has_valid_tail(TailSize)) {
            if (file.get_state().get_valid_size() == file.final_size()) {
                state = State::Finished;
            } else {
                state = State::DownloadedBase;
            }
        } else {
            state = State::DownloadingTail;
        }
    } else {
        state = State::DownloadingHeader;
    }
}

Transfer::Action Transfer::PlainGcodeDownloadOrder::step(const PartialFile &file) {
    switch (state) {
    case State::DownloadingHeader:
        if (file.has_valid_head(HeadSize)) {
            state = State::DownloadingTail;
            return Action::RangeJump;
        }
        return Action::Continue;
    case State::DownloadingTail:
        if (file.has_valid_tail(TailSize)) {
            state = State::DownloadedBase;
            return Action::RangeJump;
        }
        return Action::Continue;
    case State::DownloadedBase:
        state = State::DownloadingBody;
        return Action::Continue;
    case State::DownloadingBody:
        if (file.final_size() == file.get_state().get_valid_size()) {
            state = State::Finished;
            return Action::Finished;
        } else {
            return Action::Continue;
        }
    case State::Finished:
        return Action::Finished;
    default:
        fatal_error("unhandled state", "download");
    }
}

size_t Transfer::PlainGcodeDownloadOrder::get_next_offset(const PartialFile &file) const {
    switch (state) {
    case State::DownloadingHeader: {
        auto head = file.get_valid_head();
        return head.has_value() ? head->end : 0;
    }
    case State::DownloadingTail: {
        auto tail = file.get_valid_tail();
        log_info(transfers, "returning offset for tail: %i, %u, %u", tail.has_value(), tail->start, tail->end);
        return tail.has_value() ? tail->end : file.final_size() - TailSize;
    }
    case State::DownloadingBody:
    case State::DownloadedBase:
    case State::Finished:
        return file.get_valid_head()->end;
    default:
        fatal_error("unhandled state", "download");
    }
}

Transfer::Action Transfer::GenericFileDownloadOrder::step(const PartialFile &file) {
    if (file.final_size() == file.get_state().get_valid_size()) {
        return Action::Finished;
    } else {
        return Action::Continue;
    }
}

size_t Transfer::GenericFileDownloadOrder::get_next_offset(const PartialFile &file) const {
    auto head = file.get_valid_head();
    return head ? head->end : 0;
}

Transfer::BeginResult Transfer::begin(const char *destination_path, const Download::Request &request) {
    log_info(transfers, "Starting transfer of %s", destination_path);

    // allocate slot for the download
    auto slot = Monitor::instance.allocate(Monitor::Type::Connect, destination_path, 0, true);
    if (!slot.has_value()) {
        log_error(transfers, "Failed to allocate slot for %s", destination_path);
        return NoTransferSlot {};
    }

    // check the destination path does not exist
    struct stat st;
    if (stat_retry(destination_path, &st) == 0) {
        log_error(transfers, "Destination path %s already exists", destination_path);
        return AlreadyExists {};
    }

    // make a directory there
    if (mkdir(destination_path, 0777) != 0) {
        log_error(transfers, "Failed to create directory %s", destination_path);
        return Storage { "Failed to create directory" };
    }

    if (!store_transfer_index(destination_path)) {
        log_error(transfers, "Failed to store path to index");
        return Storage { "Failed to store path to index" };
    }

    // make the request
    auto path = Path(destination_path);
    // Create the backup file first to avoid a race condition (if we create the
    // partial file first, lose power, we would then think the file full of
    // garbage is _complete_).
    //
    // Then just close it and leave it empty until we have something to write into it.
    auto backup = unique_file_ptr(fopen(path.as_backup(), "w"));
    backup.reset();
    auto &&download = Download::begin(request, path.as_partial());
    log_info(transfers, "Download request initiated");

    return std::visit([&](auto &&arg) -> Transfer::BeginResult {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (is_same_v<T, transfers::Download>) {
            slot->update_expected_size(arg.file_size());
            // we got a valid response and can start downloading
            // so lets make a backup file for recovery
            auto backup_file = [&]() {
                auto transfer_path = Path(destination_path);
                return unique_file_ptr(fopen(transfer_path.as_backup(), "w+"));
            }();
            if (backup_file.get() == nullptr || Transfer::make_backup(backup_file.get(), request, arg.get_partial_file()->get_state(), *slot) == false) {
                return Storage { "Failed to create backup file" };
            }
            auto partial_file = arg.get_partial_file(); // get the partial file before we std::move the download away
            return Transfer(State::Downloading, std::move(arg), std::move(*slot), std::nullopt, partial_file);
        } else {
            log_error(transfers, "Failed to initiate download");
            // remove all the files we might have created
            remove(path.as_partial());
            remove(path.as_backup());
            rmdir(path.as_destination());
            return arg;
        }
    },
        download);
}

bool Transfer::restart_download() {
    auto backup_file = unique_file_ptr(fopen(path.as_backup(), "r"));
    if (backup_file.get() == nullptr) {
        log_error(transfers, "Failed to open backup file");
        last_connection_error_ms = ticks_ms();
        return false;
    }

    auto backup = Transfer::restore(backup_file.get());
    if (backup.has_value() == false) {
        log_error(transfers, "Failed to restore backup file");
        last_connection_error_ms = ticks_ms();
        return false;
    }

    auto request = backup->get_download_request();
    if (request.has_value() == false) {
        log_error(transfers, "Failed to get download request from backup file");
        last_connection_error_ms = ticks_ms();
        return false;
    }

    init_download_order_if_needed();
    // If the previous download attempt failed due to write error / timeout, don't carry that one to the next attempt.
    partial_file->reset_error();
    uint32_t position = std::visit([&](auto &&arg) { return arg.get_next_offset(*partial_file); }, *order);
    position = position / PartialFile::SECTOR_SIZE * PartialFile::SECTOR_SIZE; // ensure we start at a sector boundary

    optional<uint32_t> end_range;
    if (auto tail = partial_file->get_valid_tail(); tail.has_value()) {
        if (tail->end == partial_file->final_size() && position < tail->start) {
            // We can request not until the end of file, but until the
            // beginning of the tail - we'll stop there and have the complete
            // file by then (the tail is already all the way to the end).
            //
            // Note: end_range is _inclusive_ in the http (eg. range 0-4 will
            // return 5 bytes).
            assert(tail->start % PartialFile::SECTOR_SIZE == 0);
            end_range = tail->start - 1;
        }
    }

    auto &&download = Download::begin(*request, partial_file, position, end_range);

    log_info(transfers, "Download request initiated, position: %u", position);

    return std::visit([&](auto &&arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (is_same_v<T, transfers::Download>) {
            this->download = std::move(arg);
            return true;
        } else if constexpr (is_same_v<T, transfers::AlreadyExists>) {
            log_error(transfers, "Destination path %s already exists", slot.destination());
            last_connection_error_ms = ticks_ms();
            return false;
        } else if constexpr (is_same_v<T, transfers::RefusedRequest>) {
            log_error(transfers, "Download request refused");
            last_connection_error_ms = ticks_ms();
            return false;
        } else if constexpr (is_same_v<T, transfers::Storage>) {
            log_error(transfers, "Failed to download; storage: %s", arg.msg);
            last_connection_error_ms = ticks_ms();
            return false;
        } else {
            log_error(transfers, "Failed to restart download");
            last_connection_error_ms = ticks_ms();
            return false;
        }
    },
        download);
}

void Transfer::init_download_order_if_needed() {
    if (order.has_value()) {
        return;
    }
    bool is_plain_gcode = filename_is_plain_gcode(slot.destination());
    bool has_sufficient_size = partial_file->final_size() >= PlainGcodeDownloadOrder::MinimalFileSize;
    if (is_plain_gcode && has_sufficient_size) {
        order = DownloadOrder(PlainGcodeDownloadOrder(*partial_file));
    } else {
        order = DownloadOrder(GenericFileDownloadOrder());
    }
}

void Transfer::update_backup(bool force) {
    bool backup_outdated = last_backup_update_ms.has_value() == false || ticks_ms() - *last_backup_update_ms > BackupUpdateIntervalMs;
    if (force == false && !backup_outdated) {
        return;
    }

    unique_file_ptr backup_file(fopen(path.as_backup(), "r+"));
    if (backup_file.get() == nullptr) {
        log_error(transfers, "Failed to open backup file for update");
        return;
    }

    if (Transfer::update_backup(backup_file.get(), partial_file->get_state()) == false) {
        log_error(transfers, "Failed to update backup file");
    } else {
        log_info(transfers, "Backup file updated");
    }
    last_backup_update_ms = ticks_ms();
}

std::optional<struct stat> Transfer::get_transfer_partial_file_stat(MutablePath &destination_path) {
    if (!is_valid_transfer(destination_path)) {
        return std::nullopt;
    }

    destination_path.push(partial_filename);
    struct stat st = {};
    int result = stat_retry(destination_path.get(), &st);
    destination_path.pop();
    if (result == 0) {
        return st;
    } else {
        return std::nullopt;
    }
}

Transfer::RecoverResult Transfer::recover(const char *destination_path) {
    Path path(destination_path);

    std::optional<RestoredTransfer> backup = std::nullopt;

    PartialFile::State partial_file_state;
    {
        auto backup_file = unique_file_ptr(fopen(path.as_backup(), "r"));
        if (backup_file.get() == nullptr) {
            log_error(transfers, "Failed to open backup file");
            return Storage { "Failed to open backup file" };
        }

        backup = Transfer::restore(backup_file.get());
        if (backup.has_value() == false) {
            log_error(transfers, "Failed to restore backup file");
            return Storage { "Failed to restore backup file" };
        }
        partial_file_state = backup->get_partial_file_state();
    }

    // reopen the partial file
    PartialFile::Ptr partial_file = nullptr;
    {
        auto partial_file_result = PartialFile::open(path.as_partial(), partial_file_state);
        if (auto *err = get_if<const char *>(&partial_file_result); err != nullptr) {
            log_error(transfers, "Failed to open partial file: %s", *err);
            return Storage { *err };
        } else {
            partial_file = std::get<PartialFile::Ptr>(partial_file_result);
        }
    }

    // allocate slot for the transfer
    auto slot = Monitor::instance.allocate(Monitor::Type::Connect, destination_path, partial_file->final_size(), false, backup->id);
    if (!slot.has_value()) {
        log_error(transfers, "Failed to allocate slot for %s", destination_path);
        return NoTransferSlot {};
    }

    slot->progress(partial_file_state, false);

    return Transfer(Transfer::State::Retrying, std::nullopt, std::move(*slot), std::nullopt, partial_file);
}

Transfer::Transfer(State state, std::optional<Download> &&download, Monitor::Slot &&slot, std::optional<DownloadOrder> &&order, PartialFile::Ptr partial_file)
    : slot(std::move(slot))
    , download(std::move(download))
    , path(slot.destination())
    , order(order)
    , state(state)
    , partial_file(partial_file)
    , is_printable(filename_is_printable(slot.destination())) {
    assert(partial_file.get() != nullptr);
}

Transfer::State Transfer::step(bool is_printing) {
    switch (state) {
    case State::Downloading:
    case State::Retrying: {
        if (slot.is_stopped()) {
            done(State::Failed, Monitor::Outcome::Stopped);
        } else if (download.has_value()) {
            switch (download->step()) {
            case DownloadStep::Continue: {
                slot.progress(partial_file->get_state(), false);
                update_backup(/*force=*/false);
                init_download_order_if_needed();
                Transfer::Action next_step = std::visit([&](auto &&arg) { return arg.step(*partial_file); }, *order);
                switch (next_step) {
                case Transfer::Action::Continue:
                    if (is_printable && !already_notified) {
                        notify_created();
                    }
                    break;
                case Transfer::Action::RangeJump:
                    download.reset();
                    // So we don't "lose" part of the already downloaded file, for showing on screen, etc.
                    update_backup(/*force=*/true);
                    restart_requested_by_jump = true;
                    break;
                case Transfer::Action::Finished:
                    done(State::Finished, Monitor::Outcome::Finished);
                    break;
                }
                break;
            }
            case DownloadStep::FailedNetwork:
                recoverable_failure(is_printing);
                break;
            case DownloadStep::FailedOther:
                done(State::Failed, Monitor::Outcome::Error);
                break;
            case DownloadStep::Finished:
                download.reset();
                break;
            case DownloadStep::Aborted:
                // Unreachable - this is only after we've called the deleter
                assert(0);
                break;
            }
        } else if (last_connection_error_ms.has_value() == false || ticks_ms() - *last_connection_error_ms > 1000) {
            slot.progress(partial_file->get_state(), !restart_requested_by_jump);
            restart_requested_by_jump = false;
            if (!restart_download()) {
                // OK, some of them are probably not recoverable (eg. someone
                // has eaten the backup file at runtime), but also not expected
                // to generally happen in practice, so it's probably fine to
                // just try multiple times in that case before giving up
                // completely.
                recoverable_failure(is_printing);
            }
        }
        break;
    }
    case State::Finished:
    case State::Failed:
        break;
    }
    return state;
}

void Transfer::notify_created() {
    ChangedPath::instance.changed_path(slot.destination(), ChangedPath::Type::File, ChangedPath::Incident::Created);

    if (HAS_HUMAN_INTERACTIONS() && filename_is_printable(slot.destination()) && printer_state::remote_print_ready(/*preview_only=*/true)) {
        // While it looks a counter-intuitive, this print_begin only shows the
        // print preview / one click print, doesn't really start the print.
        print_begin(slot.destination(), false);
    }

    already_notified = true;
}

bool Transfer::cleanup_transfers() {
    auto index = unique_file_ptr(fopen(transfer_index, "r"));
    if (!index) {
        return false;
    }

    Path transfer_path;

    bool all_ok = true;
    bool can_cleanup = true;

    for (;;) {
        switch (next_in_index(index, transfer_path)) {
        case IndexIter::Ok: {
            struct stat st;
            // check the existence of the download file
            bool backup_file_found = stat_retry(transfer_path.as_backup(), &st) == 0 && S_ISREG(st.st_mode);
            bool backup_is_empty = backup_file_found && st.st_size == 0;
            bool partial_file_found = stat_retry(transfer_path.as_partial(), &st) == 0 && S_ISREG(st.st_mode);

            if (partial_file_found && !backup_file_found) {
                if (!cleanup_finalize(transfer_path)) {
                    all_ok = false;
                }
            } else if (partial_file_found && backup_is_empty) {
                if (!cleanup_remove(transfer_path)) {
                    all_ok = false;
                }
            } else if (partial_file_found && backup_file_found) { // && is not empty...
                // This one is still "in progress"
                can_cleanup = false;
            }
            break;
        }
        case IndexIter::Skip:
            continue;
        case IndexIter::IndividualError:
            all_ok = false;
            continue;
        case IndexIter::FatalError:
            all_ok = false;
            [[fallthrough]];
        case IndexIter::Eof:
            goto DONE; // break, but from the cycle
        }
    }

DONE:
    if (all_ok && can_cleanup) {
        // Close file so we can remove it.
        //
        // Note: There's a short race condition - if between we close it and
        // delete it, another transfer starts in Link and gets written in the
        // file, we lose it (once Link also starts using partial files). That's
        // probably rare and not a catastrophic failure.
        index.reset();
        remove(transfer_index);
    }

    return all_ok;
}

void Transfer::recoverable_failure(bool is_printing) {
    if (retries_left > 0) {
        if (!is_printing) {
            // We want to make sure not to give up on downloading
            // the file that is being printed. This is much broader
            // (we won't give up on downloading some other
            // completely unrelated file too), but that's probably
            // fine and we don't want the complexity of plumbing
            // all the details about what is being printed, what is
            // being downloaded and if these are in fact the same
            // files (considering every segment of the path might
            // be either LFN or SFN).
            retries_left--;
        }
        slot.progress(partial_file->get_state(), true);
        state = State::Retrying;
        restart_requested_by_jump = false;
        download.reset();
    } else {
        done(State::Failed, Monitor::Outcome::Error);
    }
}

void Transfer::done(State state, Monitor::Outcome outcome) {
    this->state = state;
    download.reset();
    partial_file.reset();
    if (state == State::Finished) {
        remove(path.as_backup());
        if (!is_printable) {
            // We don't dare move printable files at arbitrary times, because
            // they can already be printed. But we must move the other files
            // before we notify about them.
            cleanup_finalize(path);
        }
    } else {
        // FIXME: We need some kind of error handling strategy to deal with
        // failed transfers. But for now, we just need to make 100% sure
        // not to mark the download as „successfully“ finished. So we mark
        // it as failed by having an empty backup file.

        // (Overwrite the file with empty one by opening and closing right away).
        unique_file_ptr(fopen(path.as_backup(), "w"));
    }
    slot.done(outcome);

    log_info(transfers, "Transfer %s", state == State::Failed ? "failed" : "finished");
}

bool Transfer::cleanup_finalize(Path &transfer_path) {
    // move the partial file to temporary location
    const char *temporary_filename = "/usb/prusa-temporary-file.gcode";
    remove(temporary_filename); // remove the file if there is some leftover already

    char SFN[FILE_PATH_BUFFER_LEN];
    strlcpy(SFN, transfer_path.as_destination(), sizeof(SFN));
    get_SFN_path(SFN);
    uint32_t old_SFN_crc = crc32_calc((const uint8_t *)SFN, sizeof(SFN));
    if (rename(transfer_path.as_partial(), temporary_filename) != 0) {
        log_error(transfers, "Failed to move partial file to temporary location");
        return false;
    }
    // remove the transfer directory
    if (rmdir(transfer_path.as_destination()) != 0) {
        log_error(transfers, "Failed to remove transfer directory");
        return false;
    }
    if (rename(temporary_filename, transfer_path.as_destination()) != 0) {
        log_error(transfers, "Failed to move temporary file to final location");
        return false;
    }

    strlcpy(SFN, transfer_path.as_destination(), sizeof(SFN));
    get_SFN_path(SFN);
    uint32_t new_SFN_crc = crc32_calc((const uint8_t *)SFN, sizeof(SFN));

    if (old_SFN_crc != new_SFN_crc) {
        // if SFN changed, trigger a rescan of the whole folder
        ChangedPath::instance.changed_path(transfer_path.as_destination(), ChangedPath::Type::File, ChangedPath::Incident::Deleted);
        ChangedPath::instance.changed_path(transfer_path.as_destination(), ChangedPath::Type::File, ChangedPath::Incident::Created);
    } else {
        // else just send the FILE_INFO, to notify connect, that the filke is not read_only anymore
        ChangedPath::instance.changed_path(transfer_path.as_destination(), ChangedPath::Type::File, ChangedPath::Incident::Created);
    }
    log_info(transfers, "Transfer %s cleaned up", transfer_path.as_destination());

    return true;
}

bool Transfer::cleanup_remove(Path &path) {
    // Note: Order of removal is important. It is possible the partial can't be
    // removed (eg. because it's being shown as a preview, or being printed).
    // In such case we want to make sure _not_ to delete the (possibly failed)
    // backup.
    bool success = (remove(path.as_partial()) == 0) && (remove(path.as_backup()) == 0) && (rmdir(path.as_destination()) == 0);

    if (success) {
        ChangedPath::instance.changed_path(path.as_destination(), ChangedPath::Type::File, ChangedPath::Incident::Deleted);
    }
    return success;
}

bool Transfer::store_transfer_index(const char *path) {
    auto index = unique_file_ptr(fopen(transfer_index, "a"));
    if (index.get() == nullptr) {
        return false;
    }

    return fprintf(index.get(), "%s\n", path) > 0; // Returns negative on error
    // auto-closed by unique_file_ptr
}
