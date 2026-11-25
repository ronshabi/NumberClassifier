#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

// Must match the name used by NumberClassifier (Process 1).
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\NumberClassifier";
constexpr DWORD kBufSize = 4096;

static void PrintError(const char* what)
{
	std::cerr << "PipeReader: " << what << " failed, error " << ::GetLastError() << "\n";
}

int main()
{
	HANDLE pipe = ::CreateNamedPipeW(
		kPipeName,
		PIPE_ACCESS_INBOUND,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1,          // max instances
		0,          // out buffer size (unused, inbound only)
		kBufSize,   // in buffer size
		0,          // default timeout
		nullptr);

	if (pipe == INVALID_HANDLE_VALUE)
	{
		PrintError("CreateNamedPipeW");
		return 1;
	}

	std::cerr << "PipeReader: waiting for NumberClassifier to connect...\n";

	// ERROR_PIPE_CONNECTED means the client connected between Create and Connect - that is success.
	if (!::ConnectNamedPipe(pipe, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED)
	{
		PrintError("ConnectNamedPipe");
		::CloseHandle(pipe);
		return 1;
	}

	std::vector<char> buf(kBufSize);
	std::string msg;
	int rc = 0;

	for (;;)
	{
		DWORD read = 0;
		BOOL ok = ::ReadFile(pipe, buf.data(), kBufSize, &read, nullptr);
		DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();

		if (!ok && err != ERROR_MORE_DATA)
		{
			if (err != ERROR_BROKEN_PIPE) // ERROR_BROKEN_PIPE = writer closed its end, normal exit
			{
				std::cerr << "PipeReader: ReadFile failed, error " << err << "\n";
				rc = 1;
			}
			break;
		}

		msg.append(buf.data(), read);

		// ERROR_MORE_DATA: the message is larger than the buffer, keep reading the rest of it.
		if (ok)
		{
			std::cout << msg << "\n";
			msg.clear();
		}
	}

	::DisconnectNamedPipe(pipe);
	::CloseHandle(pipe);
	return rc;
}
