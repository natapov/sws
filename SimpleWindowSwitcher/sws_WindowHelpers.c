#include "sws_WindowHelpers.h"

NtUserBuildHwndList _sws_pNtUserBuildHwndList;
pCreateWindowInBand _sws_CreateWindowInBand;
pSetWindowCompositionAttribute _sws_SetWindowCompositionAttribute;
pGetWindowBand _sws_GetWindowBand;

void sws_WindowHelpers_SetWindowBlur(HWND hWnd, int type, DWORD Color, DWORD Opacity)
{
	ACCENTPOLICY policy;
	policy.nAccentState = type;
	policy.nFlags = 0;
	policy.nColor = (Opacity << 24) | (Color & 0xFFFFFF); // ACCENT_ENABLE_BLURBEHIND=3... // Color = 0XB32E9A
	policy.nFlags = 0;
	WINCOMPATTRDATA data = { 19, &policy, sizeof(ACCENTPOLICY) }; // WCA_ACCENT_POLICY=19
	_sws_SetWindowCompositionAttribute(hWnd, &data);
}

HWND* _sws_WindowHelpers_Gui_BuildWindowList
(
	NtUserBuildHwndList pNtUserBuildHwndList,
	HDESK in_hDesk,
	HWND  in_hWnd,
	BOOL  in_EnumChildren,
	BOOL  in_RemoveImmersive,
	UINT  in_ThreadID,
	INT* out_Cnt
)
{
	/* locals */
	UINT  lv_Max;
	UINT  lv_Cnt;
	UINT  lv_NtStatus;
	HWND* lv_List;

	// initial size of list
	lv_Max = 512;

	// retry to get list
	for (;;)
	{
		// allocate list
		if ((lv_List = (HWND*)malloc(lv_Max * sizeof(HWND))) == NULL)
			break;

		// call the api
		lv_NtStatus = pNtUserBuildHwndList(
			in_hDesk, in_hWnd,
			in_EnumChildren, in_RemoveImmersive, in_ThreadID,
			lv_Max, lv_List, &lv_Cnt);

		// success?
		if (lv_NtStatus == NOERROR)
			break;

		// free allocated list
		free(lv_List);

		// clear
		lv_List = NULL;

		// other error then buffersize? or no increase in size?
		if (lv_NtStatus != STATUS_BUFFER_TOO_SMALL || lv_Cnt <= lv_Max)
			break;

		// update max plus some extra to take changes in number of windows into account
		lv_Max = lv_Cnt + 16;
	}

	// return the count
	*out_Cnt = lv_Cnt;

	// return the list, or NULL when failed
	return lv_List;
}


/********************************************************/
/* enumerate all top level windows including metro apps */
/********************************************************/
sws_error_t sws_WindowHelpers_RealEnumWindows(
	WNDENUMPROC in_Proc,
	LPARAM in_Param
)
{
	if (!_sws_pNtUserBuildHwndList)
	{
		return sws_error_Report(SWS_ERROR_NOT_INITIALIZED);
	}

	/* locals */
	INT   lv_Cnt;
	HWND  lv_hWnd;
	BOOL  lv_Result;
	HWND  lv_hFirstWnd;
	HWND  lv_hDeskWnd;
	HWND* lv_List;

	// no error yet
	lv_Result = TRUE;

	// first try api to get full window list including immersive/metro apps
	lv_List = _sws_WindowHelpers_Gui_BuildWindowList(_sws_pNtUserBuildHwndList, 0, 0, 0, 0, 0, &lv_Cnt);

	// success?
	if (lv_List)
	{
		// loop through list
		while (lv_Cnt-- > 0 && lv_Result)
		{
			// get handle
			lv_hWnd = lv_List[lv_Cnt];

			// filter out the invalid entry (0x00000001) then call the callback
			if (IsWindow(lv_hWnd))
				lv_Result = in_Proc(lv_hWnd, in_Param);
		}

		// free the list
		free(lv_List);
	}
	else
	{
		// get desktop window, this is equivalent to specifying NULL as hwndParent
		lv_hDeskWnd = GetDesktopWindow();

		// fallback to using FindWindowEx, get first top-level window
		lv_hFirstWnd = FindWindowEx(lv_hDeskWnd, 0, 0, 0);

		// init the enumeration
		lv_Cnt = 0;
		lv_hWnd = lv_hFirstWnd;

		// loop through windows found
		// - since 2012 the EnumWindows API in windows has a problem (on purpose by MS)
		//   that it does not return all windows (no metro apps, no start menu etc)
		// - luckally the FindWindowEx() still is clean and working
		while (lv_hWnd && lv_Result)
		{
			// call the callback
			lv_Result = in_Proc(lv_hWnd, in_Param);

			// get next window
			lv_hWnd = FindWindowEx(lv_hDeskWnd, lv_hWnd, 0, 0);

			// protect against changes in window hierachy during enumeration
			if (lv_hWnd == lv_hFirstWnd || lv_Cnt++ > 10000)
				break;
		}
	}

	// return the result
	//return lv_Result;
	return SWS_ERROR_SUCCESS;
}

