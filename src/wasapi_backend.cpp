// WASAPI backend, written from the Windows Core Audio API documentation
// (IMMDeviceEnumerator, IAudioClient, IAudioCaptureClient, loopback
// recording). Built on Windows only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
// windows.h must come first.
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio_observer/wasapi_backend.hpp"
#include "audio_observer/wasapi_format.hpp"

namespace audio_observer {

namespace {

// The portable code mirrors these SDK constants; keep them in step.
static_assert(wasapi::kFormatTagPcm == WAVE_FORMAT_PCM, "WAVE_FORMAT_PCM");
static_assert(wasapi::kFormatTagIeeeFloat == WAVE_FORMAT_IEEE_FLOAT, "WAVE_FORMAT_IEEE_FLOAT");
static_assert(wasapi::kFormatTagExtensible == WAVE_FORMAT_EXTENSIBLE, "WAVE_FORMAT_EXTENSIBLE");
static_assert(wasapi::kBufferFlagDataDiscontinuity == AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY,
              "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY");
static_assert(wasapi::kBufferFlagSilent == AUDCLNT_BUFFERFLAGS_SILENT, "AUDCLNT_BUFFERFLAGS_SILENT");
static_assert(wasapi::kBufferFlagTimestampError == AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR,
              "AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR");

// PKEY_Device_FriendlyName, defined here so that no GUID library is needed.
const PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Requested shared-mode buffer, in 100 ns units (100 ms). Windows may make
// it larger; the actual size bounds the largest packet.
constexpr REFERENCE_TIME kBufferDurationHns = 1'000'000;
// The capture thread waits for the stream event at most this long before
// polling anyway, so it notices stop requests and keeps working if an event
// is missed.
constexpr DWORD kWaitMilliseconds = 20;

// ---------------------------------------------------------------- RAII

// Owns one reference to a COM interface.
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() { reset(); }
    ComPtr(ComPtr&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = std::exchange(other.p_, nullptr);
        }
        return *this;
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    // Releases the current interface and returns the slot for an out-parameter.
    T** put() noexcept {
        reset();
        return &p_;
    }
    void** put_void() noexcept { return reinterpret_cast<void**>(put()); }
    void reset() noexcept {
        if (p_ != nullptr) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

// Owns memory returned by COM with CoTaskMemAlloc.
template <typename T>
class CoTaskMem {
public:
    CoTaskMem() noexcept = default;
    ~CoTaskMem() { CoTaskMemFree(p_); }
    CoTaskMem(const CoTaskMem&) = delete;
    CoTaskMem& operator=(const CoTaskMem&) = delete;
    T* get() const noexcept { return p_; }
    T** put() noexcept {
        CoTaskMemFree(p_);
        p_ = nullptr;
        return &p_;
    }

private:
    T* p_ = nullptr;
};

class PropVariant {
public:
    PropVariant() noexcept { PropVariantInit(&value_); }
    ~PropVariant() { PropVariantClear(&value_); }
    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;
    PROPVARIANT* get() noexcept { return &value_; }

private:
    PROPVARIANT value_;
};

class EventHandle {
public:
    EventHandle() noexcept = default;
    ~EventHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
    void reset(HANDLE handle) noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

// COM on the current thread, in the multithreaded apartment, until destroyed.
// Must be destroyed on the thread that created it.
class ComApartment {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

// ---------------------------------------------------------------- helpers

std::string to_utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));  // drop the terminator
    return out;
}

const char* describe_hresult(HRESULT hr) noexcept {
    switch (hr) {
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return "the device was removed, disabled or reconfigured";
        case AUDCLNT_E_DEVICE_IN_USE:
            return "the device is in exclusive use by another application";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:
            return "the Windows audio service is not running";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return "the format is not supported";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
            return "the endpoint could not be created";
        case AUDCLNT_E_RESOURCES_INVALIDATED:
            return "the stream's resources were invalidated";
        case E_ACCESSDENIED:
            return "access denied (check the Windows microphone privacy settings)";
        case E_OUTOFMEMORY:
            return "out of memory";
        case E_INVALIDARG:
        case E_POINTER:
            return "invalid argument";
        default:
            return "unexpected error";
    }
}

std::string hresult_message(const std::string& what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return what + ": " + describe_hresult(hr) + " (HRESULT " + code + ")";
}

void check(HRESULT hr, const std::string& what) {
    if (FAILED(hr)) {
        throw BackendError(hresult_message(what, hr));
    }
}

// True when @p guid is the WAVEFORMATEXTENSIBLE sub-format for @p tag: the
// documented scheme puts the format tag in the first field of the GUID
// {tag-0000-0010-8000-00AA00389B71}.
bool is_subformat(const GUID& guid, unsigned long tag) noexcept {
    static const unsigned char kTail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag && guid.Data2 == 0x0000 && guid.Data3 == 0x0010 &&
           std::memcmp(guid.Data4, kTail, sizeof kTail) == 0;
}

wasapi::WaveFormatFields fields_of(const WAVEFORMATEX& w) noexcept {
    wasapi::WaveFormatFields f;
    f.format_tag = w.wFormatTag;
    f.channels = w.nChannels;
    f.samples_per_sec = w.nSamplesPerSec;
    f.block_align = w.nBlockAlign;
    f.bits_per_sample = w.wBitsPerSample;
    if (w.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        w.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& x = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(w);
        f.valid_bits_per_sample = x.Samples.wValidBitsPerSample;
        if (is_subformat(x.SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            f.sub_format = wasapi::SubFormat::ieee_float;
        } else if (is_subformat(x.SubFormat, WAVE_FORMAT_PCM)) {
            f.sub_format = wasapi::SubFormat::pcm;
        } else {
            f.sub_format = wasapi::SubFormat::other;
        }
    } else if (w.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        f.sub_format = wasapi::SubFormat::other;  // truncated structure
    }
    return f;
}

// One active endpoint, as listed.
struct Endpoint {
    EndpointInfo info;
    std::string id;
    ComPtr<IMMDevice> device;
};

std::string friendly_name(IMMDevice& device) {
    ComPtr<IPropertyStore> store;
    check(device.OpenPropertyStore(STGM_READ, store.put()), "Reading endpoint properties");
    PropVariant value;
    check(store->GetValue(kFriendlyNameKey, value.get()), "Reading an endpoint name");
    if (value.get()->vt != VT_LPWSTR) {
        throw BackendError("An active endpoint has no friendly name");
    }
    return to_utf8(value.get()->pwszVal);
}

std::string endpoint_id(IMMDevice& device) {
    CoTaskMem<wchar_t> id;
    check(device.GetId(id.put()), "Reading an endpoint ID");
    return to_utf8(id.get());
}

// Lists active capture endpoints and active render endpoints (as loopback
// sources). An endpoint whose name cannot be read fails the whole listing,
// so that a name can never look unique because a namesake was skipped.
std::vector<Endpoint> enumerate(IMMDeviceEnumerator& enumerator) {
    std::vector<Endpoint> endpoints;
    for (const EDataFlow flow : {eCapture, eRender}) {
        ComPtr<IMMDeviceCollection> collection;
        check(enumerator.EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()),
              "Listing audio endpoints");
        UINT count = 0;
        check(collection->GetCount(&count), "Counting audio endpoints");
        for (UINT i = 0; i < count; ++i) {
            Endpoint e;
            check(collection->Item(i, e.device.put()), "Reading an audio endpoint");
            e.info.name = friendly_name(*e.device.get());
            e.id = endpoint_id(*e.device.get());
            e.info.direction = flow == eCapture ? EndpointDirection::input : EndpointDirection::output;
            e.info.loopback = flow == eRender;
            // Reading the mix format activates a client object but
            // initializes no stream. A failure leaves the format empty;
            // opening the endpoint then reports the error.
            ComPtr<IAudioClient> client;
            CoTaskMem<WAVEFORMATEX> mix;
            if (SUCCEEDED(e.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             client.put_void())) &&
                SUCCEEDED(client->GetMixFormat(mix.put())) && mix.get() != nullptr) {
                const wasapi::WaveFormatFields fields = fields_of(*mix.get());
                if (const auto layout = wasapi::describe_mix_format(fields)) {
                    e.info.format = layout->format;
                } else {
                    e.info.format.sample_rate_hz = fields.samples_per_sec;
                    e.info.format.channel_count = fields.channels;
                }
            }
            endpoints.push_back(std::move(e));
        }
    }
    return endpoints;
}

// ---------------------------------------------------------------- stream

// Which call failed on the capture thread. The thread records only this and
// the HRESULT, so it never allocates; error() builds the message.
enum class FailSite : int {
    none,
    com_init,
    wait,
    next_packet_size,
    get_buffer,
    release_buffer,
};

const char* to_string(FailSite site) noexcept {
    switch (site) {
        case FailSite::none:
            return "";
        case FailSite::com_init:
            return "Initializing COM on the capture thread";
        case FailSite::wait:
            return "Waiting for the capture event";
        case FailSite::next_packet_size:
            return "IAudioCaptureClient::GetNextPacketSize";
        case FailSite::get_buffer:
            return "IAudioCaptureClient::GetBuffer";
        case FailSite::release_buffer:
            return "IAudioCaptureClient::ReleaseBuffer";
    }
    return "";
}

class WasapiCaptureStream final : public CaptureStream {
public:
    WasapiCaptureStream(std::shared_ptr<ComApartment> com, Endpoint& endpoint,
                        StreamDirection direction)
        : com_(std::move(com)), device_(std::move(endpoint.device)) {
        const std::string& name = endpoint.info.name;
        check(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void()),
              "Activating an audio client on " + name);
        CoTaskMem<WAVEFORMATEX> mix;
        check(client_->GetMixFormat(mix.put()), "Reading the mix format of " + name);
        if (mix.get() == nullptr) {
            throw BackendError("Reading the mix format of " + name + ": no format returned");
        }
        std::string why;
        const auto layout = wasapi::describe_mix_format(fields_of(*mix.get()), &why);
        if (!layout) {
            throw BackendError("Unsupported mix format on " + name + ": " + why);
        }
        layout_ = *layout;

