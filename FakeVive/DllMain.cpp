#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <MinHook.h>
#include <openvr.h>
#include <spdlog/spdlog.h>

namespace
{
	const char* Title = "FakeVive";

	const char* IVRSystemPrefix = "IVRSystem";
	const char* OverrideManufacturer = "HTC";
	const char* OverrideModelNumber = "Vive";

	const char* LogFileName = "FakeVive.log"; // Stored in %TEMP%
	std::shared_ptr<spdlog::logger> Log;

	const char* DebugFlag = "-FakeViveDebug"; // Debug mode will be enabled if this is found in the command line
	bool DebugModeEnabled = false;

	typedef HRESULT(WINAPI* TDirectDrawCreate)(GUID*, void*, void*);
	TDirectDrawCreate Actual_DirectDrawCreate;

	typedef void* (*T_VR_GetGenericInterface)(const char*, vr::EVRInitError*);
	T_VR_GetGenericInterface Actual_VR_GetGenericInterface;

	typedef uint32_t (*T_GetStringTrackedDeviceProperty)(vr::IVRSystem*, vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, char*, uint32_t, vr::ETrackedPropertyError*);
	T_GetStringTrackedDeviceProperty Actual_GetStringTrackedDeviceProperty;
	const auto VTable_GetStringTrackedDeviceProperty = 0x1A;

	void FatalError(const std::string& str)
	{
		if (Log)
			Log->error(str);
		MessageBox(nullptr, str.c_str(), Title, MB_OK | MB_ICONERROR);
		TerminateProcess(GetCurrentProcess(), 0);
	}

	std::string GetLogFilePath()
	{
		// %TEMP% folder
		char tempDir[MAX_PATH];
		auto tempDirLength = GetTempPath(sizeof(tempDir), tempDir);
		if (tempDirLength == 0 || tempDirLength > sizeof(tempDir))
			return LogFileName;
		return std::string(tempDir) + LogFileName;
	}

	bool CheckDebugModeEnabled()
	{
#if _DEBUG
		return true;
#else
		auto commandLine = GetCommandLine();
		return strstr(commandLine, DebugFlag) != nullptr;
#endif
	}

	void InitializeLogging()
	{
		// Always log to a file, but if debug mode is active then also display a console window
		std::vector<spdlog::sink_ptr> sinks;
		sinks.push_back(std::make_shared<spdlog::sinks::simple_file_sink_st>(GetLogFilePath(), /* truncate */ true));
		if (DebugModeEnabled)
		{
			AllocConsole();
			sinks.push_back(std::make_shared<spdlog::sinks::wincolor_stdout_sink_st>());
		}

		Log = std::make_shared<spdlog::logger>(Title, sinks.begin(), sinks.end());
		spdlog::register_logger(Log);
		Log->set_level(DebugModeEnabled ? spdlog::level::debug : spdlog::level::info);
		Log->flush_on(spdlog::level::debug); // needed or else the log will never flush

		Log->info("{} loaded", Title);
	}

	void InitializeDDrawProxy()
	{
		char systemRoot[32767];
		GetSystemDirectory(systemRoot, sizeof(systemRoot));
		auto ddrawPath = std::string(systemRoot) + "\\ddraw.dll";
		Log->debug("Using system ddraw.dll at {}", ddrawPath);
		auto ddraw = LoadLibrary(ddrawPath.c_str());
		if (!ddraw)
			FatalError("Failed to open the system ddraw.dll");
		Actual_DirectDrawCreate = reinterpret_cast<TDirectDrawCreate>(GetProcAddress(ddraw, "DirectDrawCreate"));
		if (!Actual_DirectDrawCreate)
			FatalError("Unable to locate DirectDrawCreate");
	}

