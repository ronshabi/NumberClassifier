// Process 3 injector (a bystander).
//
// Process 3 launches nothing. It finds the already-running Process 2
// (PipeReader) by PID, injects the hook DLL into it, and then reads the tap
// pipe the hook feeds. The hook steals a copy of every message the moment
// Process 2 reads it off the pipe; Process 2 is undisturbed and still prints
// normally.
//
// Usage:
//   Process3.exe            - finds PipeReader.exe automatically
//   Process3.exe <pid>      - attaches to the given PID
//
// Run order: start Process 2 (PipeReader), then Process 3, then Process 1
// (NumberClassifier). Attach Process 3 before Process 1 begins writing, or the
// earliest messages may be read by Process 2 before the hook is in place.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tlhelp32.h>

#include <iostream>
#include <string>
#include <vector>

constexpr wchar_t kTapName[] = L"\\\\.\\pipe\\NumberClassifierTap";
constexpr wchar_t kTargetName[] = L"PipeReader.exe";
constexpr DWORD kBufSize = 4096;

static std::wstring OwnDir()
{
	wchar_t path[MAX_PATH];
	DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s(path, n);
	return s.substr(0, s.find_last_of(L"\\/"));
}

static DWORD FindPidByName(const wchar_t* name)
{
	DWORD pid = 0;

	HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (snap == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);

	if (::Process32FirstW(snap, &pe))
	{
		do
		{
			if (::_wcsicmp(pe.szExeFile, name) == 0)
			{
				pid = pe.th32ProcessID;
				break;
			}
		} while (::Process32NextW(snap, &pe));
	}

	::CloseHandle(snap);
	return pid;
}

static bool InjectDll(HANDLE process, const std::wstring& dllPath)
{
	SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);

	void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (remote == nullptr)
	{
		return false;
	}

	bool ok = false;

	if (::WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr))
	{
		HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
		auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(k32, "LoadLibraryW"));

		HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);

		if (thread != nullptr)
		{
			::WaitForSingleObject(thread, INFINITE);

			// LoadLibraryW's return (the module base) is truncated into the
			// thread exit code; nonzero means the DLL loaded.
			DWORD exitCode = 0;
			::GetExitCodeThread(thread, &exitCode);
			ok = (exitCode != 0);
			::CloseHandle(thread);
		}
	}

	::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
	return ok;
}

int main(int argc, char** argv)
{
	std::wstring dllPath = OwnDir() + L"\\Process3HookDll.dll";

	if (::GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		std::wcerr << L"Process3: hook DLL not found next to Process3.exe: " << dllPath << L"\n";
		return 1;
	}

	// 1. Resolve Process 2's PID (from the argument, or by image name).
	DWORD pid = 0;

	if (argc > 1)
	{
		pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));

		if (pid == 0)
		{
			std::cerr << "Process3: invalid PID argument '" << argv[1] << "'\n";
			return 1;
		}
	}
	else
	{
		pid = FindPidByName(kTargetName);

		if (pid == 0)
		{
			std::cerr << "Process3: PipeReader.exe not found - start Process 2 first, "
			             "or pass its PID as an argument.\n";
			return 1;
		}
	}

	std::cerr << "Process3: target Process 2 PID = " << pid << "\n";

	// 2. Create the tap pipe before the injected hook connects to it.
	HANDLE tap = ::CreateNamedPipeW(
		kTapName,
		PIPE_ACCESS_INBOUND,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 0, kBufSize, 0, nullptr);

	if (tap == INVALID_HANDLE_VALUE)
	{
		std::cerr << "Process3: CreateNamedPipe failed, error " << ::GetLastError() << "\n";
		return 1;
	}

	// 3. Open Process 2 and inject the hook DLL. No process is created.
	HANDLE target = ::OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
		PROCESS_VM_WRITE | PROCESS_VM_READ,
		FALSE, pid);

	if (target == nullptr)
	{
		std::cerr << "Process3: OpenProcess(" << pid << ") failed, error " << ::GetLastError() << "\n";
		::CloseHandle(tap);
		return 1;
	}

	if (!InjectDll(target, dllPath))
	{
		std::cerr << "Process3: DLL injection failed, error " << ::GetLastError() << "\n";
		::CloseHandle(target);
		::CloseHandle(tap);
		return 1;
	}

	std::cerr << "Process3: hook installed in Process 2, listening...\n";

	// 4. Read stolen messages until the hook closes the tap (Process 2 exits).
	if (!::ConnectNamedPipe(tap, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED)
	{
		std::cerr << "Process3: ConnectNamedPipe failed, error " << ::GetLastError() << "\n";
	}

	std::vector<char> buf(kBufSize);
	std::string msg;

	for (;;)
	{
		DWORD read = 0;
		BOOL ok = ::ReadFile(tap, buf.data(), kBufSize, &read, nullptr);
		DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();

		if (!ok && err != ERROR_MORE_DATA)
		{
			break; // ERROR_BROKEN_PIPE on normal exit
		}

		msg.append(buf.data(), read);

		if (ok)
		{
			std::cout << "[P3 stolen] " << msg << "\n";
			msg.clear();
		}
	}

	::WaitForSingleObject(target, 5000);
	std::cerr << "Process3: tap closed (Process 2 stopped reading).\n";

	::DisconnectNamedPipe(tap);
	::CloseHandle(tap);
	::CloseHandle(target);
	return 0;
}
