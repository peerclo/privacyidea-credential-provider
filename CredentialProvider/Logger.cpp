#include "Logger.h"
#include "Configuration.h"
#include <objbase.h>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace std;

Logger::Logger() {
	this->releaseLog = Configuration::Get().releaseLog;
}

void Logger::logS(string message, const char* file, int line, bool logInProduction)
{
#ifdef _DEBUG
	UNREFERENCED_PARAMETER(logInProduction);
#endif
	// Check if it should be logged first and to which file
	string outfilePath = logfilePathDebug;
#ifndef _DEBUG
	if (!logInProduction || !this->releaseLog)
	{
		return;
	}
	outfilePath = logfilePathProduction;
#endif // !_DEBUG

	// Format: [Time] [file:line]  message
	time_t rawtime;
	struct tm* timeinfo = (tm*)CoTaskMemAlloc(sizeof(tm));
	char buffer[80];
	time(&rawtime);
	localtime_s(timeinfo, &rawtime);
	strftime(buffer, sizeof(buffer), "%d-%m-%Y %I:%M:%S", timeinfo);
	CoTaskMemFree(timeinfo);
	string fullMessage = "[" + string(buffer) + "] [" + string(file) + ":" + to_string(line) + "] " + message;
	
	ofstream os;
	os.open(outfilePath.c_str(), std::ios_base::app); // Append mode
	os << fullMessage << std::endl;

	OutputDebugStringA(fullMessage.c_str());
	OutputDebugStringA("\n");
}

void Logger::logW(wstring message, const char* file, int line, bool logInProduction)
{
	using convert_typeX = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_typeX, wchar_t> converterX;

	string conv = converterX.to_bytes(message);
	logS(conv, file, line, logInProduction);
}

void Logger::log(const char* message, const char* file, int line, bool logInProduction)
{
	string msg = "";
	if (message != NULL && message[0] != NULL) {
		msg = string(message);
	}
	logS(msg, file, line, logInProduction);
}

void Logger::log(const wchar_t* message, const char* file, int line, bool logInProduction)
{
	wstring msg = L"";
	if (message != NULL && message[0] != NULL) {
		msg = wstring(message);
	}
	logW(msg, file, line, logInProduction);
}

void Logger::log(int message, const char* file, int line, bool logInProduction)
{
	string i = "(int) " + to_string(message);
	logS(i, file, line, logInProduction);
}
