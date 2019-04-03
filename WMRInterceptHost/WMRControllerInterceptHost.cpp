#include "stdafx.h"
#include "WMRControllerInterceptHost.h"
#include "PipeServer.h"
#include "HookCommon.h"

extern "C"
{
	DWORD HookCrystalKeyStartIMUStream_RSPSubOffs;
	DWORD HookCrystalKeyStopIMUStream_RSPSubOffs;
	UINT_PTR HookCrystalKeyStopIMUStream_CMPAddr;
}
static void *g_pCrystalKeyStartIMUStreamHook;
static BYTE g_CrystalKeyStartIMUStreamHook_Backup[15];
static void *g_pCrystalKeyStopIMUStreamHook;
static BYTE g_CrystalKeyStopIMUStreamHook_Backup[17];
static HMODULE g_hMotionControllerHid;
static HMODULE g_hMotionControllerSystem;
static bool g_started = false;

//120 bytes
struct IMUData
{
	float gyroscope[3];
	float unknown1;
	float accelerometer[3];
	DWORD padding1; //guessed
	uint64_t timestamp1; //100ns tick
	uint64_t timestamp2; //smaller than timestamp1 (but still 100ns per tick)
	float magnetometer[3]; //at least manipulatable with a magnet
	DWORD padding2; //guessed
	uint64_t timestamp1b; //equal to timestamp1?
	uint64_t timestamp2b; //equal to timestamp2?
	uint64_t unknown4[3];
	DWORD unknown5; //known values : 0,1,2,3
	DWORD padding3; //guessed
};

typedef void(*DeviceStateChangeCallback)(DWORD controllerHandle, DWORD state, UINT_PTR userData);
//modeInfo : 0 == idle (energy saving); 1 == moving; hex 0000000100000001 == ???; 
typedef void(*DeviceModeCallback)(DWORD controllerHandle, UINT_PTR modeInfo, UINT_PTR userData);
//Known value : 4 (?? happens after moving the controller again, not in visual range); 3; 2; 1; 0 (?? happened after going idle again)
typedef void(*DeviceStatusCallback)(DWORD controllerHandle, DWORD state, UINT_PTR userData);
typedef void(*IMUStreamCallback)(DWORD controllerHandle, IMUData *imuData, UINT_PTR userData);

typedef HRESULT(*tdCrystalKeyRegisterForDeviceStateChange)(DeviceStateChangeCallback callback, UINT_PTR userData);
typedef HRESULT(*tdCrystalKeyGetDeviceType)(DWORD controllerHandle, DWORD *controllerLeftOrRight);
typedef HRESULT(*tdCrystalKeySetDeviceModeCallback)(DWORD controllerHandle, DeviceModeCallback callback, UINT_PTR userData);
typedef HRESULT(*tdCrystalKeySetDeviceStatusCallback)(DWORD controllerHandle, DeviceStatusCallback callback, UINT_PTR userData);
typedef HRESULT(*tdCrystalKeyStartIMUStream)(DWORD controllerHandle, IMUStreamCallback callback, UINT_PTR userData);
typedef HRESULT(*tdCrystalKeyStopIMUStream)(DWORD controllerHandle);

static tdCrystalKeyRegisterForDeviceStateChange CrystalKeyRegisterForDeviceStateChange;
static tdCrystalKeyGetDeviceType CrystalKeyGetDeviceType;
//static tdCrystalKeySetDeviceModeCallback CrystalKeySetDeviceModeCallback;
//static tdCrystalKeySetDeviceStatusCallback CrystalKeySetDeviceStatusCallback;
static tdCrystalKeyStartIMUStream CrystalKeyStartIMUStream;
static tdCrystalKeyStopIMUStream CrystalKeyStopIMUStream;


struct ControllerIMUCallbackData
{
	DWORD controllerHandle;
	DWORD type;
	IMUStreamCallback origCallback;
};
static bool g_ControllerIMUSectionInitialized = false;
static CRITICAL_SECTION g_ControllerIMUSection;
static std::vector<ControllerIMUCallbackData> g_ControllerIMUCallbacks;

unsigned int g_UsingControllerIMUSection = 0;

static void *g_DeviceStateChangeCallbackPage;

