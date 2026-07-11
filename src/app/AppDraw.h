#pragma once

#include "app/AppContext.h"

extern VisualTheme gTheme;

void EnsureThemeResources();
void DestroyThemeResources();
void DestroyParticipantSpecIcons(AppContext* ctx);
void EnsureParticipantSpecIconList(AppContext* ctx);
int ResolveParticipantSpecIconIndex(
    AppContext* ctx,
    const std::optional<std::string>& className,
    const std::optional<std::string>& specName);
void ApplyUiFonts(HWND root);
void ApplyRecordingsFonts(AppContext* ctx);

bool IsStyledButtonId(int controlId);
bool IsStatusLightId(int controlId);
bool IsOwnerDrawStaticId(int controlId);
void ConfigureStyledButtons(AppContext* ctx);
void DrawStyledButton(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
void DrawStatusLight(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
void DrawLengthValue(const DRAWITEMSTRUCT* drawInfo);
void DrawHelpIcon(const DRAWITEMSTRUCT* drawInfo);
void DrawConfigurationTooltip(const DRAWITEMSTRUCT* drawInfo);
void HideConfigurationTooltip(AppContext* ctx);
void ConfigureConfigurationTooltips(AppContext* ctx);
void DrawRecordingsGridLines(const NMLVCUSTOMDRAW* customDraw, const AppContext* ctx);
LRESULT CALLBACK RecordingsHeaderSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData);
void DrawYouTubeLinkStatus(const DRAWITEMSTRUCT* drawInfo, const AppContext* ctx);
LRESULT CALLBACK HoverTooltipSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
