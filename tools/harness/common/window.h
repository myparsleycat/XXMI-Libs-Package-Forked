#pragma once

#include <windows.h>

static void show_harness_window(HWND window, bool required = false)
{
	char visible[2] = {};
	if (required || (GetEnvironmentVariableA("XH_VISIBLE", visible, sizeof(visible)) == 1 && visible[0] == '1'))
		ShowWindow(window, SW_SHOWNOACTIVATE);
}