static void IMUStreamIntercept(DWORD controllerHandle, IMUData *imuData, UINT_PTR userData)
{
	DWORD controllerType = (DWORD)-1;
	IMUStreamCallback origCallback = nullptr;
	InterlockedIncrement(&g_UsingControllerIMUSection);
	if (!g_started)
	{
		InterlockedDecrement(&g_UsingControllerIMUSection);
		return;
	}
	EnterCriticalSection(&g_ControllerIMUSection);
	for (size_t i = 0; i < g_ControllerIMUCallbacks.size(); i++)
	{
		if (g_ControllerIMUCallbacks[i].controllerHandle == controllerHandle)
		{
			origCallback = g_ControllerIMUCallbacks[i].origCallback;
			controllerType = g_ControllerIMUCallbacks[i].type;
			break;
		}
	}
	LeaveCriticalSection(&g_ControllerIMUSection);
	InterlockedDecrement(&g_UsingControllerIMUSection);

	if (controllerType == (DWORD)-1)
		CrystalKeyGetDeviceType(controllerHandle, &controllerType);

	ControllerStreamData streamData = {};
	memcpy(streamData.accelerometer, imuData->accelerometer, sizeof(IMUData::accelerometer));
	memcpy(streamData.gyroscope, imuData->gyroscope, sizeof(IMUData::gyroscope));
	memcpy(streamData.magnetometer, imuData->magnetometer, sizeof(IMUData::magnetometer));
	streamData.unknown1 = imuData->unknown1;
	streamData.timestamp1 = imuData->timestamp1;
	streamData.timestamp2 = imuData->timestamp2;
	streamData.unknown2 = imuData->unknown5;
	OnControllerStreamData(controllerHandle, (BYTE)controllerType, streamData);

	if (origCallback)
	{
		origCallback(controllerHandle, imuData, userData);
	}
}
static void OnDeviceStateChange(DWORD controllerHandle, DWORD state, UINT_PTR userData)
{
	switch (state)
	{
	case 0: //connected
		OnControllerTrackingStart(controllerHandle);
		break;
	case 1: //disconnected
		OnControllerTrackingStop(controllerHandle);
		break;
	}
}

extern "C" void _OnCrystalKeyStartIMUStream(DWORD controllerHandle, IMUStreamCallback *pCallback, UINT_PTR *pUserData)
{
	*pCallback = IMUStreamIntercept;
}
extern "C" void _OnSuccessCrystalKeyStartIMUStream(DWORD controllerHandle, IMUStreamCallback oldCallback, UINT_PTR oldUserData)
{
	DWORD controllerType = (DWORD)-1;
	bool hasIMUCallback = false;
	InterlockedIncrement(&g_UsingControllerIMUSection);
	if (!g_started)
	{
		InterlockedDecrement(&g_UsingControllerIMUSection);
		return;
	}
	EnterCriticalSection(&g_ControllerIMUSection);
	for (size_t i = 0; i < g_ControllerIMUCallbacks.size(); i++)
	{
		if (g_ControllerIMUCallbacks[i].controllerHandle == controllerHandle)
		{
			hasIMUCallback = true;
			controllerType = g_ControllerIMUCallbacks[i].type;
			g_ControllerIMUCallbacks[i].origCallback = oldCallback;
			break;
		}
	}
	if (!hasIMUCallback)
	{
		CrystalKeyGetDeviceType(controllerHandle, &controllerType);
		g_ControllerIMUCallbacks.push_back({ controllerHandle, controllerType, oldCallback });
	}
	LeaveCriticalSection(&g_ControllerIMUSection);
	InterlockedDecrement(&g_UsingControllerIMUSection);

	if (!hasIMUCallback)
	{
		OnControllerStreamStart(controllerHandle, (BYTE)controllerType);
	}
}
extern "C" void _OnPostCrystalKeyStopIMUStream(DWORD controllerHandle)
{
	DWORD controllerType = (DWORD)-1;
	bool hasIMUCallback = false;
	InterlockedIncrement(&g_UsingControllerIMUSection);
	if (!g_started)
	{
		InterlockedDecrement(&g_UsingControllerIMUSection);
		return;
	}
	EnterCriticalSection(&g_ControllerIMUSection);
	for (size_t i = 0; i < g_ControllerIMUCallbacks.size(); i++)
	{
		if (g_ControllerIMUCallbacks[i].controllerHandle == controllerHandle)
		{
			hasIMUCallback = true;
			controllerType = g_ControllerIMUCallbacks[i].type;
			g_ControllerIMUCallbacks.erase(g_ControllerIMUCallbacks.begin() + i);
			break;
		}
	}
	LeaveCriticalSection(&g_ControllerIMUSection);
	InterlockedDecrement(&g_UsingControllerIMUSection);
	if (!hasIMUCallback)
	{
		OnErrorLog("Intercepted a CrystalKeyStopIMUStream call on a controller which I did not hook!");
	}
	else
	{
		OnControllerStreamStop(controllerHandle, (BYTE)controllerType);
	}
}