        // Shared mode only, in the mix format: nothing about the endpoint
        // or other applications' streams changes.
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (direction == StreamDirection::loopback) {
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        }
        check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDurationHns, 0,
                                  mix.get(), nullptr),
              "Opening a shared-mode " + std::string(audio_observer::to_string(direction)) +
                  " stream on " + name);
        event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (event_.get() == nullptr) {
            const DWORD code = GetLastError();
            throw BackendError(hresult_message("Creating the capture event",
                                               code != 0 ? HRESULT_FROM_WIN32(code) : E_FAIL));
        }
        check(client_->SetEventHandle(event_.get()), "Setting the capture event of " + name);
        check(client_->GetBufferSize(&buffer_frames_), "Reading the buffer size of " + name);
        if (buffer_frames_ == 0) {
            throw BackendError("The audio client of " + name + " reported an empty buffer");
        }
        check(client_->GetService(__uuidof(IAudioCaptureClient), capture_.put_void()),
              "Getting the capture client of " + name);
        scratch_ = std::make_unique<float[]>(static_cast<std::size_t>(buffer_frames_) *
                                             layout_.format.channel_count);

        info_.id = endpoint.id;
        info_.name = name;
        info_.direction = direction;
        info_.format = layout_.format;
    }

    ~WasapiCaptureStream() override { stop(); }

    const StreamInfo& info() const noexcept override { return info_; }
    std::size_t max_block_frames() const noexcept override { return buffer_frames_; }

    void start(std::shared_ptr<BlockQueue> queue) override {
        std::lock_guard<std::mutex> lock(control_);
        if (!queue || queue->channel_count() != layout_.format.channel_count) {
            throw BackendError("WASAPI stream started without a matching queue");
        }
        if (state_.load() != StreamState::opened) {
            throw BackendError("WASAPI stream can only be started once");
        }
        // Offset from the QPC clock (in which WASAPI stamps packets) to the
        // host steady clock, so block times compare with host_now().
        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        const HostTime now = host_now();
        const auto qpc_ns = std::chrono::nanoseconds(
            static_cast<std::int64_t>(wasapi::qpc_ticks_to_hns(
                static_cast<std::uint64_t>(counter.QuadPart),
                static_cast<std::uint64_t>(frequency.QuadPart))) *
            100);
        qpc_to_host_ = now.time_since_epoch() - qpc_ns;

        queue_ = std::move(queue);
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) {
            queue_.reset();
            throw BackendError(hresult_message("Starting " + info_.name, hr));
        }
        client_started_ = true;
        state_.store(StreamState::running);
        try {
            thread_ = std::thread([this] { capture_loop(); });
        } catch (...) {
            client_->Stop();
            client_started_ = false;
            queue_.reset();
            state_.store(StreamState::stopped);
            throw BackendError("Could not start the capture thread for " + info_.name);
        }
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(control_);
        stop_requested_.store(true);
        if (thread_.joinable()) {
            SetEvent(event_.get());
            thread_.join();
        }
        if (client_started_) {
            client_->Stop();
            client_started_ = false;
        }
        queue_.reset();
        StreamState expected = StreamState::running;
        if (!state_.compare_exchange_strong(expected, StreamState::stopped) &&
            expected == StreamState::opened) {
            state_.store(StreamState::stopped);  // stopped before it started
        }
    }

    StreamState state() const noexcept override { return state_.load(); }

    std::string error() const override {
        if (state_.load() != StreamState::failed) {
            return {};
        }
        return hresult_message(std::string(to_string(fail_site_.load())) + " on " + info_.name,
                               fail_hr_.load());
    }

