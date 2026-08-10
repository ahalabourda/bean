#include "app/ClipPreviewEngine.h"
#include "app/AppContext.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <audioclient.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <string>

namespace {

class EngineNotify : public IMFMediaEngineNotify {
public:
    explicit EngineNotify(ClipPreviewEngine* owner)
        : owner_(owner)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IMFMediaEngineNotify) {
            *object = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD eventCode, DWORD_PTR, DWORD) override;

private:
    ClipPreviewEngine* owner_ = nullptr;
    ULONG references_ = 1;
};

std::wstring FileUrl(const std::filesystem::path& path)
{
    std::array<wchar_t, 32768> buffer{};
    DWORD length = static_cast<DWORD>(buffer.size());
    if (SUCCEEDED(UrlCreateFromPathW(path.wstring().c_str(), buffer.data(), &length, 0))) {
        return std::wstring(buffer.data(), length);
    }
    return {};
}

} // namespace

const wchar_t* ClipPreviewEngine::VideoHostWindowClass()
{
    static constexpr wchar_t kClassName[] = L"BeanClipVideoHost";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW windowClass{};
        // Intentionally no CS_HREDRAW/CS_VREDRAW — those force full client
        // repaints on every size change and increase flicker.
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kClassName;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        windowClass.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
        RegisterClassW(&windowClass);
        registered = true;
    }
    return kClassName;
}

class ClipPreviewEngine::NotifyCallback final : public EngineNotify {
public:
    explicit NotifyCallback(ClipPreviewEngine* owner)
        : EngineNotify(owner)
    {
    }
};

HRESULT EngineNotify::EventNotify(DWORD eventCode, DWORD_PTR, DWORD)
{
    // The callback may run on a Media Foundation worker thread. Forward all
    // state changes to Bean's STA UI thread instead of touching window state here.
    if (owner_) {
        owner_->NotifyEvent(eventCode);
    }
    return S_OK;
}

ClipPreviewEngine::ClipPreviewEngine(HWND eventWindow, HWND containerWindow)
    : eventWindow_(eventWindow)
    , containerWindow_(containerWindow)
{
}

ClipPreviewEngine::~ClipPreviewEngine()
{
    Close();
    ReleaseEngine();
    DestroyPlaybackWindow();
    if (initialized_) {
        MFShutdown();
        initialized_ = false;
    }
}

HRESULT ClipPreviewEngine::Initialize()
{
    if (initialized_) {
        return S_OK;
    }
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        lastError_ = hr;
        return hr;
    }
    hr = CreateEngine();
    if (FAILED(hr)) {
        MFShutdown();
        lastError_ = hr;
        return hr;
    }
    initialized_ = true;
    return S_OK;
}

HRESULT ClipPreviewEngine::CreateEngine()
{
    if (!containerWindow_) {
        return E_INVALIDARG;
    }
    DestroyPlaybackWindow();
    playbackWindow_ = CreateWindowExW(
        0,
        VideoHostWindowClass(),
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        1,
        1,
        containerWindow_,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!playbackWindow_) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    IMFAttributes* attributes = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) {
        DestroyPlaybackWindow();
        return hr;
    }

    notify_ = new NotifyCallback(this);
    hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_);
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_GameMedia);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, reinterpret_cast<UINT64>(playbackWindow_));
    }
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(
            CLSID_MFMediaEngineClassFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory_));
    }
    if (SUCCEEDED(hr)) {
        hr = factory_->CreateInstance(0, attributes, &engine_);
    }
    attributes->Release();
    if (FAILED(hr)) {
        ReleaseEngine();
        DestroyPlaybackWindow();
        return hr;
    }

    hr = engine_->QueryInterface(IID_PPV_ARGS(&engineEx_));
    if (FAILED(hr)) {
        ReleaseEngine();
        DestroyPlaybackWindow();
        return hr;
    }
    engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    engine_->SetAutoPlay(FALSE);
    engineEx_->EnableTimeUpdateTimer(TRUE);
    return S_OK;
}

void ClipPreviewEngine::DestroyPlaybackWindow()
{
    if (playbackWindow_) {
        DestroyWindow(playbackWindow_);
        playbackWindow_ = nullptr;
    }
}

void ClipPreviewEngine::ReleaseEngine()
{
    if (engineEx_) {
        engineEx_->Release();
        engineEx_ = nullptr;
    }
    if (engine_) {
        engine_->Release();
        engine_ = nullptr;
    }
    if (factory_) {
        factory_->Release();
        factory_ = nullptr;
    }
    if (notify_) {
        notify_->Release();
        notify_ = nullptr;
    }
}

HRESULT ClipPreviewEngine::Open(const std::filesystem::path& path)
{
    if (!initialized_ || !engine_) {
        return E_UNEXPECTED;
    }
    Close();
    const auto url = FileUrl(path);
    if (url.empty()) {
        lastError_ = E_INVALIDARG;
        return lastError_;
    }
    BSTR source = SysAllocString(url.c_str());
    if (!source) {
        lastError_ = E_OUTOFMEMORY;
        return lastError_;
    }
    HRESULT hr = engine_->SetSource(source);
    SysFreeString(source);
    if (SUCCEEDED(hr)) {
        hr = engine_->Load();
    }
    if (FAILED(hr)) {
        lastError_ = hr;
    }
    ready_ = false;
    return hr;
}

