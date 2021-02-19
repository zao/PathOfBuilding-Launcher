#include <cstdint>
#include <vector>
#include <string>
#include <sstream>
#include <windows.h>
#include <ShlObj.h>
#include "SafeHandle.h"
#include "StringUtils.h"

std::vector<std::wstring> ParseCommandLine()
{
	std::vector<std::wstring> commandLine;
	int dwNumArgs = 0;
	LPWSTR *wszCommandLineParams = CommandLineToArgvW(GetCommandLineW(), &dwNumArgs);
	if (wszCommandLineParams != nullptr)
	{
		commandLine.reserve(dwNumArgs);
		for (int dwArg = 0; dwArg < dwNumArgs; dwArg++)
		{
			commandLine.push_back(wszCommandLineParams[dwArg]);
		}
		LocalFree(wszCommandLineParams);
	}
	return commandLine;
}

bool IsValidLuaFile(const std::wstring &path, std::string &firstLine)
{
	// Open the file (SafeHandle will close the file when exiting scope).
	SafeHandle hFile = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!hFile.IsValid())
	{
		return false;
	}

	// Read the first 255 bytes of the file, which should be enough to encompass the first line
	char szHeader[256]{};
	DWORD dwBytesRead = 0;
	if (!ReadFile(hFile.Get(), szHeader, sizeof(szHeader) - 1, &dwBytesRead, nullptr))
	{
		return false;
	}
	szHeader[dwBytesRead] = 0;
	hFile.Close();

	// Check for UTF8 BOM
	char *szHeaderCur = szHeader;
	if (szHeaderCur[0] == 0xEF && szHeaderCur[1] == 0xBB && szHeaderCur[2] == 0xBF)
	{
		szHeaderCur += 3;
	}

	// Launcher lua files must start with a #@ directive which determines the dll to launch.
	if (szHeaderCur[0] != '#' || szHeaderCur[1] != '@')
	{
		return false;
	}

	// Find the end of the line
	char *szNewLinePos = strchr(szHeaderCur, '\n');
	if (szNewLinePos == nullptr)
	{
		return false;
	}
	// Trim any whitespace at the end of the line.
	while (*szNewLinePos == '\n' || *szNewLinePos == '\r' || isspace(*szNewLinePos))
	{
		szNewLinePos--;
	}

	firstLine = std::string(szHeaderCur, szNewLinePos + 1);
	return true;
}

bool FindLaunchLua(std::wstring basePath, std::vector<std::wstring> &commandLine, std::string &firstLine)
{
	std::wstring launchPath = basePath + L"Launch.lua";
	if (IsValidLuaFile(launchPath, firstLine))
	{
		commandLine.insert(commandLine.begin() + 1, launchPath);
		return true;
	}

	launchPath = basePath + L"src\\Launch.lua";
	if (IsValidLuaFile(launchPath, firstLine))
	{
		commandLine.insert(commandLine.begin() + 1, launchPath);
		return true;
	}

	return false;
}

bool InsertLaunchLua(std::vector<std::wstring> &commandLine, std::string &firstLine)
{
	// Determine if the first command-line parameter is the location of a valid launcher lua file
	if (commandLine.size() > 1 && IsValidLuaFile(commandLine[1], firstLine))
	{
		// Convert the path of the file to the long version
		if (commandLine[1].size() > 3 && iswupper(commandLine[1][0]) && commandLine[1][1] == ':' && commandLine[1][2] == '\\')
		{
			wchar_t wszLongPath[MAX_PATH]{};
			if (GetLongPathName(commandLine[1].c_str(), wszLongPath, MAX_PATH) != 0)
			{
				commandLine[1] = wszLongPath;
			}
		}
		return true;
	}

	// Search for the Launch.lua file in various locations it may exist

	// Look in the same directory as the executable as well as the "src" folder within that
	{
		wchar_t wszModuleFilename[MAX_PATH]{};
		if (GetModuleFileName(nullptr, wszModuleFilename, MAX_PATH) > 0)
		{
			wchar_t *wszLastSlash = wcsrchr(wszModuleFilename, '\\');
			if (wszLastSlash != nullptr)
			{
				std::wstring basePath(wszModuleFilename, wszLastSlash + 1);
				if (FindLaunchLua(basePath, commandLine, firstLine))
				{
					return true;
				}
			}
		}
	}

	// Check for the registry key left by the installer
	// HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Path of Building Community\InstallLocation
	{
		DWORD dwType = 0;
		DWORD dwSize = MAX_PATH;
		wchar_t wszValue[MAX_PATH]{};
		DWORD dwStatus = RegGetValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Path of Building Community", L"InstallLocation", RRF_RT_REG_SZ, &dwType, wszValue, &dwSize);
		if (dwStatus == ERROR_SUCCESS && dwSize > sizeof(wchar_t))
		{
			// Strip the quotes around the value
			std::wstring basePath = wszValue[0] == L'\"' ? std::wstring(wszValue + 1, wszValue + dwSize / sizeof(wchar_t) - 2) : std::wstring(wszValue, wszValue + dwSize / sizeof(wchar_t) - 1);
			if (FindLaunchLua(basePath, commandLine, firstLine))
			{
				return true;
			}
		}
	}

	// Look in the %APPDATA% folder, which is where the PoB Fork installer puts the lua files
	{
		wchar_t wszAppDataPath[MAX_PATH]{};
		if (SHGetSpecialFolderPath(nullptr, wszAppDataPath, CSIDL_APPDATA, false))
		{
			std::wstring basePath(wszAppDataPath);
			basePath += L"\\Path of Building Community\\";
			if (FindLaunchLua(basePath, commandLine, firstLine))
			{
				return true;
			}
		}
	}

	// Look in the %PROGRAMDATA% folder, which is where the original PoB installer puts the lua files
	{
		wchar_t wszAppDataPath[MAX_PATH]{};
		if (SHGetSpecialFolderPath(nullptr, wszAppDataPath, CSIDL_COMMON_APPDATA, false))
		{
			std::wstring basePath(wszAppDataPath);
			basePath += L"\\Path of Building\\";
			if (FindLaunchLua(basePath, commandLine, firstLine))
			{
				return true;
			}
		}
	}

	return false;
}

