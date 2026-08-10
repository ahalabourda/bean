#pragma once

#include "app/AppContext.h"

extern VisualTheme gTheme;

enum class BeanFileListKind {
    Recordings,
    YouTube
};

void EnsureThemeResources();
void RebuildThemeColorResources();
void DestroyThemeResources();
void DestroyParticipantSpecIcons(AppContext* ctx);
void EnsureParticipantSpecIconList(AppContext* ctx);
int ResolveParticipantSpecIconIndex(
    AppContext* ctx,
    const std::optional<std::string>& className,
    const std::optional<std::string>& specName);
void ApplyUiFonts(HWND root);
void ApplyRecordingsFonts(AppContext* ctx);
HWND CreateBeanFileList(HWND parent, int controlId, AppContext* ctx, BeanFileListKind kind);
void RefreshBeanFileList(HWND list);
int GetBeanFileListSelectedIndex(HWND list);

bool IsStyledButtonId(int controlId);
bool IsStyledComboId(int controlId);
bool IsStatusLightId(int controlId);
bool IsOwnerDrawStaticId(int controlId);
void ConfigureStyledButtons(AppContext* ctx);
void ConfigureModernControls(AppContext* ctx);
void ScheduleModernComboRedraw(HWND combo);
void DrawStyledButton(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
void DrawStyledComboItem(const DRAWITEMSTRUCT* drawInfo);
void DrawStatusLight(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
void DrawLengthValue(const DRAWITEMSTRUCT* drawInfo);
void DrawHelpIcon(const DRAWITEMSTRUCT* drawInfo);
void DrawConfigurationTooltip(const DRAWITEMSTRUCT* drawInfo);
void HideConfigurationTooltip(AppContext* ctx);
void ConfigureConfigurationTooltips(AppContext* ctx);
void DrawYouTubeLinkStatus(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
void DrawYouTubeUploadStatus(const DRAWITEMSTRUCT* drawInfo, AppContext* ctx);
LRESULT CALLBACK HoverTooltipSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
