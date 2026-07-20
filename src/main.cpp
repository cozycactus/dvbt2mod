// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif

#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/digital/ofdm_cyclic_prefixer.h>
#include <gnuradio/dtv/dvb_bbheader_bb.h>
#include <gnuradio/dtv/dvb_bbscrambler_bb.h>
#include <gnuradio/dtv/dvb_bch_bb.h>
#include <gnuradio/dtv/dvb_config.h>
#include <gnuradio/dtv/dvb_ldpc_bb.h>
#include <gnuradio/dtv/dvbs2_config.h>
#include <gnuradio/dtv/dvbt2_cellinterleaver_cc.h>
#include <gnuradio/dtv/dvbt2_config.h>
#include <gnuradio/dtv/dvbt2_framemapper_cc.h>
#include <gnuradio/dtv/dvbt2_freqinterleaver_cc.h>
#include <gnuradio/dtv/dvbt2_interleaver_bb.h>
#include <gnuradio/dtv/dvbt2_modulator_bc.h>
#include <gnuradio/dtv/dvbt2_p1insertion_cc.h>
#include <gnuradio/dtv/dvbt2_paprtr_cc.h>
#include <gnuradio/dtv/dvbt2_pilotgenerator_cc.h>
#include <gnuradio/gr_complex.h>
#include <gnuradio/io_signature.h>
#include <gnuradio/logger.h>
#include <gnuradio/sync_block.h>
#include <gnuradio/top_block.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace dtv = gr::dtv;

namespace {

namespace platform {

fs::path path_from_utf8(const std::string& path)
{
#if defined(_WIN32)
    return fs::u8path(path);
#else
    return fs::path(path);
#endif
}

#if defined(_WIN32)

int duplicate_standard_stream(DWORD standard_handle_id, int access_mode)
{
    const HANDLE standard_handle = ::GetStdHandle(standard_handle_id);
    if (standard_handle == nullptr || standard_handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    HANDLE duplicate_handle = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(),
                          standard_handle,
                          ::GetCurrentProcess(),
                          &duplicate_handle,
                          0,
                          FALSE,
                          DUPLICATE_SAME_ACCESS) == 0) {
        errno = EIO;
        return -1;
    }

    if (access_mode == _O_WRONLY &&
        ::GetFileType(duplicate_handle) == FILE_TYPE_PIPE) {
        DWORD pipe_mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (::SetNamedPipeHandleState(
                duplicate_handle, &pipe_mode, nullptr, nullptr) == 0) {
            ::CloseHandle(duplicate_handle);
            errno = EIO;
            return -1;
        }
    }

    // Adopt an independently owned copy of the actual Win32 standard HANDLE.
    // The descriptor is binary for the few CRT operations that own its lifetime;
    // payload I/O below still goes directly through ReadFile/WriteFile.
    const int duplicate = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(duplicate_handle), access_mode | _O_BINARY);
    if (duplicate < 0) {
        const int saved_errno = errno;
        ::CloseHandle(duplicate_handle);
        errno = saved_errno;
        return -1;
    }
    return duplicate;
}

int duplicate_stdin()
{
    return duplicate_standard_stream(STD_INPUT_HANDLE, _O_RDONLY);
}

int duplicate_stdout()
{
    return duplicate_standard_stream(STD_OUTPUT_HANDLE, _O_WRONLY);
}

int open_input(const std::string& path)
{
    return ::_wopen(path_from_utf8(path).c_str(), _O_RDONLY | _O_BINARY);
}

int open_output(const std::string& path, bool overwrite)
{
    // In overwrite mode truncation is deliberately deferred until the opened
    // INPUT and OUTPUT handles have been compared for identity.
    const int flags =
        _O_WRONLY | _O_CREAT | _O_BINARY | (overwrite ? 0 : _O_EXCL);
    return ::_wopen(path_from_utf8(path).c_str(),
                    flags,
                    _S_IREAD | _S_IWRITE);
}

int close_fd(int fd) { return ::_close(fd); }

HANDLE native_handle(int fd) noexcept;

std::ptrdiff_t read_fd(int fd, void* buffer, std::size_t bytes)
{
    const HANDLE handle = native_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    const auto count = static_cast<DWORD>(
        std::min<std::size_t>(bytes, std::numeric_limits<DWORD>::max()));
    DWORD bytes_to_read = count;
    if (::GetFileType(handle) == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (::PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
                error == ERROR_PIPE_NOT_CONNECTED) {
                return 0;
            }
            errno = EIO;
            return -1;
        }
        if (available == 0) {
            ::Sleep(1);
            errno = EAGAIN;
            return -1;
        }
        bytes_to_read = std::min(count, available);
    }
    DWORD transferred = 0;
    if (::ReadFile(handle, buffer, bytes_to_read, &transferred, nullptr) != 0) {
        return static_cast<std::ptrdiff_t>(transferred);
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_MORE_DATA && transferred != 0) {
        return static_cast<std::ptrdiff_t>(transferred);
    }
    if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
        error == ERROR_NO_DATA) {
        return 0;
    }
    errno = error == ERROR_OPERATION_ABORTED ? EINTR : EIO;
    return -1;
}

std::ptrdiff_t write_fd(int fd, const void* buffer, std::size_t bytes)
{
    const HANDLE handle = native_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    const auto count = static_cast<DWORD>(
        std::min<std::size_t>(bytes, std::numeric_limits<DWORD>::max()));
    DWORD transferred = 0;
    if (::WriteFile(handle, buffer, count, &transferred, nullptr) != 0) {
        if (transferred == 0 && count != 0 &&
            ::GetFileType(handle) == FILE_TYPE_PIPE) {
            ::Sleep(1);
            errno = EAGAIN;
            return -1;
        }
        return static_cast<std::ptrdiff_t>(transferred);
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
        error == ERROR_PIPE_NOT_CONNECTED) {
        errno = EPIPE;
    } else if (error == ERROR_OPERATION_ABORTED) {
        errno = EINTR;
    } else {
        errno = EIO;
    }
    return -1;
}

bool rewind_fd(int fd) { return ::_lseeki64(fd, 0, SEEK_SET) >= 0; }

HANDLE native_handle(int fd) noexcept
{
    const intptr_t value = ::_get_osfhandle(fd);
    return value == -1 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(value);
}

bool same_open_file(int first_fd, int second_fd)
{
    const HANDLE first_handle = native_handle(first_fd);
    const HANDLE second_handle = native_handle(second_fd);
    if (first_handle == INVALID_HANDLE_VALUE || second_handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(EBADF,
                                std::generic_category(),
                                "get native INPUT/OUTPUT handles");
    }
    ::SetLastError(NO_ERROR);
    const DWORD first_type = ::GetFileType(first_handle);
    const DWORD first_type_error = ::GetLastError();
    ::SetLastError(NO_ERROR);
    const DWORD second_type = ::GetFileType(second_handle);
    const DWORD second_type_error = ::GetLastError();
    if ((first_type == FILE_TYPE_UNKNOWN && first_type_error != NO_ERROR) ||
        (second_type == FILE_TYPE_UNKNOWN && second_type_error != NO_ERROR)) {
        throw std::system_error(
            static_cast<int>(first_type == FILE_TYPE_UNKNOWN ? first_type_error
                                                             : second_type_error),
            std::system_category(),
            "identify opened INPUT/OUTPUT handles");
    }
    if (first_type != FILE_TYPE_DISK || second_type != FILE_TYPE_DISK) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION first {};
    BY_HANDLE_FILE_INFORMATION second {};
    if (::GetFileInformationByHandle(first_handle, &first) == 0 ||
        ::GetFileInformationByHandle(second_handle, &second) == 0) {
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(),
                                "identify opened INPUT/OUTPUT files");
    }
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
           first.nFileIndexHigh == second.nFileIndexHigh &&
           first.nFileIndexLow == second.nFileIndexLow;
}

int truncate_fd(int fd) noexcept
{
    const errno_t result = ::_chsize_s(fd, 0);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return ::_lseeki64(fd, 0, SEEK_SET) < 0 ? -1 : 0;
}

std::size_t preferred_write_size(int fd)
{
    struct _stat64 status {};
    if (::_fstat64(fd, &status) == 0 &&
        (status.st_mode & _S_IFMT) == _S_IFREG) {
        return 64 * 1024;
    }
    // Keep individual synchronous writes to redirected stdout small. They are
    // cancellable below, but a small chunk also limits pipe latency.
    return 4 * 1024;
}

bool stdin_refers_to_file(const std::string& path)
{
    const intptr_t stdin_handle_value = ::_get_osfhandle(::_fileno(stdin));
    if (stdin_handle_value == -1) return false;
    const HANDLE stdin_handle = reinterpret_cast<HANDLE>(stdin_handle_value);

    const fs::path native_path = path_from_utf8(path);
    const HANDLE output_handle = ::CreateFileW(native_path.c_str(),
                                                FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                    FILE_SHARE_DELETE,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr);
    if (output_handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION input_info {};
        BY_HANDLE_FILE_INFORMATION output_info {};
        const bool have_identity =
            ::GetFileInformationByHandle(stdin_handle, &input_info) != 0 &&
            ::GetFileInformationByHandle(output_handle, &output_info) != 0;
        ::CloseHandle(output_handle);
        if (have_identity) {
            return input_info.dwVolumeSerialNumber ==
                       output_info.dwVolumeSerialNumber &&
                   input_info.nFileIndexHigh == output_info.nFileIndexHigh &&
                   input_info.nFileIndexLow == output_info.nFileIndexLow;
        }
    }

    // Attribute-only CreateFile can still be denied by another handle's share
    // mode. In that case compare the final path already attached to stdin.
    const DWORD required = ::GetFinalPathNameByHandleW(
        stdin_handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (required == 0) return false;
    std::vector<wchar_t> input_path(static_cast<std::size_t>(required) + 1);
    const DWORD length = ::GetFinalPathNameByHandleW(stdin_handle,
                                                     input_path.data(),
                                                     static_cast<DWORD>(input_path.size()),
                                                     FILE_NAME_NORMALIZED);
    if (length == 0 || length >= input_path.size()) return false;
    std::error_code ec;
    return fs::equivalent(fs::path(input_path.data()), native_path, ec) && !ec;
}

std::string utf8_from_wide(const wchar_t* text)
{
    if (text == nullptr) return {};
    const int bytes = ::WideCharToMultiByte(CP_UTF8,
                                            WC_ERR_INVALID_CHARS,
                                            text,
                                            -1,
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr);
    if (bytes <= 0) {
        throw std::runtime_error("cannot convert a command-line argument to UTF-8");
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8,
                              WC_ERR_INVALID_CHARS,
                              text,
                              -1,
                              result.data(),
                              bytes,
                              nullptr,
                              nullptr) <= 0) {
        throw std::runtime_error("cannot convert a command-line argument to UTF-8");
    }
    result.pop_back();
    return result;
}