private:
    void fail(FailSite site, HRESULT hr) noexcept {
        fail_hr_.store(hr);
        fail_site_.store(site);
        state_.store(StreamState::failed);
    }

    // Capture thread. After COM is initialized it does not allocate: it
    // waits, reads packets, converts them into the preallocated scratch
    // buffer and pushes them into the queue.
    void capture_loop() noexcept {
        ComApartment com;
        if (FAILED(com.result())) {
            fail(FailSite::com_init, com.result());
            return;
        }
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(event_.get(), kWaitMilliseconds);
            if (wait == WAIT_FAILED) {
                fail(FailSite::wait, HRESULT_FROM_WIN32(GetLastError()));
                return;
            }
            if (stop_requested_.load(std::memory_order_acquire) || !drain_packets()) {
                return;
            }
        }
    }

    // Reads every waiting packet. Returns false after a failure.
    bool drain_packets() noexcept {
        const std::size_t channels = layout_.format.channel_count;
        for (;;) {
            UINT32 packet_frames = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                fail(FailSite::next_packet_size, hr);
                return false;
            }
            if (packet_frames == 0) {
                return true;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD buffer_flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = capture_->GetBuffer(&data, &frames, &buffer_flags, &device_position, &qpc_position);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                return true;
            }
            if (FAILED(hr)) {
                fail(FailSite::get_buffer, hr);
                return false;
            }
            const wasapi::PacketMapping m =
                wasapi::map_packet(device_position, qpc_position, buffer_flags, qpc_to_host_);
            const bool fits = frames <= buffer_frames_;
            if (fits) {
                if (m.silent || data == nullptr) {
                    std::fill_n(scratch_.get(), static_cast<std::size_t>(frames) * channels, 0.0f);
                } else {
                    wasapi::convert_to_float(data, frames, layout_, scratch_.get());
                }
            }
            // Hand the device buffer back before queueing, to hold it briefly.
            hr = capture_->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                fail(FailSite::release_buffer, hr);
                return false;
            }
            if (frames == 0) {
                continue;
            }
            if (fits) {
                queue_->push(FrameBlock(scratch_.get(), frames, channels, m.first_sample_index,
                                        m.host_time),
                             m.flags);
            } else {
                queue_->drop(frames);  // cannot happen with a well-behaved driver
            }
        }
    }

    // Declaration order is release order in reverse: the capture client,
    // then the audio client, then the event it signalled, then the device,
    // and COM last.
    std::shared_ptr<ComApartment> com_;
    ComPtr<IMMDevice> device_;
    EventHandle event_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;

    StreamInfo info_;
    wasapi::SampleLayout layout_;
    UINT32 buffer_frames_ = 0;
    std::unique_ptr<float[]> scratch_;  // capture thread only, once started
    std::chrono::nanoseconds qpc_to_host_{0};

    std::mutex control_;  // serializes start() and stop()
    std::shared_ptr<BlockQueue> queue_;
    std::thread thread_;
    bool client_started_ = false;
    std::atomic<bool> stop_requested_{false};
    std::atomic<StreamState> state_{StreamState::opened};
    std::atomic<HRESULT> fail_hr_{S_OK};
    std::atomic<FailSite> fail_site_{FailSite::none};
};

