#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace utils
{
constexpr int radix = 10;

bool StartsWithOne(int num)
{
	while (num >= 10)
	{
		num /= radix;
	}

	return num == 1;
}

int NumDigits(int num)
{
	int digs = 0;

	if (num == 0)
	{
		return 1;
	}

	while (num != 0)
	{
		num /= radix;
		++digs;
	}

	return digs;
}

int DigMult(int num)
{
	int res = 1;

	if (num == 0)
	{
		return 0;
	}

	while (num != 0)
	{
		int dig = num % radix;
		res *= dig;
		num /= radix;
	}

	return res;
}
}

// Must match the name used by PipeReader (Process 2).
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\NumberClassifier";

// Client end of the named pipe. Each Send() call is written as one pipe message.
class PipeWriter
{
public:
	PipeWriter()
	{
		m_pipe = ::CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

		if (m_pipe == INVALID_HANDLE_VALUE)
		{
			DWORD err = ::GetLastError();

			if (err == ERROR_FILE_NOT_FOUND)
			{
				throw std::runtime_error("pipe not found - start PipeReader first");
			}

			throw std::runtime_error("CreateFileW failed, error " + std::to_string(err));
		}
	}

	~PipeWriter()
	{
		::FlushFileBuffers(m_pipe);
		::CloseHandle(m_pipe);
	}

	PipeWriter(const PipeWriter&) = delete;
	PipeWriter& operator=(const PipeWriter&) = delete;

	void Send(const std::string& msg)
	{
		DWORD written = 0;

		if (!::WriteFile(m_pipe, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr) ||
			written != msg.size())
		{
			throw std::runtime_error("WriteFile failed, error " + std::to_string(::GetLastError()));
		}
	}

private:
	HANDLE m_pipe = INVALID_HANDLE_VALUE;
};

class RandomManager
{
public:

	static constexpr int kRandomMin = 1;
	static constexpr int kRandomMax = 999;

	explicit RandomManager(PipeWriter& out) : m_out(out) {}

	int GenerateOne()
	{
		return m_dist(m_engine);
	}

	void Run(int amount)
	{
		std::ostringstream line;
		line << "X = " << amount;
		m_out.Send(line.str());

		for (int i = 0; i < amount; ++i)
		{
			int curr = GenerateOne();

			WorkOnClassifier(curr);

		}
	}

	void WorkOnClassifier(int num)
	{
		std::ostringstream line;

		if (num % 2 == 1)
		{
			line << num << ": this is an odd number";
			m_out.Send(line.str());
			return;
		}

		if (utils::StartsWithOne(num))
		{
			StartsWithOneProc(num);
			return;
		}

		if (utils::NumDigits(num) == 3)
		{
			line << num << ": the digit's multiplications are equal to " << utils::DigMult(num);
			m_out.Send(line.str());
			return;
		}

		line << num << ": this number belongs to the others";
		m_out.Send(line.str());
	}

	void StartsWithOneProc(int amountToGen)
	{
		std::ostringstream line;

		auto printVec = [&](const std::vector<int>& vec)
		{
			for (auto const x : vec)
			{
				line << x << " ";
			}
		};

		std::vector<int> gen;
		gen.reserve(amountToGen);

		for (int i = 0; i < amountToGen; ++i) {
			gen.push_back(GenerateOne());
		}

		line << amountToGen << ": starts with 1 [origin list: ";
		printVec(gen);

		std::sort(gen.begin(), gen.end());
		line << "Ordered list: ";
		printVec(gen);
		line << "]";

		m_out.Send(line.str());
	}


private:
	PipeWriter& m_out;
	std::random_device m_r{};
	std::default_random_engine m_engine{m_r()};
	std::uniform_int_distribution<int> m_dist{kRandomMin, kRandomMax};
};

int main()
{
	try
	{
		PipeWriter pipe{};
		RandomManager r{pipe};

		int x = r.GenerateOne();
		r.Run(x);
	}
	catch (const std::exception& e)
	{
		std::cerr << "NumberClassifier: " << e.what() << "\n";
		return 1;
	}

	return 0;
}