class cancellable_io
{
public:
    class operation
    {
    public:
        explicit operation(cancellable_io& owner) : owner_(owner) { owner_.begin(); }
        ~operation() { owner_.end(); }

        operation(const operation&) = delete;
        operation& operator=(const operation&) = delete;

    private:
        cancellable_io& owner_;
    };

    ~cancellable_io()
    {
        const HANDLE handle = thread_handle_.load(std::memory_order_acquire);
        if (handle != nullptr) ::CloseHandle(handle);
    }

    void begin()
    {
        if (thread_handle_.load(std::memory_order_acquire) == nullptr) {
            HANDLE duplicate = nullptr;
            if (::DuplicateHandle(::GetCurrentProcess(),
                                  ::GetCurrentThread(),
                                  ::GetCurrentProcess(),
                                  &duplicate,
                                  0,
                                  FALSE,
                                  DUPLICATE_SAME_ACCESS) == 0) {
                throw std::system_error(static_cast<int>(::GetLastError()),
                                        std::system_category(),
                                        "DuplicateHandle(I/O thread)");
            }
            HANDLE expected = nullptr;
            if (!thread_handle_.compare_exchange_strong(
                    expected, duplicate, std::memory_order_acq_rel)) {
                ::CloseHandle(duplicate);
            }
        }
        active_.store(true, std::memory_order_release);
    }

    void end() noexcept { active_.store(false, std::memory_order_release); }

    std::uint32_t cancel(int fd) noexcept
    {
        const HANDLE handle = thread_handle_.load(std::memory_order_acquire);
        const HANDLE file_handle = fd >= 0 ? native_handle(fd) : INVALID_HANDLE_VALUE;
        if (handle == nullptr || file_handle == INVALID_HANDLE_VALUE) {
            return active_.load(std::memory_order_acquire) ? ERROR_INVALID_HANDLE
                                                          : ERROR_SUCCESS;
        }
        while (active_.load(std::memory_order_acquire)) {
            if (::CancelSynchronousIo(handle) != 0) return ERROR_SUCCESS;
            const DWORD thread_error = ::GetLastError();

            // CancelIoEx targets the actual descriptor handle and is a
            // fallback if thread-targeted cancellation is unavailable.
            if (::CancelIoEx(file_handle, nullptr) != 0) return ERROR_SUCCESS;
            const DWORD file_error = ::GetLastError();

            if (!active_.load(std::memory_order_acquire)) return ERROR_SUCCESS;
            // ERROR_NOT_FOUND from both APIs is the narrow transition where
            // begin() is visible but the CRT has not entered ReadFile/WriteFile
            // yet. Yield until either the worker observes stop or I/O exists to
            // cancel. Any persistent API error is returned instead of spun on.
            if (thread_error != ERROR_NOT_FOUND || file_error != ERROR_NOT_FOUND) {
                return thread_error != ERROR_NOT_FOUND ? thread_error : file_error;
            }
            ::Sleep(1);
        }
        return ERROR_SUCCESS;
    }

private:
    std::atomic<HANDLE> thread_handle_{ nullptr };
    std::atomic<bool> active_{ false };
};

#else

int duplicate_stdin() { return ::dup(STDIN_FILENO); }
int duplicate_stdout() { return ::dup(STDOUT_FILENO); }
int open_input(const std::string& path) { return ::open(path.c_str(), O_RDONLY); }

int open_output(const std::string& path, bool overwrite)
{
    // In overwrite mode truncation is deliberately deferred until the opened
    // INPUT and OUTPUT descriptors have been compared for identity.
    const int flags = O_WRONLY | O_CREAT | (overwrite ? 0 : O_EXCL);
    return ::open(path.c_str(), flags, 0666);
}

int close_fd(int fd) { return ::close(fd); }

std::ptrdiff_t read_fd(int fd, void* buffer, std::size_t bytes)
{
    return static_cast<std::ptrdiff_t>(::read(fd, buffer, bytes));
}

std::ptrdiff_t write_fd(int fd, const void* buffer, std::size_t bytes)
{
    return static_cast<std::ptrdiff_t>(::write(fd, buffer, bytes));
}

bool rewind_fd(int fd) { return ::lseek(fd, 0, SEEK_SET) >= 0; }

bool same_open_file(int first_fd, int second_fd)
{
    struct stat first {};
    struct stat second {};
    if (::fstat(first_fd, &first) != 0 || ::fstat(second_fd, &second) != 0) {
        throw std::system_error(errno,
                                std::generic_category(),
                                "identify opened INPUT/OUTPUT files");
    }
    return S_ISREG(first.st_mode) && S_ISREG(second.st_mode) &&
           first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

int truncate_fd(int fd) noexcept
{
    if (::ftruncate(fd, 0) != 0) return -1;
    return ::lseek(fd, 0, SEEK_SET) < 0 ? -1 : 0;
}

std::size_t preferred_write_size(int fd)
{
    struct stat status {};
    if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)) return 64 * 1024;
    const long pipe_buf = ::fpathconf(fd, _PC_PIPE_BUF);
    return pipe_buf > 0 ? static_cast<std::size_t>(pipe_buf) : 512;
}

bool stdin_refers_to_file(const std::string& path)
{
    struct stat input_status {};
    struct stat output_status {};
    return ::fstat(STDIN_FILENO, &input_status) == 0 &&
           ::stat(path.c_str(), &output_status) == 0 &&
           S_ISREG(input_status.st_mode) &&
           input_status.st_dev == output_status.st_dev &&
           input_status.st_ino == output_status.st_ino;
}

class cancellable_io
{
public:
    class operation
    {
    public:
        explicit operation(cancellable_io& owner) : owner_(owner) { owner_.begin(); }
        ~operation() { owner_.end(); }

        operation(const operation&) = delete;
        operation& operator=(const operation&) = delete;

    private:
        cancellable_io& owner_;
    };

    void begin() {}
    void end() noexcept {}
    std::uint32_t cancel(int) noexcept { return 0; }
};

#endif

} // namespace platform

static_assert(sizeof(gr_complex) == 2 * sizeof(float),
              "dvbt2mod requires packed complex float samples");
static_assert(std::numeric_limits<float>::is_iec559,
              "dvbt2mod requires IEEE-754 float32");

struct Config {
    int profile = 1;
    int bandwidth_khz = 8000;
    int fft = 32768;
    bool t2gi = true;
    int qam = 256;
    int rate_num = 3;
    int rate_den = 5;
    int guard_num = 1;
    int guard_den = 128;
    int pilot = 7;
    int plp_fec_blocks = 202;
    int bb_fec_blocks = 168;
    int ti_blocks = 3;
    int data_symbols = 59;
    int ts_rate = 4000000;
    int frame_size = 64800;
    int l1_qam = 64;
    int t2_frames = 2;
    bool rotation = true;
    bool extended = true;
    bool high_efficiency = true;
    bool papr_tr = false;
    float papr_vclip = 3.0F;
    int papr_iterations = 10;
    float p1_vclip = 3.3F;
};

struct Options {
    std::string input;
    std::string output;
    Config config;
    bool repeat = false;
    bool overwrite = false;
    bool check_only = false;
    std::uint64_t samples = 0;
};

class async_error_state
{
public:
    void capture_current_exception()
    {
        std::exception_ptr current = std::current_exception();
        if (!current) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_) return;
        error_ = std::move(current);
        failed_.store(true, std::memory_order_release);
    }

    bool failed() const { return failed_.load(std::memory_order_acquire); }

    void rethrow_if_failed() const
    {
        if (!failed()) return;
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = error_;
        }
        if (error) std::rethrow_exception(error);
    }

private:
    std::atomic<bool> failed_{ false };
    mutable std::mutex mutex_;
    std::exception_ptr error_;
};

class ts_packet_source final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<ts_packet_source>;

    static sptr make(const std::string& path,
                     bool repeat,
                     bool pad_tail,
                     std::uint64_t payload_bytes_per_t2_frame,
                     bool high_efficiency,
                     std::shared_ptr<async_error_state> error_state)
    {
        return std::make_shared<ts_packet_source>(path,
                                                  repeat,
                                                  pad_tail,
                                                  payload_bytes_per_t2_frame,
                                                  high_efficiency,
                                                  std::move(error_state));
    }

    ts_packet_source(const std::string& path,
                     bool repeat,
                     bool pad_tail,
                     std::uint64_t payload_bytes_per_t2_frame,
                     bool high_efficiency,
                     std::shared_ptr<async_error_state> error_state)
        : gr::sync_block("dvbt2mod_ts_source",
                         gr::io_signature::make(0, 0, 0),
                         gr::io_signature::make(1, 1, sizeof(std::uint8_t))),
          repeat_(repeat),
          pad_tail_(pad_tail),
          payload_bytes_per_t2_frame_(payload_bytes_per_t2_frame),
          high_efficiency_(high_efficiency),
          poll_input_(path == "-"),
          error_state_(std::move(error_state))
    {
        fd_ = poll_input_ ? platform::duplicate_stdin() : platform::open_input(path);
        if (fd_ < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    poll_input_ ? "dup(stdin)" : "open(input)");
        }
        null_packet_.fill(0xff);
        null_packet_[0] = 0x47;
        null_packet_[1] = 0x1f;
        null_packet_[2] = 0xff;
        null_packet_[3] = 0x10;
    }

    ~ts_packet_source() override
    {
        if (fd_ >= 0) platform::close_fd(fd_);
    }

    std::uint64_t input_packets() const { return input_packets_; }
    std::uint64_t padding_bytes() const { return padding_bytes_; }
    int file_descriptor() const { return fd_; }

    bool stop() override
    {
        stop_requested_.store(true, std::memory_order_release);
        const std::uint32_t cancel_error = cancellable_io_.cancel(fd_);
        if (cancel_error != 0) {
            try {
                throw std::system_error(static_cast<int>(cancel_error),
                                        std::system_category(),
                                        "cancel(input I/O)");
            } catch (...) {
                error_state_->capture_current_exception();
            }
        }
        return true;
    }

    int work(int noutput_items,
             gr_vector_const_void_star&,
             gr_vector_void_star& output_items) override
    {
        try {
            return work_impl(noutput_items, output_items);
        } catch (...) {
            error_state_->capture_current_exception();
            return WORK_DONE;
        }
    }

