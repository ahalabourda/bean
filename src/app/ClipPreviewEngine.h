#pragma once

#include <windows.h>

#include <filesystem>
#include <utility>

struct IMFMediaEngine;
struct IMFMediaEngineEx;
struct IMFMediaEngineClassFactory;
struct IMFAttributes;
struct IMFMediaError;

class ClipPreviewEngine {
public:
    ClipPreviewEngine(HWND eventWindow, HWND playbackWindow);
    ~ClipPreviewEngine();

    ClipPreviewEngine(const ClipPreviewEngine&) = delete;
    ClipPreviewEngine& operator=(const ClipPreviewEngine&) = delete;

    HRESULT Initialize();
    HRESULT Open(const std::filesystem::path& path);
    void Close();

    HRESULT Play();
    HRESULT Pause();
    HRESULT SeekMilliseconds(int milliseconds);
    HRESULT SetVolumePercent(int percent);
    void NotifyEvent(DWORD eventCode);

    bool IsReady() const;
    bool IsPlaying() const;
    int DurationMilliseconds() const;
    int PositionMilliseconds() const;
    std::pair<int, int> NativeVideoSize() const;
    HRESULT GetLastErrorCode() const;

private:
    class NotifyCallback;

    HRESULT CreateEngine();
    void ReleaseEngine();

    HWND eventWindow_ = nullptr;
    HWND playbackWindow_ = nullptr;
    IMFMediaEngineClassFactory* factory_ = nullptr;
    IMFMediaEngine* engine_ = nullptr;
    IMFMediaEngineEx* engineEx_ = nullptr;
    NotifyCallback* notify_ = nullptr;
    bool initialized_ = false;
    bool ready_ = false;
    HRESULT lastError_ = S_OK;
};