BOOL sws_WindowHelpers_IsAltTabWindow(
	_In_ HWND hwnd
)
{
	TITLEBARINFO ti;
	HWND hwndTry, hwndWalk = NULL;

	wchar_t wszClassName[100];
	GetClassNameW(hwnd, wszClassName, 100);
	if (!wcscmp(wszClassName, L"#32770"))
	{
		// ??? somwhow allow Explorer dialog boxes
		// but restrict Notepad ones...
	}

	if (!IsWindowVisible(hwnd))
		return FALSE;

	hwndTry = GetAncestor(hwnd, GA_ROOTOWNER);
	while (hwndTry != hwndWalk)
	{
		hwndWalk = hwndTry;
		hwndTry = GetLastActivePopup(hwndWalk);
		if (IsWindowVisible(hwndTry))
			break;
	}
	if (hwndWalk != hwnd)
		return FALSE;

	// the following removes some task tray programs and "Program Manager"
	ti.cbSize = sizeof(ti);
	GetTitleBarInfo(hwnd, &ti);
	if (ti.rgstate[0] & STATE_SYSTEM_INVISIBLE)
		return FALSE;

	// Tool windows should not be displayed either, these do not appear in the
	// task bar.
	if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return FALSE;

	return TRUE;
}

HICON sws_WindowHelpers_GetIconFromHWND(HWND hWnd, BOOL* bOwnProcess, BOOL bIsDesktop)
{
	HICON hIcon = NULL;

	wchar_t wszPath[MAX_PATH];
	ZeroMemory(wszPath, MAX_PATH * sizeof(wchar_t));

	if (bIsDesktop)
	{
		GetSystemDirectory(wszPath, MAX_PATH);
		wcscat_s(wszPath, MAX_PATH, L"\\imageres.dll");
		return ExtractIconW(
			GetModuleHandle(NULL),
			wszPath,
			-110
		);
	}

	DWORD dwProcessId, dwSize = MAX_PATH;
	GetWindowThreadProcessId(hWnd, &dwProcessId);
	HANDLE hProcess;
	hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
	if (hProcess)
	{
		wchar_t exeName[MAX_PATH + 1];
		QueryFullProcessImageNameW(GetCurrentProcess(), 0, exeName, &dwSize);
		CharLowerW(exeName);
		//QueryFullProcessImageNameW(hProcess, 0, wszPath, dwSize);
		GetModuleFileNameExW((HMODULE)hProcess, NULL, wszPath, MAX_PATH);
		CharLowerW(wszPath);

		if (!wcscmp(exeName, wszPath))
		{
			*bOwnProcess = TRUE;
		}

#ifndef COMPILE_AS_LIBRARY
		if (wcsstr(wszPath, L"applicationframehost.exe"))
		{
			IPropertyStore* propStore;
			SHGetPropertyStoreForWindow(
				hWnd,
				&__uuidof_IPropertyStore,
				&propStore
			);
			if (propStore)
			{
				PROPERTYKEY pKey;
				pKey.fmtid = __uuidof_AppUserModelIdProperty;
				pKey.pid = 5;
				PROPVARIANT prop;
				propStore->lpVtbl->GetValue(propStore, &pKey, &prop);
				propStore->lpVtbl->Release(propStore);
				return sws_WindowHelpers_PackageInfo_GetForAumid(prop.bstrVal);
			}
		}
		else
		{
#endif
			SendMessageTimeoutW(hWnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 1000, &hIcon);
			if (!hIcon)
			{
				SendMessageTimeoutW(hWnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 1000, &hIcon);
			}
			if (!hIcon)
			{
#ifdef _WIN64
				GetClassLong(hWnd, -34);
#else
				GetClassLongPtr(hWnd, -34);
#endif
			}
			if (!hIcon)
			{
#ifdef _WIN64
				GetClassLong(hWnd, -14);
#else
				GetClassLongPtr(hWnd, -14);
#endif
			}
			if (!hIcon)
			{
				SendMessageTimeoutW(hWnd, WM_QUERYDRAGICON, 0, 0, 0, 1000, &hIcon);
			}
			if (!hIcon)
			{
				SHFILEINFOW shinfo;
				ZeroMemory(&shinfo, sizeof(SHFILEINFOW));
				SHGetFileInfoW(
					wszPath,
					FILE_ATTRIBUTE_NORMAL,
					&shinfo,
					sizeof(SHFILEINFOW),
					SHGFI_ICON
				);
				hIcon = shinfo.hIcon;
				CloseHandle(hProcess);
			}
#ifndef COMPILE_AS_LIBRARY
		}
#endif
	}

	return hIcon;
}