private:
    enum class load_result { ready, wait, done };

    int work_impl(int noutput_items, gr_vector_void_star& output_items)
    {
        auto* output = static_cast<std::uint8_t*>(output_items[0]);
        int produced = 0;

        while (produced < noutput_items) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                return produced == 0 ? WORK_DONE : produced;
            }

            if (packet_offset_ < packet_.size()) {
                const std::size_t count = std::min<std::size_t>(
                    packet_.size() - packet_offset_,
                    static_cast<std::size_t>(noutput_items - produced));
                std::memcpy(output + produced, packet_.data() + packet_offset_, count);
                packet_offset_ += count;
                produced += static_cast<int>(count);
                continue;
            }

            if (padding_remaining_ != 0) {
                const std::size_t available =
                    null_packet_.size() - padding_packet_offset_;
                const std::size_t count = std::min<std::size_t>(
                    std::min<std::uint64_t>(padding_remaining_, available),
                    static_cast<std::size_t>(noutput_items - produced));
                std::memcpy(output + produced,
                            null_packet_.data() + padding_packet_offset_,
                            count);
                produced += static_cast<int>(count);
                padding_remaining_ -= count;
                padding_packet_offset_ =
                    (padding_packet_offset_ + count) % null_packet_.size();
                continue;
            }

            if (finished_) return produced == 0 ? WORK_DONE : produced;

            const load_result result = load_packet();
            if (result == load_result::wait) return produced;
            if (result == load_result::done) {
                return produced == 0 ? WORK_DONE : produced;
            }
        }
        return produced;
    }

    load_result load_packet()
    {
        while (packet_fill_ < packet_.size()) {
#if !defined(_WIN32)
            if (poll_input_) {
                pollfd descriptor{ fd_, POLLIN, 0 };
                const int poll_result = ::poll(&descriptor, 1, 50);
                if (poll_result == 0) return load_result::wait;
                if (poll_result < 0) {
                    if (errno == EINTR) return load_result::wait;
                    throw std::system_error(errno,
                                            std::generic_category(),
                                            "poll(input)");
                }
                if ((descriptor.revents & POLLNVAL) != 0) {
                    throw std::runtime_error("input file descriptor became invalid");
                }
            }
#endif

            std::ptrdiff_t bytes = 0;
            {
                platform::cancellable_io::operation operation(cancellable_io_);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return load_result::done;
                }
                bytes = platform::read_fd(fd_,
                                          packet_.data() + packet_fill_,
                                          packet_.size() - packet_fill_);
            }
            if (bytes > 0) {
                packet_fill_ += static_cast<std::size_t>(bytes);
                continue;
            }
            if (bytes < 0) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return load_result::done;
                }
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    return load_result::wait;
                }
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "read(input)");
            }

            if (packet_fill_ != 0) {
                throw std::runtime_error(
                    "input ended with an incomplete 188-byte MPEG-TS packet");
            }
            if (repeat_) {
                if (!platform::rewind_fd(fd_)) {
                    throw std::system_error(errno,
                                            std::generic_category(),
                                            "rewind(input)");
                }
                continue;
            }
            plan_padding();
            return padding_remaining_ == 0 ? load_result::done : load_result::ready;
        }

        if (packet_[0] != 0x47) {
            throw std::runtime_error("MPEG-TS sync byte missing at packet " +
                                     std::to_string(input_packets_));
        }
        ++input_packets_;
        packet_fill_ = 0;
        packet_offset_ = 0;
        return load_result::ready;
    }

    void plan_padding()
    {
        finished_ = true;
        if (!pad_tail_ || input_packets_ == 0) return;

        if (input_packets_ >
            std::numeric_limits<std::uint64_t>::max() / packet_.size()) {
            throw std::runtime_error("input byte count overflow");
        }
        const std::uint64_t input_bytes = input_packets_ * packet_.size();
        const std::uint64_t effective_payload_bytes =
            high_efficiency_ ? input_packets_ * 187 : input_bytes;
        const std::uint64_t frames =
            effective_payload_bytes / payload_bytes_per_t2_frame_ +
            (effective_payload_bytes % payload_bytes_per_t2_frame_ != 0 ? 1 : 0);
        if (frames > std::numeric_limits<std::uint64_t>::max() /
                         payload_bytes_per_t2_frame_) {
            throw std::runtime_error("padded input size overflow");
        }
        const std::uint64_t target_payload =
            frames * payload_bytes_per_t2_frame_;
        const std::uint64_t removed_sync_bytes =
            high_efficiency_
                ? target_payload / 187 + (target_payload % 187 != 0 ? 1 : 0)
                : 0;
        if (target_payload > std::numeric_limits<std::uint64_t>::max() -
                                 removed_sync_bytes) {
            throw std::runtime_error("padded input size overflow");
        }
        const std::uint64_t target_bytes = target_payload + removed_sync_bytes;
        if (target_bytes < input_bytes) {
            throw std::runtime_error("internal error: padded input target underflow");
        }
        padding_bytes_ = target_bytes - input_bytes;
        padding_remaining_ = padding_bytes_;
    }

    int fd_ = -1;
    bool repeat_ = false;
    bool pad_tail_ = false;
    std::uint64_t payload_bytes_per_t2_frame_ = 0;
    bool high_efficiency_ = false;
    bool poll_input_ = false;
    platform::cancellable_io cancellable_io_;
    std::shared_ptr<async_error_state> error_state_;
    std::atomic<bool> stop_requested_{ false };
    std::array<std::uint8_t, 188> packet_{};
    std::size_t packet_fill_ = 0;
    std::size_t packet_offset_ = packet_.size();
    std::array<std::uint8_t, 188> null_packet_{};
    std::size_t padding_packet_offset_ = 0;
    std::uint64_t input_packets_ = 0;
    std::uint64_t padding_bytes_ = 0;
    std::uint64_t padding_remaining_ = 0;
    bool finished_ = false;
};

class interruptible_output_sink final : public gr::sync_block
{
public:
    using sptr = std::shared_ptr<interruptible_output_sink>;

    static sptr make(const std::string& path,
                     bool overwrite,
                     int input_fd,
                     std::shared_ptr<async_error_state> error_state)
    {
        return std::make_shared<interruptible_output_sink>(
            path, overwrite, input_fd, std::move(error_state));
    }

    interruptible_output_sink(
        const std::string& path,
        bool overwrite,
        int input_fd,
        std::shared_ptr<async_error_state> error_state)
        : gr::sync_block("dvbt2mod_output_sink",
                         gr::io_signature::make(1, 1, sizeof(gr_complex)),
                         gr::io_signature::make(0, 0, 0)),
          is_stdout_(path == "-"),
          error_state_(std::move(error_state))
    {
        if (is_stdout_) {
            fd_ = platform::duplicate_stdout();
        } else {
            fd_ = platform::open_output(path, overwrite);
        }
        if (fd_ < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    is_stdout_ ? "dup(stdout)" : "open(output)");
        }

        try {
            if (input_fd >= 0 && platform::same_open_file(input_fd, fd_)) {
                throw std::runtime_error(
                    "opened INPUT and OUTPUT refer to the same file");
            }
            if (!is_stdout_ && overwrite && platform::truncate_fd(fd_) != 0) {
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "truncate(output)");
            }
        } catch (...) {
            platform::close_fd(fd_);
            fd_ = -1;
            throw;
        }

        max_write_bytes_ = platform::preferred_write_size(fd_);
        max_write_bytes_ -= max_write_bytes_ % sizeof(gr_complex);
        if (max_write_bytes_ == 0) max_write_bytes_ = sizeof(gr_complex);
    }

    ~interruptible_output_sink() override
    {
        if (fd_ >= 0) platform::close_fd(fd_);
    }

    void close()
    {
        if (fd_ < 0) return;
        const int descriptor = fd_;
        fd_ = -1;
        if (platform::close_fd(descriptor) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    is_stdout_ ? "close(stdout)" : "close(output)");
        }
    }

    bool stop() override
    {
        stop_requested_.store(true, std::memory_order_release);
        const std::uint32_t cancel_error = cancellable_io_.cancel(fd_);
        if (cancel_error != 0) {
            try {
                throw std::system_error(static_cast<int>(cancel_error),
                                        std::system_category(),
                                        "cancel(output I/O)");
            } catch (...) {
                error_state_->capture_current_exception();
            }
        }
        return true;
    }

    std::uint64_t bytes_written() const
    {
        return bytes_written_.load(std::memory_order_relaxed);
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star&) override
    {
        try {
            return work_impl(noutput_items, input_items);
        } catch (...) {
            error_state_->capture_current_exception();
            return WORK_DONE;
        }
    }

private:
    int work_impl(int noutput_items, gr_vector_const_void_star& input_items)
    {
        if (stop_requested_.load(std::memory_order_acquire)) return WORK_DONE;
        if (noutput_items <= 0) return 0;

        const auto* input = static_cast<const std::uint8_t*>(input_items[0]);
#if defined(_WIN32)
        const std::size_t item_size = sizeof(gr_complex);
        const std::size_t available_bytes =
            static_cast<std::size_t>(noutput_items) * item_size;
        if (partial_item_bytes_ >= item_size ||
            partial_item_bytes_ >= available_bytes) {
            throw std::runtime_error("invalid partial output item state");
        }
        const std::size_t bytes_to_write = std::min<std::size_t>(
            available_bytes - partial_item_bytes_, max_write_bytes_);

        std::ptrdiff_t bytes = 0;
        {
            platform::cancellable_io::operation operation(cancellable_io_);
            if (stop_requested_.load(std::memory_order_acquire)) return WORK_DONE;
            bytes = platform::write_fd(
                fd_, input + partial_item_bytes_, bytes_to_write);
        }
        if (bytes > 0) {
            const std::size_t total =
                partial_item_bytes_ + static_cast<std::size_t>(bytes);
            const int consumed = static_cast<int>(total / item_size);
            partial_item_bytes_ = total % item_size;
            bytes_written_.fetch_add(static_cast<std::uint64_t>(bytes),
                                     std::memory_order_relaxed);
            return consumed;
        }
        if (bytes < 0 && stop_requested_.load(std::memory_order_acquire)) {
            return WORK_DONE;
        }
        if (bytes < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (bytes < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "write(output)");
        }
        throw std::runtime_error("write(output) made no progress");
#else
        const std::size_t bytes_to_write = std::min<std::size_t>(
            static_cast<std::size_t>(noutput_items) * sizeof(gr_complex),
            max_write_bytes_);
        std::size_t written = 0;

        while (written < bytes_to_write) {
            if (stop_requested_.load(std::memory_order_acquire)) return WORK_DONE;

#if !defined(_WIN32)
            pollfd descriptor{ fd_, POLLOUT, 0 };
            const int poll_result = ::poll(&descriptor, 1, 50);
            if (poll_result == 0) {
                if (written == 0) return 0;
                continue;
            }
            if (poll_result < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "poll(output)");
            }
            if ((descriptor.revents & POLLNVAL) != 0) {
                throw std::runtime_error("output file descriptor became invalid");
            }
#endif

            std::ptrdiff_t bytes = 0;
            {
                platform::cancellable_io::operation operation(cancellable_io_);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return WORK_DONE;
                }
                bytes = platform::write_fd(
                    fd_, input + written, bytes_to_write - written);
            }
            if (bytes > 0) {
                written += static_cast<std::size_t>(bytes);
                bytes_written_.fetch_add(static_cast<std::uint64_t>(bytes),
                                         std::memory_order_relaxed);
                continue;
            }
            if (bytes < 0 && stop_requested_.load(std::memory_order_acquire)) {
                return WORK_DONE;
            }
            if (bytes < 0 && (errno == EINTR || errno == EAGAIN ||
                              errno == EWOULDBLOCK)) {
                continue;
            }
            if (bytes < 0) {
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "write(output)");
            }
            throw std::runtime_error("write(output) made no progress");
        }
        return static_cast<int>(written / sizeof(gr_complex));
