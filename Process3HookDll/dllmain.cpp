// Process 3 hook DLL.
//
// Injected into Process 2 (PipeReader) while it is already running. It installs
// an Import Address Table hook on ReadFile. Every ReadFile that returns data
// from a pipe handle has those bytes copied to the tap pipe (which Process 3's
// injector reads and prints). The real data is left untouched in Process 2's
// buffer, so Process 2 keeps working normally - Process 3 only steals a copy of
// what Process 2 receives.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

// Original ReadFile taken from the exe's IAT (the genuine kernel32 entry).
// Called directly below, so these calls do not re-enter the hook.
static ReadFile_t g_realReadFile = nullptr;
static HANDLE     g_tap = INVALID_HANDLE_VALUE;

// Must match the tap pipe name used by the Process3 injector.
constexpr wchar_t kTapName[] = L"\\\\.\\pipe\\NumberClassifierTap";

static void EnsureTap()
{
	if (g_tap != INVALID_HANDLE_VALUE)
	{
		return;
	}

	HANDLE h = ::CreateFileW(kTapName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD mode = PIPE_READMODE_MESSAGE;
		::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
		g_tap = h;
	}
}

static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov)
{
	// Let Process 2 receive its data first, then copy whatever it got.
	BOOL ok = g_realReadFile(hFile, buf, n, read, ov);

	DWORD got = (read != nullptr) ? *read : 0;

	if (got > 0 && ::GetFileType(hFile) == FILE_TYPE_PIPE)
	{
		EnsureTap();

		if (g_tap != INVALID_HANDLE_VALUE)
		{
			DWORD w = 0;

			// WriteFile is not hooked, so this does not re-enter the hook.
			if (!::WriteFile(g_tap, buf, got, &w, nullptr))
			{
				::CloseHandle(g_tap);
				g_tap = INVALID_HANDLE_VALUE;
			}
		}
	}

	return ok;
}

// Patch the ReadFile slot in the main module's Import Address Table.
static bool InstallHook()
{
	auto base = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

	const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

	if (imports.VirtualAddress == 0)
	{
		return false;
	}

	auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);

	for (; desc->Name != 0; ++desc)
	{
		// Names come from OriginalFirstThunk; the writable IAT is FirstThunk.
		DWORD namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
		auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + namesRva);
		auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

		for (; names->u1.AddressOfData != 0; ++names, ++iat)
		{
			if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)
			{
				continue; // imported by ordinal, no name to match
			}

			auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);

			if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "ReadFile") == 0)
			{
				auto slot = reinterpret_cast<void**>(&iat->u1.Function);

				DWORD oldProtect = 0;
				::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect);
				g_realReadFile = reinterpret_cast<ReadFile_t>(*slot);
				*slot = reinterpret_cast<void*>(&HookedReadFile);
				::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

				return true;
			}
		}
	}

	return false;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		::DisableThreadLibraryCalls(self);
		InstallHook();
		break;

	case DLL_PROCESS_DETACH:
		if (g_tap != INVALID_HANDLE_VALUE)
		{
			::FlushFileBuffers(g_tap);
			::CloseHandle(g_tap);
			g_tap = INVALID_HANDLE_VALUE;
		}
		break;
	}

	return TRUE;
}