void ClipPreviewEngine::NotifyEvent(DWORD eventCode)
{
    if (eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAY
        || eventCode == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH) {
        ready_ = true;
    } else if (eventCode == MF_MEDIA_ENGINE_EVENT_ERROR) {
        ready_ = false;
        lastError_ = E_FAIL;
    }
    if (eventWindow_) {
        PostMessageW(eventWindow_, WM_BEAN_CLIPS_MEDIA_EVENT, eventCode, 0);
    }
}

void ClipPreviewEngine::Close()
{
    ready_ = false;
    cachedVideoWidth_ = 0;
    cachedVideoHeight_ = 0;
    if (engine_) {
        engine_->Pause();
        engine_->SetSource(nullptr);
    }
}

HRESULT ClipPreviewEngine::Play()
{
    if (!engine_) {
        return E_UNEXPECTED;
    }
    return engine_->Play();
}

HRESULT ClipPreviewEngine::Pause()
{
    if (!engine_) {
        return E_UNEXPECTED;
    }
    return engine_->Pause();
}

HRESULT ClipPreviewEngine::SeekMilliseconds(int milliseconds)
{
    if (!engine_) {
        return E_UNEXPECTED;
    }
    return engine_->SetCurrentTime(static_cast<double>((std::max)(0, milliseconds)) / 1000.0);
}

HRESULT ClipPreviewEngine::SetVolumePercent(int percent)
{
    if (!engine_) {
        return E_UNEXPECTED;
    }
    const double volume = static_cast<double>((std::clamp)(percent, 0, 100)) / 100.0;
    return engine_->SetVolume(volume);
}

HRESULT ClipPreviewEngine::UpdatePlaybackWindow()
{
    if (!containerWindow_ || !playbackWindow_) {
        return E_UNEXPECTED;
    }

    RECT container{};
    if (!GetClientRect(containerWindow_, &container)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const int containerWidth = container.right - container.left;
    const int containerHeight = container.bottom - container.top;
    if (containerWidth <= 0 || containerHeight <= 0) {
        return S_OK;
    }

    int videoWidth = 0;
    int videoHeight = 0;
    const auto nativeSize = NativeVideoSize();
    if (nativeSize.first > 0 && nativeSize.second > 0) {
        videoWidth = nativeSize.first;
        videoHeight = nativeSize.second;
        cachedVideoWidth_ = videoWidth;
        cachedVideoHeight_ = videoHeight;
    } else {
        videoWidth = cachedVideoWidth_;
        videoHeight = cachedVideoHeight_;
    }

    int destX = 0;
    int destY = 0;
    int destWidth = containerWidth;
    int destHeight = containerHeight;
    if (videoWidth > 0 && videoHeight > 0) {
        const double scale = (std::min)(
            static_cast<double>(containerWidth) / static_cast<double>(videoWidth),
            static_cast<double>(containerHeight) / static_cast<double>(videoHeight));
        destWidth = (std::max)(1, static_cast<int>(videoWidth * scale + 0.5));
        destHeight = (std::max)(1, static_cast<int>(videoHeight * scale + 0.5));
        destX = (containerWidth - destWidth) / 2;
        destY = (containerHeight - destHeight) / 2;
    }

    SetWindowPos(
        playbackWindow_,
        nullptr,
        destX,
        destY,
        destWidth,
        destHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);

    if (!engineEx_) {
        InvalidateRect(containerWindow_, nullptr, FALSE);
        return S_OK;
    }

    RECT destination{0, 0, destWidth, destHeight};
    const MFARGB borderColor{
        0,
        0,
        0,
        255};
    const HRESULT hr = engineEx_->UpdateVideoStream(nullptr, &destination, &borderColor);
    InvalidateRect(containerWindow_, nullptr, FALSE);
    return hr;
}

bool ClipPreviewEngine::IsReady() const
{
    return ready_;
}

bool ClipPreviewEngine::IsPlaying() const
{
    if (!engine_) {
        return false;
    }
    return engine_->IsPaused() == FALSE;
}

int ClipPreviewEngine::DurationMilliseconds() const
{
    if (!engine_) {
        return 0;
    }
    const double seconds = engine_->GetDuration();
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<int>((std::min)(seconds * 1000.0, 2147483647.0));
}

int ClipPreviewEngine::PositionMilliseconds() const
{
    if (!engine_) {
        return 0;
    }
    const double seconds = engine_->GetCurrentTime();
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<int>((std::min)(seconds * 1000.0, 2147483647.0));
}

std::pair<int, int> ClipPreviewEngine::NativeVideoSize() const
{
    if (!engineEx_) {
        return {0, 0};
    }
    DWORD width = 0;
    DWORD height = 0;
    if (FAILED(engine_->GetNativeVideoSize(&width, &height))) {
        return {0, 0};
    }
    return {static_cast<int>(width), static_cast<int>(height)};
}

HRESULT ClipPreviewEngine::GetLastErrorCode() const
{
    return lastError_;
}