#endif
    }

    int fd_ = -1;
    bool is_stdout_ = false;
    std::size_t max_write_bytes_ = 512;
    platform::cancellable_io cancellable_io_;
    std::shared_ptr<async_error_state> error_state_;
    std::atomic<bool> stop_requested_{ false };
    std::atomic<std::uint64_t> bytes_written_{ 0 };
#if defined(_WIN32)
    std::size_t partial_item_bytes_ = 0;
#endif
};

struct Graph {
    gr::top_block_sptr top;
    std::shared_ptr<async_error_state> error_state;
    ts_packet_source::sptr source;
    interruptible_output_sink::sptr output_sink;
};

#if defined(_WIN32)
std::atomic<int> caught_signal{ 0 };
static_assert(std::atomic<int>::is_always_lock_free,
              "Windows console control handling requires a lock-free atomic int");

void store_caught_signal(int signal_number) noexcept
{
    caught_signal.store(signal_number, std::memory_order_relaxed);
}

int load_caught_signal() noexcept
{
    return caught_signal.load(std::memory_order_relaxed);
}
#else
volatile std::sig_atomic_t caught_signal = 0;

void store_caught_signal(int signal_number) noexcept
{
    caught_signal = signal_number;
}

int load_caught_signal() noexcept { return caught_signal; }
#endif

extern "C" void handle_signal(int signal_number)
{
    store_caught_signal(signal_number);
}

#if defined(_WIN32)
extern "C" BOOL WINAPI handle_console_control(DWORD control_type)
{
    switch (control_type) {
    case CTRL_C_EVENT:
        store_caught_signal(SIGINT);
        return TRUE;
    case CTRL_BREAK_EVENT:
        store_caught_signal(SIGBREAK);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

class scoped_signal_handlers
{
public:
    scoped_signal_handlers()
    {
        store_caught_signal(0);
        old_sigint_ = std::signal(SIGINT, handle_signal);
        old_sigterm_ = std::signal(SIGTERM, handle_signal);
#if defined(_WIN32)
        old_sigbreak_ = std::signal(SIGBREAK, handle_signal);
        if (::SetConsoleCtrlHandler(handle_console_control, TRUE) == 0) {
            const DWORD error = ::GetLastError();
            restore_standard_handlers();
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "install Windows console control handler");
        }
        console_handler_installed_ = true;
#else
        old_sigpipe_ = std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    ~scoped_signal_handlers()
    {
#if defined(_WIN32)
        if (console_handler_installed_) {
            ::SetConsoleCtrlHandler(handle_console_control, FALSE);
        }
#endif
        restore_standard_handlers();
    }

    scoped_signal_handlers(const scoped_signal_handlers&) = delete;
    scoped_signal_handlers& operator=(const scoped_signal_handlers&) = delete;

private:
    using signal_handler = void (*)(int);

    void restore_standard_handlers() noexcept
    {
        std::signal(SIGINT, old_sigint_);
        std::signal(SIGTERM, old_sigterm_);
#if defined(_WIN32)
        std::signal(SIGBREAK, old_sigbreak_);
#else
        std::signal(SIGPIPE, old_sigpipe_);
#endif
    }

    signal_handler old_sigint_ = SIG_DFL;
    signal_handler old_sigterm_ = SIG_DFL;
#if defined(_WIN32)
    signal_handler old_sigbreak_ = SIG_DFL;
    bool console_handler_installed_ = false;
#else
    signal_handler old_sigpipe_ = SIG_DFL;
#endif
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void print_help(std::ostream& out)
{
    out << R"(dvbt2mod 0.1.0 - MPEG-TS to DVB-T2 CF32 modulator

Usage:
  dvbt2mod INPUT.ts OUTPUT.cf32 [OPTION NUMBER]...
  dvbt2mod - - [OPTION NUMBER]...       (stdin -> stdout)

All modulation option values are numeric. Paths are the only string values.

Profiles:
  --profile N          0: GNU Radio QA golden, 1: vv001, 9: vv009 (default 1)

Physical modulation values:
  --bandwidth-khz N    1700, 5000, 6000, 7000, 8000, 10000
  --fft N              1024, 2048, 4096, 8192, 16384, 32768
  --t2gi N             use special T2-GI FFT signalling (0 or 1)
  --qam N              4, 16, 64, 256
  --rate-num N         code-rate numerator
  --rate-den N         code-rate denominator
  --guard-num N        guard-interval numerator
  --guard-den N        guard-interval denominator
  --pilot N            pilot pattern 1..8
  --fec-blocks N       PLP FEC blocks per T2 frame
  --ti-blocks N        time-interleaver blocks per T2 frame
  --data-symbols N     data OFDM symbols per T2 frame
  --frame-size N       16200 or 64800 bits
  --l1-qam N           1 (BPSK), 4, 16, or 64
  --t2-frames N        T2 frames per super-frame (2..255)
  --rotation N         constellation rotation (0 or 1)
  --extended N         extended carrier mode (0 or 1)
  --high-efficiency N  BBHEADER high-efficiency mode (0 or 1)
  --papr-tr N          tone-reservation PAPR block (0 or 1)

Runtime values:
  --repeat N           repeat seekable input (0 or 1)
  --samples N          stop after N complex output samples; 0 = unlimited
  --overwrite N        allow replacing OUTPUT (0 or 1; default 0)
  --check N            validate and construct the graph without running (0 or 1)

Other:
  --help               show this help
  --version            show version

Output is headerless native little-endian CF32 (float32 I, float32 Q).
Finite TS input is null-padded to a complete T2 frame; --samples is a raw IQ cap.
The supported MVP is DVB-T2 v1.1.1, SISO, single PLP, PAPR off or TR.
)";
}

template <typename T>
T parse_integer(std::string_view text, std::string_view option)
{
    T value{};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (text.empty() || result.ec != std::errc{} || result.ptr != end) {
        fail(std::string(option) + " expects a base-10 integer, got '" +
             std::string(text) + "'");
    }
    return value;
}

bool parse_bool(std::string_view text, std::string_view option)
{
    const int value = parse_integer<int>(text, option);
    if (value != 0 && value != 1) {
        fail(std::string(option) + " expects 0 or 1");
    }
    return value == 1;
}

Config profile_config(int profile)
{
    Config c;
    c.profile = profile;
    switch (profile) {
    case 0: // Exact gr-dtv/python/dtv/qa_dtv.py golden configuration.
        c.bandwidth_khz = 8000;
        c.fft = 4096;
        c.t2gi = false;
        c.qam = 64;
        c.rate_num = 2;
        c.rate_den = 3;
        c.guard_num = 1;
        c.guard_den = 32;
        c.pilot = 7;
        c.plp_fec_blocks = 3;
        c.bb_fec_blocks = 168;
        c.ti_blocks = 3;
        c.data_symbols = 8;
        c.ts_rate = 4000000;
        c.frame_size = 64800;
        c.l1_qam = 16;
        c.t2_frames = 2;
        c.rotation = true;
        c.extended = false;
        c.high_efficiency = false;
        c.papr_tr = true;
        c.papr_vclip = 3.0F;
        c.papr_iterations = 10;
        c.p1_vclip = 3.01F;
        return c;
    case 1: // Exact gr-dtv/examples/vv001-cr35.grc modulation settings.
        return c;
    case 9: // Exact gr-dtv/examples/vv009-4kfft.grc modulation settings.
        c.bandwidth_khz = 8000;
        c.fft = 4096;
        c.t2gi = false;
        c.qam = 64;
        c.rate_num = 2;
        c.rate_den = 3;
        c.guard_num = 1;
        c.guard_den = 32;
        c.pilot = 7;
        c.plp_fec_blocks = 31;
        c.bb_fec_blocks = 168;
        c.ti_blocks = 3;
        c.data_symbols = 100;
        c.ts_rate = 4000000;
        c.frame_size = 64800;
        c.l1_qam = 16;
        c.t2_frames = 2;
        c.rotation = true;
        c.extended = false;
        c.high_efficiency = false;
        c.papr_tr = false;
        c.p1_vclip = 3.3F;
        return c;
    default:
        fail("--profile expects 0, 1, or 9");
    }
}

std::string require_value(int argc, char** argv, int& index)
{
    if (index + 1 >= argc) {
        fail(std::string(argv[index]) + " requires a numeric value");
    }
    return argv[++index];
}

Options parse_options(int argc, char** argv)
{
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        if (argument == "--help") {
            print_help(std::cout);
            std::exit(0);
        }
        if (argument == "--version") {
            std::cout << "dvbt2mod 0.1.0\n";
            std::exit(0);
        }
    }

    if (argc < 3) {
        print_help(std::cerr);
        fail("INPUT and OUTPUT are required");
    }

    Options options;
    options.input = argv[1];
    options.output = argv[2];
    if (options.input.rfind("--", 0) == 0 || options.output.rfind("--", 0) == 0) {
        fail("INPUT and OUTPUT must be the first two arguments");
    }

    int selected_profile = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string_view option(argv[i]);
        const std::string value = require_value(argc, argv, i);
        if (option == "--profile") {
            selected_profile = parse_integer<int>(value, option);
        }
    }
    options.config = profile_config(selected_profile);

    for (int i = 3; i < argc; ++i) {
        const std::string option(argv[i]);
        const std::string value = require_value(argc, argv, i);
        Config& c = options.config;
        if (option == "--profile") {
            continue;
        } else if (option == "--bandwidth-khz") {
            c.bandwidth_khz = parse_integer<int>(value, option);
        } else if (option == "--fft") {
            c.fft = parse_integer<int>(value, option);
        } else if (option == "--t2gi") {
            c.t2gi = parse_bool(value, option);
        } else if (option == "--qam") {
            c.qam = parse_integer<int>(value, option);
        } else if (option == "--rate-num") {
            c.rate_num = parse_integer<int>(value, option);
        } else if (option == "--rate-den") {
            c.rate_den = parse_integer<int>(value, option);
        } else if (option == "--guard-num") {
            c.guard_num = parse_integer<int>(value, option);
        } else if (option == "--guard-den") {
            c.guard_den = parse_integer<int>(value, option);
        } else if (option == "--pilot") {
            c.pilot = parse_integer<int>(value, option);
        } else if (option == "--fec-blocks") {
            c.plp_fec_blocks = parse_integer<int>(value, option);
        } else if (option == "--ti-blocks") {
            c.ti_blocks = parse_integer<int>(value, option);
        } else if (option == "--data-symbols") {
            c.data_symbols = parse_integer<int>(value, option);
        } else if (option == "--frame-size") {
            c.frame_size = parse_integer<int>(value, option);
        } else if (option == "--l1-qam") {
            c.l1_qam = parse_integer<int>(value, option);
        } else if (option == "--t2-frames") {
            c.t2_frames = parse_integer<int>(value, option);
        } else if (option == "--rotation") {
            c.rotation = parse_bool(value, option);
        } else if (option == "--extended") {
            c.extended = parse_bool(value, option);
        } else if (option == "--high-efficiency") {
            c.high_efficiency = parse_bool(value, option);
        } else if (option == "--papr-tr") {
            c.papr_tr = parse_bool(value, option);
        } else if (option == "--repeat") {
            options.repeat = parse_bool(value, option);
        } else if (option == "--samples") {
            options.samples = parse_integer<std::uint64_t>(value, option);
        } else if (option == "--overwrite") {
            options.overwrite = parse_bool(value, option);
        } else if (option == "--check") {
            options.check_only = parse_bool(value, option);
        } else {
            fail("unknown option '" + option + "'");
        }
    }
    return options;
}