	uint32_t My_GetStringTrackedDeviceProperty(vr::IVRSystem* system, vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError *pError)
	{
		Log->debug("IVRSystem::GetStringTrackedDeviceProperty {}", prop);
		auto size = Actual_GetStringTrackedDeviceProperty(system, unDeviceIndex, prop, pchValue, unBufferSize, pError);
		Log->debug("IVRSystem::GetStringTrackedDeviceProperty returned {}, error = {}, value = \"{}\"", size, *pError, *pError == vr::TrackedProp_Success ? pchValue : "");

		const char* overrideStr;
		switch (prop)
		{
		case vr::ETrackedDeviceProperty::Prop_ManufacturerName_String:
			overrideStr = OverrideManufacturer;
			break;
		case vr::ETrackedDeviceProperty::Prop_ModelNumber_String:
			overrideStr = OverrideModelNumber;
			break;
		default:
			Log->debug("Ignoring tracked device property {}", prop);
			return size;
		}
		Log->debug("Forcing tracked device property {} to \"{}\"", prop, overrideStr);

		size = static_cast<uint32_t>(strlen(overrideStr)) + 1;
		strncpy_s(pchValue, unBufferSize, overrideStr, size - 1);
		*pError = vr::TrackedProp_Success;
		return size;
	}

	void InstallDevicePropertyHook(vr::IVRSystem* system)
	{
		Log->info("Installing GetStringTrackedDeviceProperty hook");

		auto vtable = *reinterpret_cast<void***>(system);
		auto getStringTrackedDevicePropertyPtr = reinterpret_cast<T_GetStringTrackedDeviceProperty*>(&vtable[VTable_GetStringTrackedDeviceProperty]);

		// The vtable is readonly, so it needs to temporarily be made writable
		DWORD oldProtect;
		if (!VirtualProtect(getStringTrackedDevicePropertyPtr, sizeof(void*), PAGE_READWRITE, &oldProtect))
			FatalError("Failed to hook GetStringTrackedDeviceProperty");

		Actual_GetStringTrackedDeviceProperty = *getStringTrackedDevicePropertyPtr;
		*getStringTrackedDevicePropertyPtr = My_GetStringTrackedDeviceProperty;

		VirtualProtect(getStringTrackedDevicePropertyPtr, sizeof(void*), oldProtect, &oldProtect);

		Log->info("GetStringTrackedDeviceProperty hook installed");
	}

	void* My_VR_GetGenericInterface(const char* pchInterfaceVersion, vr::EVRInitError* peError)
	{
		Log->debug("VR_GetGenericInterface \"{}\"", pchInterfaceVersion);

		static auto hookInstalled = false;
		auto iface = Actual_VR_GetGenericInterface(pchInterfaceVersion, peError);
		if (!hookInstalled && iface && strncmp(pchInterfaceVersion, IVRSystemPrefix, strlen(IVRSystemPrefix)) == 0)
		{
			auto system = static_cast<vr::IVRSystem*>(iface);
			InstallDevicePropertyHook(system);
			hookInstalled = true;
		}
		return iface;
	}

	void InstallOpenVRInterfaceHook()
	{
		Log->info("Installing VR_GetGenericInterface hook");

		if (MH_Initialize() != MH_OK)
			FatalError("Failed to initialize MinHook");
		if (MH_CreateHook(vr::VR_GetGenericInterface, My_VR_GetGenericInterface, reinterpret_cast<LPVOID*>(&Actual_VR_GetGenericInterface)) != MH_OK)
			FatalError("Failed to hook VR_GetGenericInterface");
		if (MH_EnableHook(vr::VR_GetGenericInterface) != MH_OK)
			FatalError("Failed to enable VR_GetGenericInterface hook");

		Log->info("VR_GetGenericInterface hook installed");
	}

	void Initialize()
	{
		DebugModeEnabled = CheckDebugModeEnabled();
		InitializeLogging();
		InitializeDDrawProxy();
		InstallOpenVRInterfaceHook();
	}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hinstDLL);
		Initialize();
	}
	return TRUE;
}

// This should be the only function that needs to be implemented for opengl32.dll apps
HRESULT WINAPI My_DirectDrawCreate(GUID *lpGUID, void* lplpDD, void* pUnkOuter)
{
	Log->debug("Forwarding DirectDrawCreate call to system ddraw.dll");
	return Actual_DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}
