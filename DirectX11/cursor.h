#pragma once

#include <windows.h>

// These functions will bypass our hooks *if* the option to do so has been enabled:
BOOL WINAPI CursorUpscalingBypass_GetClientRect(_In_ HWND hWnd, _Out_ LPRECT lpRect);
BOOL WINAPI CursorUpscalingBypass_GetCursorInfo(_Inout_ PCURSORINFO pci);
BOOL WINAPI CursorUpscalingBypass_ScreenToClient(_In_ HWND hWnd, LPPOINT lpPoint);

void InstallMouseHooks(bool hide);

// Set and read the cursor past the hide_cursor hooks. The cursor the game set
// stays what the hooks report to the game.
HCURSOR CursorBypass_SetCursor(HCURSOR cursor);
BOOL CursorBypass_GetCursorInfo(PCURSORINFO pci);
// NULL or the cursor hide_cursor shows in place of the game's
bool CursorIsHidden(HCURSOR cursor);
// Sets the cursor the game set last, under hide_cursor, after
// CursorBypass_SetCursor. False without the SetCursor hook, which is the only
// record of the game's cursor: the caller restores the cursor it replaced.
bool CursorRestoreGame();
