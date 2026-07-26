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
    // containerWindow is the full preview area (letterbox bars). Video is rendered
    // into a child HWND that we size to preserve aspect ratio.
    ClipPreviewEngine(HWND eventWindow, HWND containerWindow);
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
    // Fit the video into the container HWND with correct aspect ratio.
    HRESULT UpdatePlaybackWindow();
    void NotifyEvent(DWORD eventCode);

    bool IsReady() const;
    bool IsPlaying() const;
    int DurationMilliseconds() const;
    int PositionMilliseconds() const;
    std::pair<int, int> NativeVideoSize() const;
    HRESULT GetLastErrorCode() const;

    // Window class for the letterbox host / playback child. Uses a dark
    // background brush so resize erase does not flash the default STATIC white.
    static const wchar_t* VideoHostWindowClass();

private:
    class NotifyCallback;

    HRESULT CreateEngine();
    void ReleaseEngine();
    void DestroyPlaybackWindow();

    HWND eventWindow_ = nullptr;
    HWND containerWindow_ = nullptr;
    HWND playbackWindow_ = nullptr;
    IMFMediaEngineClassFactory* factory_ = nullptr;
    IMFMediaEngine* engine_ = nullptr;
    IMFMediaEngineEx* engineEx_ = nullptr;
    NotifyCallback* notify_ = nullptr;
    bool initialized_ = false;
    bool ready_ = false;
    HRESULT lastError_ = S_OK;
    int cachedVideoWidth_ = 0;
    int cachedVideoHeight_ = 0;
};