// ---------------------------------------------------------------- backend

class WasapiBackend final : public Backend {
public:
    WasapiBackend() : com_(std::make_shared<ComApartment>()) {
        if (com_->result() == RPC_E_CHANGED_MODE) {
            throw BackendError(
                "The WASAPI backend needs a multithreaded COM apartment on this thread");
        }
        check(com_->result(), "Initializing COM");
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), enumerator_.put_void()),
              "Creating the audio endpoint enumerator");
    }

    std::string name() const override { return "wasapi"; }

    std::vector<EndpointInfo> endpoints() override {
        std::vector<EndpointInfo> infos;
        for (Endpoint& e : enumerate(*enumerator_.get())) {
            infos.push_back(std::move(e.info));
        }
        return infos;
    }

    std::unique_ptr<CaptureStream> open(const std::string& endpoint_name,
                                        StreamDirection direction) override {
        std::vector<Endpoint> listed = enumerate(*enumerator_.get());
        std::vector<EndpointInfo> infos;
        infos.reserve(listed.size());
        for (const Endpoint& e : listed) {
            infos.push_back(e.info);
        }
        // Fails closed: exactly one endpoint of the right kind, by exact name.
        const EndpointInfo& chosen = select_endpoint(infos, endpoint_name, direction);
        const auto index = static_cast<std::size_t>(&chosen - infos.data());
        return std::make_unique<WasapiCaptureStream>(com_, listed[index], direction);
    }

private:
    // COM is released last, after the enumerator.
    std::shared_ptr<ComApartment> com_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
};

}  // namespace

std::unique_ptr<Backend> make_wasapi_backend() {
    return std::make_unique<WasapiBackend>();
}

}  // namespace audio_observer

#endif  // _WIN32