bool one_of(int value, std::initializer_list<int> allowed)
{
    for (const int candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

bool ratio_is(int numerator, int denominator, int a, int b)
{
    return numerator == a && denominator == b;
}

bool pilot_allowed_for_fft_and_guard(const Config& c)
{
    // ETSI EN 302 755, table 52: allowed SISO FFT/GI/PP combinations.
    const auto allowed = [&c](std::initializer_list<int> pilots) {
        return one_of(c.pilot, pilots);
    };
    if (ratio_is(c.guard_num, c.guard_den, 1, 128)) {
        if (c.fft == 8192 || c.fft == 16384 || c.fft == 32768) return c.pilot == 7;
        return false;
    }
    if (ratio_is(c.guard_num, c.guard_den, 1, 32)) {
        if (c.fft == 32768) return allowed({ 4, 6 });
        if (c.fft == 16384) return allowed({ 4, 6, 7 });
        if (c.fft == 8192 || c.fft == 4096 || c.fft == 2048)
            return allowed({ 4, 7 });
        return false;
    }
    if (ratio_is(c.guard_num, c.guard_den, 1, 16)) {
        if (c.fft == 32768) return allowed({ 2, 4, 8 });
        if (c.fft == 16384) return allowed({ 2, 4, 5, 8 });
        if (c.fft == 8192) return allowed({ 4, 5, 8 });
        return allowed({ 4, 5 });
    }
    if (ratio_is(c.guard_num, c.guard_den, 19, 256)) {
        if (c.fft == 32768) return allowed({ 2, 4, 8 });
        if (c.fft == 16384) return allowed({ 2, 4, 5, 8 });
        if (c.fft == 8192) return allowed({ 4, 5, 8 });
        return false;
    }
    if (ratio_is(c.guard_num, c.guard_den, 1, 8)) {
        if (c.fft == 32768) return allowed({ 2, 8 });
        if (c.fft == 8192 || c.fft == 16384) return allowed({ 2, 3, 8 });
        return allowed({ 2, 3 });
    }
    if (ratio_is(c.guard_num, c.guard_den, 19, 128)) {
        if (c.fft == 32768) return allowed({ 2, 8 });
        if (c.fft == 8192 || c.fft == 16384) return allowed({ 2, 3, 8 });
        return false;
    }
    if (ratio_is(c.guard_num, c.guard_den, 1, 4)) {
        if (c.fft == 8192 || c.fft == 16384) return allowed({ 1, 8 });
        if (c.fft == 1024 || c.fft == 2048 || c.fft == 4096) return c.pilot == 1;
        return false;
    }
    return false;
}

struct CarrierCells {
    int data;
    int frame_closing;
    int frame_closing_data;
};

CarrierCells carrier_cells_for(const Config& c)
{
    // ETSI carrier-cell counts mirrored by GNU Radio 3.10's frame mapper.
    // Rows are PP1..PP8. A zero data count denotes an unsupported combination.
    static constexpr std::array<CarrierCells, 8> cells_1k = { {
        { 764, 568, 402 }, { 768, 710, 654 }, { 798, 710, 490 },
        { 804, 780, 707 }, { 818, 780, 544 }, { 0, 0, 0 },
        { 0, 0, 0 },      { 0, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_2k = { {
        { 1522, 1136, 804 }, { 1532, 1420, 1309 }, { 1596, 1420, 980 },
        { 1602, 1562, 1415 }, { 1632, 1562, 1088 }, { 0, 0, 0 },
        { 1646, 1632, 1396 }, { 0, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_4k = { {
        { 3084, 2272, 1609 }, { 3092, 2840, 2619 }, { 3228, 2840, 1961 },
        { 3234, 3124, 2831 }, { 3298, 3124, 2177 }, { 0, 0, 0 },
        { 3328, 3266, 2792 }, { 0, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_8k_normal = { {
        { 6208, 4544, 3218 }, { 6214, 5680, 5238 }, { 6494, 5680, 3922 },
        { 6498, 6248, 5662 }, { 6634, 6248, 4354 }, { 0, 0, 0 },
        { 6698, 6532, 5585 }, { 6698, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_8k_extended = { {
        { 6296, 4608, 3264 }, { 6298, 5760, 5312 }, { 6584, 5760, 3978 },
        { 6588, 6336, 5742 }, { 6728, 6336, 4416 }, { 0, 0, 0 },
        { 6788, 6624, 5664 }, { 6788, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_16k_normal = { {
        { 12418, 9088, 6437 }, { 12436, 11360, 10476 },
        { 12988, 11360, 7845 }, { 13002, 12496, 11324 },
        { 13272, 12496, 8709 }, { 13288, 13064, 11801 },
        { 13416, 13064, 11170 }, { 13406, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_16k_extended = { {
        { 12678, 9280, 6573 }, { 12698, 11600, 10697 },
        { 13262, 11600, 8011 }, { 13276, 12760, 11563 },
        { 13552, 12760, 8893 }, { 13568, 13340, 12051 },
        { 13698, 13340, 11406 }, { 13688, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_32k_normal = { {
        { 0, 0, 0 }, { 24886, 22720, 20952 }, { 0, 0, 0 },
        { 26022, 24992, 22649 }, { 0, 0, 0 }, { 26592, 26128, 23603 },
        { 26836, 0, 0 }, { 26812, 0, 0 },
    } };
    static constexpr std::array<CarrierCells, 8> cells_32k_extended = { {
        { 0, 0, 0 }, { 25412, 23200, 21395 }, { 0, 0, 0 },
        { 26572, 25520, 23127 }, { 0, 0, 0 }, { 27152, 26680, 24102 },
        { 27404, 0, 0 }, { 27376, 0, 0 },
    } };

    const std::size_t pp = static_cast<std::size_t>(c.pilot - 1);
    CarrierCells cells{};
    int tr_cells = 0;
    switch (c.fft) {
    case 1024:
        cells = cells_1k[pp];
        tr_cells = 10;
        break;
    case 2048:
        cells = cells_2k[pp];
        tr_cells = 18;
        break;
    case 4096:
        cells = cells_4k[pp];
        tr_cells = 36;
        break;
    case 8192:
        cells = (c.extended ? cells_8k_extended : cells_8k_normal)[pp];
        tr_cells = 72;
        break;
    case 16384:
        cells = (c.extended ? cells_16k_extended : cells_16k_normal)[pp];
        tr_cells = 144;
        break;
    case 32768:
        cells = (c.extended ? cells_32k_extended : cells_32k_normal)[pp];
        tr_cells = 288;
        break;
    default:
        fail("internal error: no carrier-cell table for FFT");
    }

    if (c.papr_tr && cells.data != 0) {
        cells.data -= tr_cells;
        if (cells.frame_closing != 0) cells.frame_closing -= tr_cells;
        if (cells.frame_closing_data != 0) cells.frame_closing_data -= tr_cells;
    }
    if ((ratio_is(c.guard_num, c.guard_den, 1, 128) && c.pilot == 7) ||
        (ratio_is(c.guard_num, c.guard_den, 1, 32) && c.pilot == 4) ||
        (ratio_is(c.guard_num, c.guard_den, 1, 16) && c.pilot == 2) ||
        (ratio_is(c.guard_num, c.guard_den, 19, 256) && c.pilot == 2)) {
        cells.frame_closing = 0;
        cells.frame_closing_data = 0;
    }
    return cells;
}

int ceil_to_multiple(int value, int multiple)
{
    return ((value + multiple - 1) / multiple) * multiple;
}

int p2_symbols_for_fft(int fft)
{
    switch (fft) {
    case 1024: return 16;
    case 2048: return 8;
    case 4096: return 4;
    case 8192: return 2;
    case 16384:
    case 32768: return 1;
    default: fail("internal error: no P2-symbol count for FFT");
    }
}

int kbch_bits_for(const Config& c)
{
    if (c.frame_size == 64800) {
        if (ratio_is(c.rate_num, c.rate_den, 1, 2)) return 32208;
        if (ratio_is(c.rate_num, c.rate_den, 3, 5)) return 38688;
        if (ratio_is(c.rate_num, c.rate_den, 2, 3)) return 43040;
        if (ratio_is(c.rate_num, c.rate_den, 3, 4)) return 48408;
        if (ratio_is(c.rate_num, c.rate_den, 4, 5)) return 51648;
        if (ratio_is(c.rate_num, c.rate_den, 5, 6)) return 53840;
    } else {
        if (ratio_is(c.rate_num, c.rate_den, 1, 3)) return 5232;
        if (ratio_is(c.rate_num, c.rate_den, 2, 5)) return 6312;
        if (ratio_is(c.rate_num, c.rate_den, 1, 2)) return 7032;
        if (ratio_is(c.rate_num, c.rate_den, 3, 5)) return 9552;
        if (ratio_is(c.rate_num, c.rate_den, 2, 3)) return 10632;
        if (ratio_is(c.rate_num, c.rate_den, 3, 4)) return 11712;
        if (ratio_is(c.rate_num, c.rate_den, 4, 5)) return 12432;
        if (ratio_is(c.rate_num, c.rate_den, 5, 6)) return 13152;
    }
    fail("internal error: no KBCH size for frame/code-rate combination");
}

std::uint64_t output_samples_per_t2_frame(const Config& c)
{
    const std::uint64_t cp =
        (static_cast<std::uint64_t>(c.fft) * c.guard_num) / c.guard_den;
    return 2048 +
           static_cast<std::uint64_t>(p2_symbols_for_fft(c.fft) + c.data_symbols) *
               (static_cast<std::uint64_t>(c.fft) + cp);
}

std::uint64_t input_bytes_for_t2_frames(const Config& c, std::uint64_t frames)
{
    const std::uint64_t payload_bytes_per_fec =
        static_cast<std::uint64_t>(kbch_bits_for(c) - 80) / 8;
    const std::uint64_t fec_per_frame = static_cast<std::uint64_t>(c.plp_fec_blocks);
    if (frames != 0 &&
        payload_bytes_per_fec >
            std::numeric_limits<std::uint64_t>::max() / fec_per_frame / frames) {
        fail("requested --samples value makes the required input size overflow");
    }
    const std::uint64_t payload_bytes = payload_bytes_per_fec * fec_per_frame * frames;
    if (!c.high_efficiency) {
        return payload_bytes;
    }
    const std::uint64_t removed_sync_bytes = payload_bytes / 187 +
                                             (payload_bytes % 187 != 0 ? 1 : 0);
    if (payload_bytes >
        std::numeric_limits<std::uint64_t>::max() - removed_sync_bytes) {
        fail("required high-efficiency input size overflow");
    }
    return payload_bytes + removed_sync_bytes;
}

void validate_frame_capacity(const Config& c)
{
    const CarrierCells cells = carrier_cells_for(c);
    if (cells.data == 0) {
        fail("unsupported FFT/carrier-mode/pilot-pattern combination");
    }

    const int p2_symbols = p2_symbols_for_fft(c.fft);
    int p2_cells = 0;
    switch (c.fft) {
    case 1024: p2_cells = 558; break;
    case 2048: p2_cells = 1118; break;
    case 4096: p2_cells = 2236; break;
    case 8192: p2_cells = 4472; break;
    case 16384: p2_cells = 8944; break;
    case 32768: p2_cells = 22432; break;
    default: fail("internal error: no P2 table for FFT");
    }

    int l1_bits_per_cell = 0;
    switch (c.l1_qam) {
    case 1: l1_bits_per_cell = 1; break;
    case 4: l1_bits_per_cell = 2; break;
    case 16: l1_bits_per_cell = 4; break;
    case 64: l1_bits_per_cell = 6; break;
    default: fail("internal error: no L1 bits-per-cell mapping");
    }
    constexpr int l1_post_bits_before_alignment = 1500;
    const int l1_alignment = p2_symbols == 1
                                 ? 2 * l1_bits_per_cell
                                 : p2_symbols * l1_bits_per_cell;
    const int l1_post_cells =
        ceil_to_multiple(l1_post_bits_before_alignment, l1_alignment) /
        l1_bits_per_cell;

    const std::int64_t mapped_cells =
        cells.frame_closing == 0
            ? static_cast<std::int64_t>(p2_symbols) * p2_cells +
                  static_cast<std::int64_t>(c.data_symbols) * cells.data
            : static_cast<std::int64_t>(p2_symbols) * p2_cells +
                  static_cast<std::int64_t>(c.data_symbols - 1) * cells.data +
                  cells.frame_closing;
    const std::int64_t fixed_cells =
        1840 + l1_post_cells + cells.frame_closing - cells.frame_closing_data;
    const int payload_cells_per_fec = c.frame_size / (c.qam == 4   ? 2
                                                            : c.qam == 16 ? 4
                                                            : c.qam == 64 ? 6
                                                                          : 8);
    const std::int64_t available_payload_cells = mapped_cells - fixed_cells;
    const int maximum_fec_blocks =
        available_payload_cells > 0
            ? static_cast<int>(available_payload_cells / payload_cells_per_fec)
            : 0;
    if (c.plp_fec_blocks > maximum_fec_blocks) {
        fail("--fec-blocks exceeds T2-frame capacity for these numeric parameters; "
             "maximum is " +
             std::to_string(maximum_fec_blocks));
    }
}

void validate_config(const Config& c)
{
    if (!one_of(c.bandwidth_khz, { 1700, 5000, 6000, 7000, 8000, 10000 })) {
        fail("unsupported --bandwidth-khz");
    }
    if (!one_of(c.fft, { 1024, 2048, 4096, 8192, 16384, 32768 })) {
        fail("unsupported --fft");
    }
    if (c.extended && c.fft < 8192) {
        fail("--extended 1 requires FFT 8192, 16384, or 32768");
    }
    if (!one_of(c.qam, { 4, 16, 64, 256 })) {
        fail("unsupported --qam");
    }
    const bool known_rate = ratio_is(c.rate_num, c.rate_den, 1, 3) ||
                            ratio_is(c.rate_num, c.rate_den, 2, 5) ||
                            ratio_is(c.rate_num, c.rate_den, 1, 2) ||
                            ratio_is(c.rate_num, c.rate_den, 3, 5) ||
                            ratio_is(c.rate_num, c.rate_den, 2, 3) ||
                            ratio_is(c.rate_num, c.rate_den, 3, 4) ||
                            ratio_is(c.rate_num, c.rate_den, 4, 5) ||
                            ratio_is(c.rate_num, c.rate_den, 5, 6);
    if (!known_rate) {
        fail("unsupported code-rate numerator/denominator");
    }
    if (ratio_is(c.rate_num, c.rate_den, 1, 3) ||
        ratio_is(c.rate_num, c.rate_den, 2, 5)) {
        fail("DVB-T2 v1.1.1 does not signal code rate 1/3 or 2/5");
    }
    const bool known_guard = ratio_is(c.guard_num, c.guard_den, 1, 32) ||
                             ratio_is(c.guard_num, c.guard_den, 1, 16) ||
                             ratio_is(c.guard_num, c.guard_den, 1, 8) ||
                             ratio_is(c.guard_num, c.guard_den, 1, 4) ||
                             ratio_is(c.guard_num, c.guard_den, 1, 128) ||
                             ratio_is(c.guard_num, c.guard_den, 19, 128) ||
                             ratio_is(c.guard_num, c.guard_den, 19, 256);
    if (!known_guard) {
        fail("unsupported guard-interval numerator/denominator");
    }
    const bool new_guard = ratio_is(c.guard_num, c.guard_den, 1, 128) ||
                           ratio_is(c.guard_num, c.guard_den, 19, 128) ||
                           ratio_is(c.guard_num, c.guard_den, 19, 256);
    const bool expected_t2gi = (c.fft == 8192 || c.fft == 32768) && new_guard;
    if (c.t2gi != expected_t2gi) {
        fail(std::string("--t2gi must be ") + (expected_t2gi ? "1" : "0") +
             " for this DVB-T2 v1.1.1 FFT/guard combination");
    }
    const long long cp_product = static_cast<long long>(c.fft) * c.guard_num;
    if (c.guard_den <= 0 || cp_product % c.guard_den != 0) {
        fail("guard interval does not produce an integral cyclic prefix");
    }
    if (c.pilot < 1 || c.pilot > 8) {
        fail("--pilot must be in 1..8");
    }
    if (!pilot_allowed_for_fft_and_guard(c)) {
        fail("pilot pattern is not allowed for this SISO FFT/guard combination");
    }
    if (c.plp_fec_blocks < 1 || c.plp_fec_blocks > 1023) {
        fail("--fec-blocks must be in 1..1023");
    }
    if (c.ti_blocks < 1 || c.ti_blocks > 255 ||
        c.ti_blocks > c.plp_fec_blocks) {
        fail("--ti-blocks must be in 1..min(255, fec-blocks)");
    }
    const int minimum_data_symbols = c.fft == 32768 ? 3 : 7;
    if (c.data_symbols < minimum_data_symbols || c.data_symbols > 4095) {
        fail("--data-symbols is outside the DVB-T2 range for this FFT (minimum " +
             std::to_string(minimum_data_symbols) + ")");
    }
    if (c.fft == 32768 && ((p2_symbols_for_fft(c.fft) + c.data_symbols) % 2 != 0)) {
        fail("32K mode requires P2 plus data-symbol count to be even");
    }
    if (!one_of(c.frame_size, { 16200, 64800 })) {
        fail("--frame-size must be 16200 or 64800");
    }
    if (!one_of(c.l1_qam, { 1, 4, 16, 64 })) {
        fail("--l1-qam must be 1, 4, 16, or 64");
    }
    if (c.t2_frames < 2 || c.t2_frames > 255) {
        fail("--t2-frames must be in 2..255");
    }
    const std::uint64_t frame_samples = output_samples_per_t2_frame(c);
    const std::uint64_t cp =
        (static_cast<std::uint64_t>(c.fft) * c.guard_num) / c.guard_den;
    const double sample_rate = c.bandwidth_khz == 1700
                                   ? 131000000.0 / 71.0
                                   : static_cast<double>(c.bandwidth_khz) * 1000.0 *
                                         8.0 / 7.0;
    if (static_cast<double>(frame_samples) / sample_rate > 0.250000001) {
        int maximum_data_symbols = static_cast<int>(
            (0.25 * sample_rate - 2048.0) / static_cast<double>(c.fft + cp));
        maximum_data_symbols -= p2_symbols_for_fft(c.fft);
        if (c.fft == 32768 &&
            ((p2_symbols_for_fft(c.fft) + maximum_data_symbols) % 2 != 0)) {
            --maximum_data_symbols;
        }
        fail("--data-symbols makes the T2 frame longer than 250 ms; maximum is " +
             std::to_string(maximum_data_symbols));
    }
    validate_frame_capacity(c);
}

dtv::dvb_framesize_t framesize_for(const Config& c)
{
    return c.frame_size == 64800 ? dtv::FECFRAME_NORMAL : dtv::FECFRAME_SHORT;
}

dtv::dvb_code_rate_t rate_for(const Config& c)
{
    if (ratio_is(c.rate_num, c.rate_den, 1, 3)) return dtv::C1_3;
    if (ratio_is(c.rate_num, c.rate_den, 2, 5)) return dtv::C2_5;
    if (ratio_is(c.rate_num, c.rate_den, 1, 2)) return dtv::C1_2;
    if (ratio_is(c.rate_num, c.rate_den, 3, 5)) return dtv::C3_5;
    if (ratio_is(c.rate_num, c.rate_den, 2, 3)) return dtv::C2_3;
    if (ratio_is(c.rate_num, c.rate_den, 3, 4)) return dtv::C3_4;
    if (ratio_is(c.rate_num, c.rate_den, 4, 5)) return dtv::C4_5;
    if (ratio_is(c.rate_num, c.rate_den, 5, 6)) return dtv::C5_6;
    fail("internal error: unmapped code rate");
}

dtv::dvb_constellation_t constellation_for(int qam)
{
    switch (qam) {
    case 4: return dtv::MOD_QPSK;
    case 16: return dtv::MOD_16QAM;
    case 64: return dtv::MOD_64QAM;
    case 256: return dtv::MOD_256QAM;
    default: fail("internal error: unmapped constellation");
    }
}

dtv::dvbt2_fftsize_t fftsize_for(const Config& c)
{
    switch (c.fft) {
    case 1024: return dtv::FFTSIZE_1K;
    case 2048: return dtv::FFTSIZE_2K;
    case 4096: return dtv::FFTSIZE_4K;
    case 8192: return c.t2gi ? dtv::FFTSIZE_8K_T2GI : dtv::FFTSIZE_8K;
    case 16384: return c.t2gi ? dtv::FFTSIZE_16K_T2GI : dtv::FFTSIZE_16K;
    case 32768: return c.t2gi ? dtv::FFTSIZE_32K_T2GI : dtv::FFTSIZE_32K;
    default: fail("internal error: unmapped FFT");
    }
}

dtv::dvb_guardinterval_t guard_for(const Config& c)
{
    if (ratio_is(c.guard_num, c.guard_den, 1, 32)) return dtv::GI_1_32;
    if (ratio_is(c.guard_num, c.guard_den, 1, 16)) return dtv::GI_1_16;
    if (ratio_is(c.guard_num, c.guard_den, 1, 8)) return dtv::GI_1_8;
    if (ratio_is(c.guard_num, c.guard_den, 1, 4)) return dtv::GI_1_4;
    if (ratio_is(c.guard_num, c.guard_den, 1, 128)) return dtv::GI_1_128;
    if (ratio_is(c.guard_num, c.guard_den, 19, 128)) return dtv::GI_19_128;
    if (ratio_is(c.guard_num, c.guard_den, 19, 256)) return dtv::GI_19_256;
    fail("internal error: unmapped guard interval");
}

dtv::dvbt2_l1constellation_t l1_constellation_for(int qam)
{
    switch (qam) {
    case 1: return dtv::L1_MOD_BPSK;
    case 4: return dtv::L1_MOD_QPSK;
    case 16: return dtv::L1_MOD_16QAM;
    case 64: return dtv::L1_MOD_64QAM;
    default: fail("internal error: unmapped L1 constellation");
    }
}

dtv::dvbt2_pilotpattern_t pilot_for(int pilot)
{
    switch (pilot) {
    case 1: return dtv::PILOT_PP1;
    case 2: return dtv::PILOT_PP2;
    case 3: return dtv::PILOT_PP3;
    case 4: return dtv::PILOT_PP4;
    case 5: return dtv::PILOT_PP5;
    case 6: return dtv::PILOT_PP6;
    case 7: return dtv::PILOT_PP7;
    case 8: return dtv::PILOT_PP8;
    default: fail("internal error: unmapped pilot pattern");
    }
}

dtv::dvbt2_bandwidth_t bandwidth_for(int khz)
{
    switch (khz) {
    case 1700: return dtv::BANDWIDTH_1_7_MHZ;
    case 5000: return dtv::BANDWIDTH_5_0_MHZ;
    case 6000: return dtv::BANDWIDTH_6_0_MHZ;
    case 7000: return dtv::BANDWIDTH_7_0_MHZ;
    case 8000: return dtv::BANDWIDTH_8_0_MHZ;
    case 10000: return dtv::BANDWIDTH_10_0_MHZ;
    default: fail("internal error: unmapped bandwidth");
    }
}

double sample_rate_for(const Config& c)
{
    if (c.bandwidth_khz == 1700) {
        return 131000000.0 / 71.0;
    }
    return static_cast<double>(c.bandwidth_khz) * 1000.0 * 8.0 / 7.0;
}

void validate_input(const Options& options)
{
    if (options.input == "-") {
        if (options.repeat) {
            fail("--repeat 1 cannot be used with stdin");
        }
        return;
    }

    std::error_code ec;
    const fs::path input_path = platform::path_from_utf8(options.input);
    const fs::file_status status = fs::status(input_path, ec);
    if (ec || !fs::exists(status)) {
        fail("input does not exist: " + options.input);
    }
    if (!fs::is_regular_file(status)) {
        fail("INPUT path must be a regular file; use '-' for a pipe or stream");
    }

    const std::uintmax_t size = fs::file_size(input_path, ec);
    if (ec || size == 0) {
        fail("input TS is empty or unreadable");
    }
    if (size % 188 != 0) {
        fail("input size is not a multiple of 188-byte MPEG-TS packets");
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        fail("cannot open input: " + options.input);
    }
    const std::uintmax_t packets = size / 188;
    std::array<unsigned char, 188> packet{};
    for (std::uintmax_t i = 0; i < packets; ++i) {
        input.read(reinterpret_cast<char*>(packet.data()), packet.size());
        if (!input || packet[0] != 0x47) {
            fail("input MPEG-TS sync check failed at packet " +
                 std::to_string(i));
        }
    }

    if (!options.repeat && options.samples != 0) {
        const std::uint64_t samples_per_frame =
            output_samples_per_t2_frame(options.config);
        const std::uint64_t required_frames =
            options.samples / samples_per_frame +
            (options.samples % samples_per_frame != 0 ? 1 : 0);
        const std::uint64_t required_bytes =
            input_bytes_for_t2_frames(options.config, required_frames);
        if (size < required_bytes) {
            fail("input is too short for " + std::to_string(required_frames) +
                 " complete T2 frame(s); need at least " +
                 std::to_string(required_bytes) + " bytes");
        }
    }
}

void validate_output(const Options& options)
{
    if (options.output == "-") {
        return;
    }
    const fs::path output_path = platform::path_from_utf8(options.output);
    std::error_code ec;
    if (fs::exists(output_path, ec)) {
        if (!ec) {
            if (!fs::is_regular_file(output_path, ec) || ec) {
                fail("OUTPUT path must be a regular file; use '-' for a pipe or stream");
            }
            if (options.input != "-" &&
                fs::equivalent(platform::path_from_utf8(options.input),
                               output_path,
                               ec) &&
                !ec) {
                fail("INPUT and OUTPUT refer to the same file");
            }
            if (options.input == "-" &&
                platform::stdin_refers_to_file(options.output)) {
                fail("stdin and OUTPUT refer to the same file");
            }
        }
        if (!options.overwrite) {
            fail("output already exists; pass --overwrite 1 to replace it");
        }
    }
    const fs::path parent = output_path.parent_path();
    if (!parent.empty() && !fs::is_directory(parent, ec)) {
        fail("output directory does not exist: " + parent.string());
    }
}

Graph build_graph(const Options& options)
{
    const Config& c = options.config;
    const auto framesize = framesize_for(c);
    const auto rate = rate_for(c);
    const auto constellation = constellation_for(c.qam);
    const auto fftsize = fftsize_for(c);
    const auto guard = guard_for(c);
    const auto pilot = pilot_for(c.pilot);
    const auto carrier_mode =
        c.extended ? dtv::CARRIERS_EXTENDED : dtv::CARRIERS_NORMAL;
    const auto rotation = c.rotation ? dtv::ROTATION_ON : dtv::ROTATION_OFF;
    const auto bb_mode =
        c.high_efficiency ? dtv::INPUTMODE_HIEFF : dtv::INPUTMODE_NORMAL;
    const auto papr_mode = c.papr_tr ? dtv::PAPR_TR : dtv::PAPR_OFF;
    const int cp_length = (c.fft * c.guard_num) / c.guard_den;
    const std::uint64_t payload_bytes_per_t2_frame =
        static_cast<std::uint64_t>(kbch_bits_for(c) - 80) / 8 *
        static_cast<std::uint64_t>(c.plp_fec_blocks);

    Graph graph;
    graph.top = gr::make_top_block("dvbt2mod", true);
    graph.error_state = std::make_shared<async_error_state>();

    graph.source = ts_packet_source::make(options.input,
                                          options.repeat,
                                          !options.repeat && options.samples == 0,
                                          payload_bytes_per_t2_frame,
                                          c.high_efficiency,
                                          graph.error_state);
    const auto bbheader = dtv::dvb_bbheader_bb::make(dtv::STANDARD_DVBT2,
                                                     framesize,
                                                     rate,
                                                     dtv::RO_0_35,
                                                     bb_mode,
                                                     dtv::INBAND_OFF,
                                                     c.bb_fec_blocks,
                                                     c.ts_rate);
    const auto bbscrambler =
        dtv::dvb_bbscrambler_bb::make(dtv::STANDARD_DVBT2, framesize, rate);
    const auto bch = dtv::dvb_bch_bb::make(dtv::STANDARD_DVBT2, framesize, rate);
    const auto ldpc =
        dtv::dvb_ldpc_bb::make(dtv::STANDARD_DVBT2, framesize, rate, dtv::MOD_OTHER);
    const auto bit_interleaver =
        dtv::dvbt2_interleaver_bb::make(framesize, rate, constellation);
    const auto modulator =
        dtv::dvbt2_modulator_bc::make(framesize, constellation, rotation);
    const auto cell_interleaver = dtv::dvbt2_cellinterleaver_cc::make(
        framesize, constellation, c.plp_fec_blocks, c.ti_blocks);
    const auto frame_mapper = dtv::dvbt2_framemapper_cc::make(framesize,
                                                              rate,
                                                              constellation,
                                                              rotation,
                                                              c.plp_fec_blocks,
                                                              c.ti_blocks,
                                                              carrier_mode,
                                                              fftsize,
                                                              guard,
                                                              l1_constellation_for(c.l1_qam),
                                                              pilot,
                                                              c.t2_frames,
                                                              c.data_symbols,
                                                              papr_mode,
                                                              dtv::VERSION_111,
                                                              dtv::PREAMBLE_T2_SISO,
                                                              dtv::INPUTMODE_NORMAL,
                                                              dtv::RESERVED_OFF,
                                                              dtv::L1_SCRAMBLED_OFF,
                                                              dtv::INBAND_OFF);
    const auto frequency_interleaver = dtv::dvbt2_freqinterleaver_cc::make(
        carrier_mode,
        fftsize,
        pilot,
        guard,
        c.data_symbols,
        papr_mode,
        dtv::VERSION_111,
        dtv::PREAMBLE_T2_SISO);
    const auto pilot_generator = dtv::dvbt2_pilotgenerator_cc::make(carrier_mode,
                                                                    fftsize,
                                                                    pilot,
                                                                    guard,
                                                                    c.data_symbols,
                                                                    papr_mode,
                                                                    dtv::VERSION_111,
                                                                    dtv::PREAMBLE_T2_SISO,
                                                                    dtv::MISO_TX1,
                                                                    dtv::EQUALIZATION_OFF,
                                                                    bandwidth_for(c.bandwidth_khz),
                                                                    c.fft);
    const auto prefixer = gr::digital::ofdm_cyclic_prefixer::make(
        static_cast<std::size_t>(c.fft),
        static_cast<std::size_t>(c.fft + cp_length),
        0,
        "");
    const auto p1 = dtv::dvbt2_p1insertion_cc::make(carrier_mode,
                                                    fftsize,
                                                    guard,
                                                    c.data_symbols,
                                                    dtv::PREAMBLE_T2_SISO,
                                                    dtv::SHOWLEVELS_OFF,
                                                    c.p1_vclip);

    graph.top->connect(graph.source, 0, bbheader, 0);
    graph.top->connect(bbheader, 0, bbscrambler, 0);
    graph.top->connect(bbscrambler, 0, bch, 0);
    graph.top->connect(bch, 0, ldpc, 0);
    graph.top->connect(ldpc, 0, bit_interleaver, 0);
    graph.top->connect(bit_interleaver, 0, modulator, 0);
    graph.top->connect(modulator, 0, cell_interleaver, 0);
    graph.top->connect(cell_interleaver, 0, frame_mapper, 0);
    graph.top->connect(frame_mapper, 0, frequency_interleaver, 0);
    graph.top->connect(frequency_interleaver, 0, pilot_generator, 0);

    if (c.papr_tr) {
        const auto papr = dtv::dvbt2_paprtr_cc::make(carrier_mode,
                                                     fftsize,
                                                     pilot,
                                                     guard,
                                                     c.data_symbols,
                                                     dtv::PAPR_TR,
                                                     dtv::VERSION_111,
                                                     c.papr_vclip,
                                                     c.papr_iterations,
                                                     c.fft);
        graph.top->connect(pilot_generator, 0, papr, 0);
        graph.top->connect(papr, 0, prefixer, 0);
    } else {
        graph.top->connect(pilot_generator, 0, prefixer, 0);
    }
    graph.top->connect(prefixer, 0, p1, 0);

    gr::basic_block_sptr tail = p1;
    if (options.samples != 0) {
        const auto head = gr::blocks::head::make(sizeof(gr_complex), options.samples);
        graph.top->connect(tail, 0, head, 0);
        tail = head;
    }

    if (options.check_only) {
        const auto sink = gr::blocks::null_sink::make(sizeof(gr_complex));
        graph.top->connect(tail, 0, sink, 0);
    } else {
        graph.output_sink = interruptible_output_sink::make(
            options.output,
            options.overwrite,
            graph.source ? graph.source->file_descriptor() : -1,
            graph.error_state);
        graph.top->connect(tail, 0, graph.output_sink, 0);
    }
    return graph;
}

void print_config(const Options& options)
{
    const Config& c = options.config;
    std::cerr << "profile=" << c.profile << " bw_khz=" << c.bandwidth_khz
              << " sample_rate=" << std::fixed << std::setprecision(6)
              << sample_rate_for(c) << " fft=" << c.fft << " t2gi=" << c.t2gi
              << " qam=" << c.qam << " rate=" << c.rate_num << '/' << c.rate_den
              << " gi=" << c.guard_num << '/' << c.guard_den << " pp=" << c.pilot
              << " fec=" << c.plp_fec_blocks << " ti=" << c.ti_blocks
              << " data_symbols=" << c.data_symbols << " l1_qam=" << c.l1_qam
              << " rotation=" << c.rotation << " extended=" << c.extended
              << " he=" << c.high_efficiency << " papr_tr=" << c.papr_tr
              << " repeat=" << options.repeat << " samples=" << options.samples << '\n';
}

int execute_flowgraph(const gr::top_block_sptr& top,
                      const std::shared_ptr<async_error_state>& error_state)
{
    std::atomic<bool> finished{ false };
    std::exception_ptr wait_error;
    std::thread waiter;
    const auto join_after_stop = [&] {
        if (!waiter.joinable()) return;
#if defined(_WIN32)
        // Cancellation errors must never turn Ctrl-C or a broken pipe into a
        // permanently wedged process. Supported pipe/file I/O normally exits
        // immediately; this is a last-resort process-level safety boundary.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (!finished.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished.load(std::memory_order_acquire)) {
            const unsigned int exit_code =
                load_caught_signal() == 0
                    ? 2U
                    : 128U + static_cast<unsigned int>(load_caught_signal());
            ::TerminateProcess(::GetCurrentProcess(), exit_code);
            std::_Exit(static_cast<int>(exit_code));
        }
#endif
        waiter.join();
    };
    bool stop_initiated = false;
    try {
        top->start();
        waiter = std::thread([&] {
            try {
                top->wait();
            } catch (...) {
                wait_error = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });

        while (!finished.load(std::memory_order_acquire)) {
            if (load_caught_signal() != 0 || error_state->failed()) {
                stop_initiated = true;
                top->stop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop_initiated) {
            join_after_stop();
        } else {
            waiter.join();
        }
    } catch (...) {
        top->stop();
        join_after_stop();
        throw;
    }

    if (wait_error) std::rethrow_exception(wait_error);
    error_state->rethrow_if_failed();
    return load_caught_signal();
}

int run(int argc, char** argv)
{
    const Options options = parse_options(argc, argv);
    const scoped_signal_handlers signal_handlers;
    const std::uint16_t endian_probe = 1;
    if (*reinterpret_cast<const std::uint8_t*>(&endian_probe) != 1) {
        fail("CF32LE output is supported only on a little-endian host");
    }
    validate_config(options.config);
    validate_input(options);
    validate_output(options);
    print_config(options);

    if (options.output == "-") {
        // Some GNU Radio Windows builds attach their debug console sink to
        // stdout. That would prepend scheduler messages to the headerless CF32
        // stream, so reserve stdout exclusively for payload bytes. dvbt2mod's
        // own diagnostics and errors continue to use stderr.
        auto& logging = gr::logging::singleton();
        logging.set_default_level(gr::log_level::off);
        logging.set_debug_level(gr::log_level::off);
    }

    Graph graph = build_graph(options);
    if (options.check_only) {
        std::cerr << "configuration accepted; graph constructed\n";
        return 0;
    }

    const auto started = std::chrono::steady_clock::now();
    const int signal_number = execute_flowgraph(graph.top, graph.error_state);
    if (graph.output_sink) graph.output_sink->close();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::uint64_t output_bytes = 0;
    std::uint64_t produced_samples = 0;
    if (graph.output_sink) {
        output_bytes = graph.output_sink->bytes_written();
        produced_samples = output_bytes / sizeof(gr_complex);
    }
    if (options.output != "-") {
        std::error_code ec;
        const std::uintmax_t file_bytes = fs::file_size(
            platform::path_from_utf8(options.output), ec);
        if (ec) fail("cannot determine output file size after modulation");
        if (file_bytes != output_bytes)
            fail("output file size does not match bytes written by the sink");
    }
    if (signal_number == 0 && output_bytes % sizeof(gr_complex) != 0) {
        fail("output ended with a partial complex sample");
    }
    if (signal_number == 0 && produced_samples == 0) {
        fail("input ended before one complete T2 frame could be emitted");
    }
    if (signal_number == 0 && options.samples != 0 &&
        produced_samples != options.samples) {
        fail("input ended after " + std::to_string(produced_samples) +
             " samples, before requested --samples " +
             std::to_string(options.samples));
    }

    std::cerr << (signal_number == 0 ? "done" : "stopped") << " in " << std::fixed
              << std::setprecision(3) << elapsed << " s"
              << "; complex_samples=" << produced_samples;
    if (graph.source) {
        std::cerr << "; input_packets=" << graph.source->input_packets();
        if (signal_number == 0 && graph.source->padding_bytes() != 0) {
            std::cerr << "; null_padding_bytes=" << graph.source->padding_bytes();
        }
    }
    std::cerr << "; output_bytes=" << output_bytes;
    if (output_bytes % sizeof(gr_complex) != 0) {
        std::cerr << "; partial_sample_bytes="
                  << output_bytes % sizeof(gr_complex);
    }
    if (options.samples == 0) {
        const std::uint64_t frame_samples =
            output_samples_per_t2_frame(options.config);
        std::cerr << "; complete_t2_frames=" << produced_samples / frame_samples;
    }
    std::cerr << '\n';
    return signal_number == 0 ? 0 : 128 + signal_number;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide_argv)
{
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            arguments.push_back(platform::utf8_from_wide(wide_argv[index]));
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size());
        for (std::string& argument : arguments) argv.push_back(argument.data());
        return run(argc, argv.data());
    } catch (const std::exception& error) {
        std::cerr << "dvbt2mod: error: " << error.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "dvbt2mod: error: " << error.what() << '\n';
        return 2;
    }
}
#endif