std::vector<std::string> ConvertToUTF8(std::vector<std::wstring> commandLine)
{
	std::vector<std::string> commandLineUTF8;
	if (commandLine.size() > 0)
	{
		commandLineUTF8.reserve(commandLine.size());
		for (const std::wstring &param : commandLine)
		{
			int dwUTF8Size = WideCharToMultiByte(CP_UTF8, 0, param.c_str(), (int)param.size(), NULL, 0, NULL, NULL);
			std::string paramUTF8(dwUTF8Size, 0);
			WideCharToMultiByte(CP_UTF8, 0, param.c_str(), (int)param.size(), paramUTF8.data(), dwUTF8Size, NULL, NULL);
			commandLineUTF8.emplace_back(std::move(paramUTF8));
		}
	}
	return commandLineUTF8;
}

void InitConsole()
{
	static bool initialized = false;
	if (initialized)
	{
		return;
	}

	AllocConsole();
	FILE *fDummy = nullptr;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	initialized = true;
}

// Entry function
int CALLBACK wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR lpCmdLine, _In_ int nCmdShow)
{
	// Store all the commandline parameters into a vector for easy manipulation
	std::vector<std::wstring> commandLine = ParseCommandLine();

	// Determine if the first parameter is a path to a valid lua file and insert one if it is not
	std::string firstLine;
	if (!InsertLaunchLua(commandLine, firstLine))
	{
		InitConsole();
		wprintf(L"ERROR: Could not find a valid launcher lua file.");
		return 1;
	}

	// Determine the DLL to load
	std::wstring dllName(firstLine.begin() + 2, firstLine.end());
	trim(dllName);
	if (wcsstr(dllName.c_str(), L".dll") == nullptr)
	{
		dllName += L".dll";
	}

	// Load the DLL
	HMODULE hDLL = LoadLibrary(dllName.c_str());
	if (hDLL == nullptr)
	{
		InitConsole();
		wprintf(L"ERROR: Could not find dll named '%s'\n", dllName.c_str());
		system("pause");
		return 1;
	}

	// Look for a valid entry point to the dll
	typedef int (*PFNRUNLUAFILEPROC)(int, char **);
	PFNRUNLUAFILEPROC RunLuaFile = (PFNRUNLUAFILEPROC)GetProcAddress(hDLL, "RunLuaFileAsWin");
	if (!RunLuaFile) {
		InitConsole();
		SetConsoleTitle(commandLine[1].c_str());
		RunLuaFile = (PFNRUNLUAFILEPROC)GetProcAddress(hDLL, "RunLuaFileAsConsole");
	}
	if (!RunLuaFile) {
		wprintf(L"ERROR: DLL '%s' does not appear to be a Path of Building dll.\n", dllName.c_str());
		FreeLibrary(hDLL);
		return 1;
	}

	// Create a utf8 version of the commandline parameters
	std::vector<std::string> commandLineUTF8 = ConvertToUTF8(commandLine);

	// Remove the first commandline argument as the scripts don't care about that.
	commandLineUTF8.erase(commandLineUTF8.begin());

	// Convert the commandline parameters to a form the DLL can understand
	size_t dwTotalParamSize = 0;
	for (const std::string &param : commandLineUTF8)
	{
		dwTotalParamSize += param.size() + 1;
	}
	size_t dwNumParams = commandLineUTF8.size();
	std::unique_ptr<char[]> pParamBuf = std::make_unique<char[]>(dwTotalParamSize);
	char *pCurParamBufLoc = pParamBuf.get();

	std::unique_ptr<char *[]> ppParamList = std::make_unique<char *[]>(dwNumParams);
	for (size_t i = 0; i < dwNumParams; i++)
	{
		ppParamList[i] = pCurParamBufLoc;

		const std::string &param = commandLineUTF8[i];
		memcpy(pCurParamBufLoc, param.c_str(), param.size() + 1);
		pCurParamBufLoc += param.size() + 1;
	}

	// Call into the DLL
	int dwStatus = RunLuaFile(dwNumParams, ppParamList.get());

	// Cleanup the DLL
	FreeLibrary(hDLL);

	return dwStatus;
}
