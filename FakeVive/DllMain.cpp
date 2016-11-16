#include <Windows.h>
#include <string>
#include <MinHook.h>
#include <openvr.h>

namespace
{
	const char* Title = "FakeVive";

	const char* IVRSystemPrefix = "IVRSystem";
	const char* OverrideManufacturer = "HTC";
	const char* OverrideModelNumber = "Vive";

	typedef HRESULT(WINAPI* TDirectDrawCreate)(GUID*, void*, void*);
	TDirectDrawCreate Actual_DirectDrawCreate;

	typedef void* (*T_VR_GetGenericInterface)(const char*, vr::EVRInitError*);
	T_VR_GetGenericInterface Actual_VR_GetGenericInterface;

	typedef uint32_t (*T_GetStringTrackedDeviceProperty)(vr::IVRSystem*, vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, char*, uint32_t, vr::ETrackedPropertyError*);
	T_GetStringTrackedDeviceProperty Actual_GetStringTrackedDeviceProperty;
	const auto VTable_GetStringTrackedDeviceProperty = 0x1A;

	void FatalError(const std::string& str)
	{
		MessageBox(nullptr, str.c_str(), Title, MB_OK | MB_ICONERROR);
		TerminateProcess(GetCurrentProcess(), 0);
	}

	template<class T>
	T GetProcOrExit(HMODULE module, const char* name)
	{
		auto fn = reinterpret_cast<T>(GetProcAddress(module, name));
		if (!fn)
		{
			char moduleName[32767] = {0};
			GetModuleFileName(module, moduleName, sizeof(moduleName));
			FatalError(std::string("Failed to locate procedure ") + name + std::string(" in ") + moduleName);
		}
		return fn;
	}

	void InitializeDDrawProxy()
	{
		char systemRoot[32767];
		GetSystemDirectory(systemRoot, sizeof(systemRoot));
		auto ddrawPath = std::string(systemRoot) + "\\ddraw.dll";
		auto ddraw = LoadLibrary(ddrawPath.c_str());
		if (!ddraw)
			FatalError("Failed to open the system ddraw.dll");
		Actual_DirectDrawCreate = GetProcOrExit<TDirectDrawCreate>(ddraw, "DirectDrawCreate");
	}

	uint32_t My_GetStringTrackedDeviceProperty(vr::IVRSystem* system, vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError *pError)
	{
		auto size = Actual_GetStringTrackedDeviceProperty(system, unDeviceIndex, prop, pchValue, unBufferSize, pError);
		if (size <= 0)
			return size;

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
			return size;
		}
		size = static_cast<uint32_t>(strlen(overrideStr)) + 1;
		strncpy_s(pchValue, unBufferSize, overrideStr, size - 1);
		return size;
	}

	void InstallDevicePropertyHook(vr::IVRSystem* system)
	{
		auto vtable = *reinterpret_cast<void***>(system);
		auto getStringTrackedDevicePropertyPtr = reinterpret_cast<T_GetStringTrackedDeviceProperty*>(&vtable[VTable_GetStringTrackedDeviceProperty]);

		// The vtable is readonly, so it needs to temporarily be made writable
		DWORD oldProtect;
		if (!VirtualProtect(getStringTrackedDevicePropertyPtr, sizeof(void*), PAGE_READWRITE, &oldProtect))
			FatalError("Failed to hook GetStringTrackedDeviceProperty");

		Actual_GetStringTrackedDeviceProperty = *getStringTrackedDevicePropertyPtr;
		*getStringTrackedDevicePropertyPtr = My_GetStringTrackedDeviceProperty;

		VirtualProtect(getStringTrackedDevicePropertyPtr, sizeof(void*), oldProtect, &oldProtect);
	}

	void* My_VR_GetGenericInterface(const char* pchInterfaceVersion, vr::EVRInitError* peError)
	{
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
		if (MH_Initialize() != MH_OK)
			FatalError("Failed to initialize MinHook");
		if (MH_CreateHook(vr::VR_GetGenericInterface, My_VR_GetGenericInterface, reinterpret_cast<LPVOID*>(&Actual_VR_GetGenericInterface)) != MH_OK)
			FatalError("Failed to hook VR_GetGenericInterface");
		if (MH_EnableHook(vr::VR_GetGenericInterface) != MH_OK)
			FatalError("Failed to enable VR_GetGenericInterface hook");
	}

	void Initialize()
	{
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
	return Actual_DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}