//
// Experimental / WIP: a from-scratch replacement for the game's WinMain, plus a resource-dump
// entry, injected through the StartExtermination export. This is NOT on the current patch path
// (the live DLL surface is DllMain + the working thunks in mh.c) -- it's kept here, out of mh.c,
// so the DLL's real surface stays readable. WinMainCopy's own caller is still commented out; this
// file exists to preserve the experiment, not to run it.
//
#include <windows.h>
#include "addresses.h"
#include "mh.h"

// We create this in our code and assign it back to extermination memory
static HINSTANCE main_hInstance;
static HWND main_hWndParent;

static int ShowMessageBox(const char *msg, const char *title)
{
	return MessageBoxA(NULL, msg, title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
}

static int DisplaySplashScreen(void)
{
	WNDCLASSA wnd_class;
	wnd_class.style = 40;
	wnd_class.lpfnWndProc = (WNDPROC)EXTERM_FUNC_UNK_4A6E26;
	wnd_class.hInstance = main_hInstance;
	wnd_class.hIcon = 0;
	wnd_class.hCursor = LoadCursorA(0, (LPCSTR)0x7F00);  // IDC_ARROW
	wnd_class.hbrBackground = 0;
	wnd_class.lpszMenuName = 0;
	wnd_class.lpszClassName = MH_ADDRESS(EXTERM_SPLASH_CLASS_NAME, char);
	RegisterClassA(&wnd_class);
	HWND hWnd = CreateWindowExA(0,
		MH_ADDRESS(EXTERM_SPLASH_CLASS_NAME, char),
		MH_ADDRESS(EXTERM_WINDOW_NAME, char),
		WS_POPUP | WS_BORDER | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, 128, 64,
		0, 0, main_hInstance, 0);
	ShowWindow(hWnd, 1);
	ShowWindow(hWnd, 5);
	UpdateWindow(hWnd);
	Sleep(500);
	return DestroyWindow(hWnd);
}

static int CreateMainWindow(HINSTANCE hInstance)
{
	WNDCLASSA wnd_class;

	wnd_class.style = 32;
	// Assign the game's main WndProc to our window
	wnd_class.lpfnWndProc = (WNDPROC)EXTERM_FUNC_WND_PROC;
	wnd_class.cbClsExtra = 0;
	wnd_class.cbWndExtra = 0;
	wnd_class.hInstance = hInstance;
	wnd_class.hIcon = 0;
	wnd_class.hCursor = 0;
	wnd_class.hbrBackground = 0;
	wnd_class.lpszMenuName = 0;
	wnd_class.lpszClassName = MH_ADDRESS(EXTERM_SPLASH_CLASS_NAME, char);
	if (RegisterClassA(&wnd_class))
	{
		main_hWndParent = CreateWindowExA(8u,
			MH_ADDRESS(EXTERM_SPLASH_CLASS_NAME, char),
			MH_ADDRESS(EXTERM_WINDOW_NAME, char),
			0x16000000u,
			0, 0,
			MH_GLOBAL(EXTERM_WINDOW_NWIDTH, int),
			MH_GLOBAL(EXTERM_WINDOW_NHEIGHT, int),
			0, 0, hInstance, 0);
		MH_GLOBAL(EXTERM_HWND_MAIN, HWND) = main_hWndParent;
	}

	if (main_hWndParent == 0)
		return 1;
	return 0;
}

static int WinMainCopy(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	main_hInstance = hInstance;
	char *mutex_name = MH_ADDRESS(EXTERM_MUTEX_NAME, char);
	HANDLE mutexHnd = CreateMutexA(0, 1, mutex_name);
	MSG Msg;
	if (mutexHnd) {
		int unk1_ret = MH_CALL_FUNC_INT_INT(EXTERM_FUNC_UNK_410012, 1);
		MH_GLOBAL(EXTERM_HINSTANCE, HINSTANCE) = hInstance;
		unk1_ret = MH_CALL_FUNC_INT_INT(EXTERM_FUNC_UNK_4A6D3B, (int)hInstance);
		unk1_ret = MH_CALL_FUNC_INT_CHAR(EXTERM_FUNC_UNK_45C69B, (char *)EXTERM_UNK_54F828);
		// Un-needed, DisplaySplashScreen does more or less the same thing
		// EXTERM_CALL_FUNC_INT_INT(EXTERM_FUNC_UNK_4A6F9E, unk1_ret);
		DisplaySplashScreen();
		MH_GLOBAL(EXTERM_WINDOW_OPEN, int) = 1;

		if (!CreateMainWindow(hInstance))
		{
			ShowWindow(main_hWndParent, nCmdShow);
			UpdateWindow(main_hWndParent);
			SetCursor(0);
			while (1)
			{
				while (PeekMessageA(&Msg, 0, 0, 0, 1u))
				{
					if (Msg.message == WM_QUIT)
					{
						ReleaseMutex(mutexHnd);
						//CloseHandle(hHandle);
						ExitProcess(Msg.wParam);
					}
					TranslateMessage(&Msg);
					DispatchMessageA(&Msg);
				}
				if (MH_GLOBAL(EXTERM_WINDOW_OPEN, int))
					RedrawWindow(main_hWndParent, 0, 0, 0x23u);
				WaitMessage();
			}
		}
	}
	return 0;
}

__declspec(dllexport) int StartExtermination(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	// If we're only using this to dump the files then exit afterwards
	int msgBoxId = ShowMessageBox("Do you want to dump all files from exterm.rsr to disk?", "Dump?");
	if (msgBoxId == IDYES)
	{
		// NOTE: DecompressFile() is declared nowhere in this project (formats/decompress.cpp
		// defines a Decoder class, not this C entry point) -- the dump path has never linked in
		// Release. Stubbed out to unblock the Release build; wire in a real DecompressFile (or
		// call MH_LZW_Decompress on a file buffer) when the dump feature is actually needed.
		/*
		DecompressFile(
			"F:\\games\\MH\\res_unpack\\mh_\\init\\INIT.CFG",
			"F:\\games\\MH\\res_unpack\\mh_\\init\\INIT.CFG_dec",
			"F:\\games\\MH\\res_unpack\\mh_\\init\\INIT.CFG_dec_dict"
		);
		*/
	  // The game reads the .nam and then the .rsr, no need for an extension
		/*DumpRsrFile("extermin");
		DumpRsrFile("mh");
		DumpRsrFile("mh_ex");
		DumpRsrFile("Extermex");*/
		return 0;
	}
	else {
		// Run our copy of the WinMain
		//WinMainCopy(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
	}

	(void)WinMainCopy;  // silence "unused" while the caller above is commented out
	return 0;
}