static BOOL CALLBACK _sws_WindowHelpers_GetWallpaperHWNDCallback(_In_ HWND hwnd, _Out_ LPARAM lParam)
{
	HWND* ret = (HWND*)lParam;

	HWND p = FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL);
	if (p)
	{
		HWND t = FindWindowExW(NULL, hwnd, L"WorkerW", NULL);
		if (t)
		{
			*ret = t;
		}
	}
	return TRUE;
}

HWND sws_WindowHelpers_GetWallpaperHWND(HMONITOR hMonitor)
{
	HWND progman = FindWindowW(L"Progman", NULL);
	if (progman)
	{
		SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
		EnumWindows(_sws_WindowHelpers_GetWallpaperHWNDCallback, &hMonitor);
	}
	return hMonitor;
}

void sws_WindowHelpers_Release()
{
	FreeLibrary(_sws_hWin32u);
	FreeLibrary(_sws_hUser32);
}

sws_error_t sws_WindowHelpers_Initialize()
{
	sws_error_t rv = SWS_ERROR_SUCCESS;

	if (!rv)
	{
		if (!_sws_pNtUserBuildHwndList)
		{
			_sws_hWin32u = LoadLibraryW(L"win32u.dll");
			if (!_sws_hWin32u)
			{
				rv = sws_error_Report(SWS_ERROR_LOADLIBRARY_FAILED);
			}
		}
	}
	if (!rv)
	{
		if (!_sws_pNtUserBuildHwndList)
		{
			_sws_pNtUserBuildHwndList = (NtUserBuildHwndList)GetProcAddress(_sws_hWin32u, "NtUserBuildHwndList");
			if (!_sws_pNtUserBuildHwndList)
			{
				rv = sws_error_Report(SWS_ERROR_FUNCTION_NOT_FOUND);
			}
		}
	}
	if (!rv)
	{
		if (!_sws_SetWindowCompositionAttribute)
		{
			_sws_hUser32 = LoadLibraryW(L"user32.dll");
			if (!_sws_hUser32)
			{
				rv = sws_error_Report(SWS_ERROR_LOADLIBRARY_FAILED);
			}
		}
	}
	if (!rv)
	{
		if (!_sws_SetWindowCompositionAttribute)
		{
			_sws_SetWindowCompositionAttribute = (pSetWindowCompositionAttribute)GetProcAddress(_sws_hUser32, "SetWindowCompositionAttribute");
			if (!_sws_pNtUserBuildHwndList)
			{
				rv = sws_error_Report(SWS_ERROR_FUNCTION_NOT_FOUND);
			}
		}
	}
	if (!rv)
	{
		if (!_sws_CreateWindowInBand)
		{
			_sws_CreateWindowInBand = (pCreateWindowInBand)GetProcAddress(_sws_hUser32, "CreateWindowInBand");
			if (!_sws_CreateWindowInBand)
			{
				rv = sws_error_Report(SWS_ERROR_FUNCTION_NOT_FOUND);
			}
		}
	}
	if (!rv)
	{
		if (!_sws_GetWindowBand)
		{
			_sws_GetWindowBand = (pCreateWindowInBand)GetProcAddress(_sws_hUser32, "GetWindowBand");
			if (!_sws_GetWindowBand)
			{
				rv = sws_error_Report(SWS_ERROR_FUNCTION_NOT_FOUND);
			}
		}
	}
	return rv;
}