//Hook.asm import
extern "C" void _Hook_CrystalKeyStartIMUStream();
extern "C" void _Hook_CrystalKeyStopIMUStream();

//MotionControllerHid.dll
//48 89 98 ?? ?? ?? ?? 48 89 B0 ?? ?? ?? ??
static const BYTE IMUStreamCallbackOffs_Pattern[] = {
	0x48, 0x89, 0x98, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB0, 0x00, 0x00, 0x00, 0x00
};
static const BYTE IMUStreamCallbackOffs_Mask[] = {
	0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
};
//B8 01 00 00 00 33 FF 87 81 ?? ?? ?? ??
static const BYTE IMUStreamActiveOffs_Pattern[] = {
	0xB8, 0x01, 0x00, 0x00, 0x00, 0x33, 0xFF, 0x87, 0x81, 0x00, 0x00, 0x00, 0x00
};
static const BYTE IMUStreamActiveOffs_Mask[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
};
//48 23 0D ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 48 8B C1 48 8B 15 ?? ?? ?? ?? 48 03 C0
static const BYTE SearchController_Pattern[] = {
	0x48, 0x23, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x03, 0xC0
};
static const BYTE SearchController_Mask[] = {
	0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF
};
//48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ??
static const BYTE CrystalKeyStartIMUStream_Pattern[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x00
};
static const BYTE CrystalKeyStartIMUStream_Mask[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};
//48 89 5C 24 08 57 48 83 EC ?? 83 3D ?? ?? ?? ?? 00
static const BYTE CrystalKeyStopIMUStream_Pattern[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x00, 0x83, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const BYTE CrystalKeyStopIMUStream_Mask[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF
};

//MotionControllerSystem.dll


struct ControllerListHead
{
	//Either a self pointer or a ControllerListEntry.
	void *pLeftLink;
	//Either a self pointer or a ControllerListEntry.
	void *pRightLink;
};
struct ControllerListEntry
{
	//Either another ControllerListEntry or the ControllerListHead.
	void *pLeftLink;
	//Either another ControllerListEntry or the ControllerListHead.
	void *pRightLink;
	//The controller handle used for the CrystalKey* functions.
	DWORD controllerHandle;
	//The actual object the CrystalKey* functions work with.
	void *controllerObject;
	//Reference counter object.
	void *refCounterObject;
	//Rest unknown.
};

void WMRControllerInterceptHost::Startup()
{
	if (g_started)
		return;
	g_hMotionControllerHid = GetModuleHandle(TEXT("MotionControllerHid.dll"));
	if (!g_hMotionControllerHid)
	{
		OnErrorLog("ERROR: Can't find MotionControllerHid.dll!\r\n");
		goto fail;
	}
	g_hMotionControllerSystem = GetModuleHandle(TEXT("MotionControllerSystem.dll"));
	if (!g_hMotionControllerSystem)
	{
		OnErrorLog("ERROR: Can't find MotionControllerHid.dll!\r\n");
		goto fail;
	}
	void *pTextSection = nullptr;
	size_t textSectionLen = 0;
	GetImageSection(g_hMotionControllerHid, pTextSection, textSectionLen, ".text");
	if (!pTextSection || textSectionLen == 0)
	{
		OnErrorLog("ERROR: Can't find the .text section of MotionControllerHid.dll!\r\n");
		goto fail;
	}

	CrystalKeyRegisterForDeviceStateChange = (tdCrystalKeyRegisterForDeviceStateChange)GetProcAddress(g_hMotionControllerHid, "CrystalKeyRegisterForDeviceStateChange");
	CrystalKeyGetDeviceType = (tdCrystalKeyGetDeviceType)GetProcAddress(g_hMotionControllerHid, "CrystalKeyGetDeviceType");
	CrystalKeyStartIMUStream = (tdCrystalKeyStartIMUStream)GetProcAddress(g_hMotionControllerHid, "CrystalKeyStartIMUStream");
	CrystalKeyStopIMUStream = (tdCrystalKeyStopIMUStream)GetProcAddress(g_hMotionControllerHid, "CrystalKeyStopIMUStream");
	if (!CrystalKeyRegisterForDeviceStateChange)
	{
		OnErrorLog("ERROR: CrystalKeyRegisterForDeviceStateChange not found!\r\n");
		goto fail;
	}
	if (!CrystalKeyGetDeviceType)
	{
		OnErrorLog("ERROR: CrystalKeyGetDeviceType not found!\r\n");
		goto fail;
	}
	if (!CrystalKeyStartIMUStream ||
		!FindPattern(CrystalKeyStartIMUStream, sizeof(CrystalKeyStartIMUStream_Pattern), CrystalKeyStartIMUStream_Pattern, CrystalKeyStartIMUStream_Mask, sizeof(CrystalKeyStartIMUStream_Pattern)))
	{
		OnErrorLog("ERROR: CrystalKeyStartIMUStream not found or has an unknown function body!\r\n");
		goto fail;
	}
	if (!CrystalKeyStopIMUStream ||
		!FindPattern(CrystalKeyStopIMUStream, sizeof(CrystalKeyStopIMUStream_Pattern), CrystalKeyStopIMUStream_Pattern, CrystalKeyStopIMUStream_Mask, sizeof(CrystalKeyStopIMUStream_Pattern)))
	{
		OnErrorLog("ERROR: CrystalKeyStopIMUStream not found or has an unknown function body!\r\n");
		goto fail;
	}

	InitializeCriticalSection(&g_ControllerIMUSection);
	g_ControllerIMUSectionInitialized = true;

	void *pSearchControllerResult = FindPattern(pTextSection, textSectionLen, SearchController_Pattern, SearchController_Mask, sizeof(SearchController_Pattern));
	if (!pSearchControllerResult)
	{
		OnErrorLog("WARNING: Unable to find the SearchController pattern in MotionControllerHid.dll!\r\n");
	}
	else
	{
		void *pIMUStreamCallbackOffsResult = FindPattern(CrystalKeyStartIMUStream, min(512, textSectionLen - ((UINT_PTR)CrystalKeyStartIMUStream - (UINT_PTR)pTextSection)),
			IMUStreamCallbackOffs_Pattern, IMUStreamCallbackOffs_Mask, sizeof(IMUStreamCallbackOffs_Pattern));
		void *pIMUStreamActiveOffsResult = FindPattern(CrystalKeyStartIMUStream, min(512, textSectionLen - ((UINT_PTR)CrystalKeyStartIMUStream - (UINT_PTR)pTextSection)),
			IMUStreamActiveOffs_Pattern, IMUStreamActiveOffs_Mask, sizeof(IMUStreamActiveOffs_Pattern));
		void *pSearchControllerResult = FindPattern(pTextSection, textSectionLen, SearchController_Pattern, SearchController_Mask, sizeof(SearchController_Pattern));
		if (!pIMUStreamCallbackOffsResult)
		{
			OnErrorLog("WARNING: Unable to find the IMUStreamCallbackOffs pattern in MotionControllerHid.dll!\r\n");
		}
		else if (!pIMUStreamCallbackOffsResult)
		{
			OnErrorLog("WARNING: Unable to find the IMUStreamActiveOffs pattern in MotionControllerHid.dll!\r\n");
		}
		else if (!pSearchControllerResult)
		{
			OnErrorLog("WARNING: Unable to find the SearchController pattern in MotionControllerHid.dll!\r\n");
		}
		else
		{
			DWORD userDataOffs = *(DWORD*)((UINT_PTR)pIMUStreamCallbackOffsResult + 3);
			DWORD callbackOffs = *(DWORD*)((UINT_PTR)pIMUStreamCallbackOffsResult + 10);
			DWORD activeOffs = *(DWORD*)((UINT_PTR)pIMUStreamActiveOffsResult + 9);
			ControllerListHead *pListHead = *(ControllerListHead**)(((UINT_PTR)pSearchControllerResult + 24) + *(DWORD*)((UINT_PTR)pSearchControllerResult + 20));
			if (pListHead)
			{
				ControllerListEntry *pCurEntry = (ControllerListEntry*)pListHead;
				while ((pCurEntry = (ControllerListEntry*)pCurEntry->pLeftLink) != (ControllerListEntry*)pListHead)
				{
					DWORD type = (DWORD)-1;
					if (pCurEntry->controllerObject &&
						!FAILED(CrystalKeyGetDeviceType(pCurEntry->controllerHandle, &type)) &&
						type < 2)
					{
						OnControllerTrackingStart(pCurEntry->controllerHandle);
						if (*(DWORD*)((UINT_PTR)pCurEntry->controllerObject + activeOffs) == 1)
						{
							OnControllerStreamStart(pCurEntry->controllerHandle, (BYTE)type);
							IMUStreamCallback oldCallback = (IMUStreamCallback)*(UINT_PTR*)((UINT_PTR)pCurEntry->controllerObject + callbackOffs);
							g_ControllerIMUCallbacks.push_back({ pCurEntry->controllerHandle, type, oldCallback });
							*(UINT_PTR*)((UINT_PTR)pCurEntry->controllerObject + callbackOffs) = (UINT_PTR)(IMUStreamCallback)IMUStreamIntercept;
						}
					}
					else
					{
						char logTmp[128];
						sprintf_s(logTmp, "Invalid controller list entry %p.\r\n", pCurEntry);
						OnErrorLog(logTmp);
					}
				}
			}
		}
	}

	//Since there is no unregister counterpart to CrystalKeyRegisterForDeviceStateChange, allocate an executable page in new memory that jumps to the actual callback.
	//When closing the host, overwrite the memory with a RET instruction.
	//The drawback is the memory leak of 4KiB (one page) / 64KiB of address space (allocation granularity).
	g_DeviceStateChangeCallbackPage = VirtualAlloc(NULL, 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!g_DeviceStateChangeCallbackPage)
		OnErrorLog("Unable to register a device state change callback (allocation failed)!\r\n");
	else
	{
		BYTE *pDeviceStateChangeCallbackPage = (BYTE*)g_DeviceStateChangeCallbackPage;
		pDeviceStateChangeCallbackPage[0] = 0x48;
		pDeviceStateChangeCallbackPage[1] = 0xB8;
		*(unsigned long long int*)(&pDeviceStateChangeCallbackPage[2]) = (unsigned long long int)&OnDeviceStateChange;
		pDeviceStateChangeCallbackPage[10] = 0xFF;
		pDeviceStateChangeCallbackPage[11] = 0xE0;
		DWORD oldProt;
		if (!VirtualProtect(g_DeviceStateChangeCallbackPage, 12, PAGE_EXECUTE_READ, &oldProt) ||
			FAILED(CrystalKeyRegisterForDeviceStateChange((DeviceStateChangeCallback)g_DeviceStateChangeCallbackPage, 1))) //Null userData values are rejected.
		{
			OnErrorLog("Unable to register a device state change callback!\r\n");
			VirtualFree(g_DeviceStateChangeCallbackPage, 0, MEM_RELEASE);
			g_DeviceStateChangeCallbackPage = nullptr;
		}
	}

	DWORD oldProt = 0;
	VirtualProtect(pTextSection, textSectionLen, PAGE_EXECUTE_READWRITE, &oldProt);

	BYTE *pCrystalKeyStartIMUStream = (BYTE*)CrystalKeyStartIMUStream;
	BYTE *pCrystalKeyStopIMUStream = (BYTE*)CrystalKeyStopIMUStream;

	memcpy(g_CrystalKeyStartIMUStreamHook_Backup, pCrystalKeyStartIMUStream, 15);
	g_pCrystalKeyStartIMUStreamHook = pCrystalKeyStartIMUStream;

	HookCrystalKeyStartIMUStream_RSPSubOffs = ((BYTE*)pCrystalKeyStartIMUStream)[14];
	pCrystalKeyStartIMUStream[0] = 0x48;
	pCrystalKeyStartIMUStream[1] = 0xB8;
	*(unsigned long long int*)(&pCrystalKeyStartIMUStream[2]) = (unsigned long long int)&_Hook_CrystalKeyStartIMUStream;
	pCrystalKeyStartIMUStream[10] = 0xFF;
	pCrystalKeyStartIMUStream[11] = 0xD0;
	memset(&pCrystalKeyStartIMUStream[12], 0x90, 3);

	memcpy(g_CrystalKeyStopIMUStreamHook_Backup, pCrystalKeyStopIMUStream, 17);
	g_pCrystalKeyStopIMUStreamHook = pCrystalKeyStopIMUStream;

	HookCrystalKeyStopIMUStream_RSPSubOffs = ((BYTE*)pCrystalKeyStopIMUStream)[9];
	HookCrystalKeyStopIMUStream_CMPAddr = (UINT_PTR)pCrystalKeyStopIMUStream + 17 + (UINT_PTR)((INT_PTR)(*(int*)(&pCrystalKeyStopIMUStream[12])));
	pCrystalKeyStopIMUStream[0] = 0x48;
	pCrystalKeyStopIMUStream[1] = 0xB8;
	*(unsigned long long int*)(&pCrystalKeyStopIMUStream[2]) = (unsigned long long int)&_Hook_CrystalKeyStopIMUStream;
	pCrystalKeyStopIMUStream[10] = 0xFF;
	pCrystalKeyStopIMUStream[11] = 0xD0;
	memset(&pCrystalKeyStopIMUStream[12], 0x90, 5);

	VirtualProtect(pTextSection, textSectionLen, oldProt, &oldProt);

	g_started = true;
fail:
	if (!g_started)
	{
		if (g_ControllerIMUSectionInitialized)
		{
			DeleteCriticalSection(&g_ControllerIMUSection);
			g_ControllerIMUSectionInitialized = false;
		}
		if (g_DeviceStateChangeCallbackPage)
		{
			DWORD oldProt;
			VirtualProtect(g_DeviceStateChangeCallbackPage, 12, PAGE_EXECUTE_READWRITE, &oldProt);
			*(BYTE*)g_DeviceStateChangeCallbackPage = 0xC3;
			VirtualProtect(g_DeviceStateChangeCallbackPage, 12, PAGE_EXECUTE_READ, &oldProt);
			g_DeviceStateChangeCallbackPage = nullptr;
		}
		if (g_hMotionControllerHid)
		{
			FreeLibrary(g_hMotionControllerHid);
			g_hMotionControllerHid = NULL;
		}
		if (g_hMotionControllerSystem)
		{
			FreeLibrary(g_hMotionControllerSystem);
			g_hMotionControllerSystem = NULL;
		}
		OnErrorLog("Controller hooks are disabled.\r\n");
	}
}
void WMRControllerInterceptHost::Shutdown()
{
	if (!g_started)
		return;
	if (g_hMotionControllerHid)
	{
		void *pTextSection = nullptr;
		size_t textSectionLen = 0;
		GetImageSection(g_hMotionControllerHid, pTextSection, textSectionLen, ".text");
		if (pTextSection && textSectionLen)
		{
			DWORD oldProt = 0;
			VirtualProtect(pTextSection, textSectionLen, PAGE_EXECUTE_READWRITE, &oldProt);

			if (g_pCrystalKeyStartIMUStreamHook)
			{
				memcpy(g_pCrystalKeyStartIMUStreamHook, g_CrystalKeyStartIMUStreamHook_Backup, 15);
			}
			if (g_pCrystalKeyStopIMUStreamHook)
			{
				memcpy(g_pCrystalKeyStopIMUStreamHook, g_CrystalKeyStopIMUStreamHook_Backup, 17);
			}

			VirtualProtect(pTextSection, textSectionLen, oldProt, &oldProt);
		}

		FreeLibrary(g_hMotionControllerHid);
		g_hMotionControllerHid = NULL;
	}
	if (g_hMotionControllerSystem)
	{
		FreeLibrary(g_hMotionControllerSystem);
		g_hMotionControllerSystem = NULL;
	}
	if (g_DeviceStateChangeCallbackPage)
	{
		DWORD oldProt;
		VirtualProtect(g_DeviceStateChangeCallbackPage, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		*(BYTE*)g_DeviceStateChangeCallbackPage = 0xC3;
		VirtualProtect(g_DeviceStateChangeCallbackPage, 1, PAGE_EXECUTE_READ, &oldProt);
		g_DeviceStateChangeCallbackPage = nullptr;
	}

	g_started = false;

	if (g_ControllerIMUSectionInitialized)
	{
		EnterCriticalSection(&g_ControllerIMUSection);
		LeaveCriticalSection(&g_ControllerIMUSection);
		//Busy waiting so nobody uses/is about to use the critical section.
		while (g_UsingControllerIMUSection) { Sleep(0); }
		DeleteCriticalSection(&g_ControllerIMUSection);
		g_ControllerIMUSectionInitialized = false;
	}
	g_ControllerIMUCallbacks.clear();
}