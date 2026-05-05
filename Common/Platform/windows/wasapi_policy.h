// Copyright (c) 2025 Rafael Valoto/Publisher. All rights reserved.
// Created for: GamepadCore - Plugin to support DualSense controller on Windows.
// Planned Release Year: 2025
#pragma once
#include "miniaudio.h"
#include <filesystem>
#include "windows_device_info.h"
#include <initguid.h>
#include <setupapi.h>
#include <windows.h>
#include <iostream>

#include <mmdeviceapi.h>
#include <propsys.h>
#include <vector>

#ifdef DEFINE_PROPERTYKEY
#undef DEFINE_PROPERTYKEY
#endif
#define DEFINE_PROPERTYKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) \
extern "C" const __declspec(selectany) PROPERTYKEY name = {{l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}, pid}

#ifdef DEFINE_DEVPROPKEY
#undef DEFINE_DEVPROPKEY
#endif
#define DEFINE_DEVPROPKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) \
extern "C" const __declspec(selectany) DEVPROPKEY name = {{l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}, pid}

#include <hidsdi.h>
#include <initguid.h>

#ifdef PKEY_Device_ContainerId
#undef PKEY_Device_ContainerId
#endif
DEFINE_PROPERTYKEY(PKEY_Device_ContainerId, 0x8c7ed206, 0x3f8a, 0x4827, 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);

#ifndef DEVPKEY_Device_ContainerId
DEFINE_DEVPROPKEY(DEVPKEY_Device_ContainerId, 0x8c7ed206, 0x3f8a, 0x4827, 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);
#endif

#ifndef PKEY_NAME
DEFINE_PROPERTYKEY(PKEY_NAME, 0xb725f130, 0x47ef, 0x101a, 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac, 10);
#endif

#pragma comment(lib, "Propsys.lib")

/**
 * @brief Audio device context using miniaudio for cross-platform audio playback.
 *
 * This replaces the previous WASAPI-specific implementation to support
 * Windows, Linux, and macOS platforms.
 */
struct wasapi_policy
{

	using Policy = wasapi_policy;

	using DevicePathType = std::string;
	using AudioDeviceType = ma_device;
	using AudioDeviceIdType = ma_device_id;
	using AudioRingBufferType = ma_pcm_rb;
	using AudioFrameCountType = ma_uint32;
	using ContextType = FDeviceContext;

	int NumChannels = 4;
	int SampleRate = 48000;
	bool bInitialized = false;
	bool bHasDeviceId = false;
	bool bRingBufferInitialized = false;
	bool bFoundDevice = false;

	DevicePathType DevicePath;
	AudioDeviceType Device{};
	AudioRingBufferType RingBuffer{};
	// Owned copy of the device ID (safe after ma_context_uninit)
	AudioDeviceIdType OwnedDeviceId{};
	const AudioDeviceIdType* DeviceId = nullptr;

	wasapi_policy() = default;
	~wasapi_policy() { Close(); };

	void Close()
	{
		if (bInitialized)
		{
			ma_device_stop(&Device);
			ma_device_uninit(&Device);
			bInitialized = false;
		}
		if (bRingBufferInitialized)
		{
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
		}
	}
	[[nodiscard]] bool IsValid() const
	{
		return bInitialized && bRingBufferInitialized;
	}

	bool WriteHapticData(const std::vector<std::int16_t>& InterleavedData)
	{
		std::cout << "[AudioPolicy] Writing haptic data to device" << std::endl;
		if (!IsValid() || InterleavedData.empty())
		{
			return false;
		}
		auto framesInput = static_cast<AudioFrameCountType>(InterleavedData.size() / 2);

		AudioFrameCountType framesAvailable = ma_pcm_rb_available_write(&RingBuffer);
		AudioFrameCountType framesToWrite = (framesInput > framesAvailable) ? framesAvailable : framesInput;

		if (framesToWrite == 0)
		{
			return true;
		}

		void* pWriteBufferPtr;
		if (ma_pcm_rb_acquire_write(&RingBuffer, &framesToWrite, &pWriteBufferPtr) != MA_SUCCESS)
		{
			return false;
		}

		auto* pOutputBuffer = static_cast<float*>(pWriteBufferPtr);
		constexpr float kNormalization = 1.0f / 32768.0f;
		for (AudioFrameCountType i = 0; i < framesToWrite; i++)
		{
			float LeftFloat = static_cast<float>(InterleavedData[i * 2]) * kNormalization;
			float RightFloat = static_cast<float>(InterleavedData[(i * 2) + 1]) * kNormalization;
			AudioFrameCountType baseIndex = i * NumChannels;

			if (NumChannels >= 4)
			{
				pOutputBuffer[baseIndex + 0] = 0.0f;
				pOutputBuffer[baseIndex + 1] = 0.0f;
				pOutputBuffer[baseIndex + 2] = LeftFloat;
				pOutputBuffer[baseIndex + 3] = RightFloat;
			}
			else
			{
				pOutputBuffer[baseIndex + 0] = LeftFloat;
				pOutputBuffer[baseIndex + 1] = RightFloat;
			}
		}

		ma_pcm_rb_commit_write(&RingBuffer, framesToWrite);
		return true;
	}

	static void DataCallback(AudioDeviceType* pDevice, void* pOutput, const void* /*pInput*/, AudioFrameCountType frameCount)
	{
		auto pContext = static_cast<wasapi_policy*>(pDevice->pUserData);
		if (!pContext || !pContext->bInitialized)
		{
			std::memset(pOutput, 0, frameCount * pDevice->playback.channels * sizeof(float));
			return;
		}

		AudioFrameCountType framesAvailable = ma_pcm_rb_available_read(&pContext->RingBuffer);
		AudioFrameCountType framesToRead = frameCount;

		if (framesAvailable < framesToRead)
		{
			framesToRead = framesAvailable;
		}

		if (framesToRead > 0)
		{
			void* pReadBuffer;
			AudioFrameCountType readSize = framesToRead;
			ma_pcm_rb_acquire_read(&pContext->RingBuffer, &readSize, &pReadBuffer);

			std::memcpy(pOutput, pReadBuffer, readSize * pContext->NumChannels * sizeof(float));
			ma_pcm_rb_commit_read(&pContext->RingBuffer, readSize);
		}

		if (framesToRead < frameCount)
		{
			auto pOutputFloat = static_cast<float*>(pOutput);
			AudioFrameCountType framesMissing = frameCount - framesToRead;
			std::memset(&pOutputFloat[framesToRead * pContext->NumChannels], 0,
			            framesMissing * pContext->NumChannels * sizeof(float));
		}
	}

	bool InitializeWithDeviceId(const AudioDeviceIdType* pDeviceId, int InSampleRate = 48000, int InNumChannels = 2)
	{
		if (pDeviceId)
		{
			DeviceId = pDeviceId;
			bHasDeviceId = true;
		}
		else
		{
			DeviceId = nullptr;
			bHasDeviceId = false;
		}

		SampleRate = InSampleRate;
		NumChannels = InNumChannels;
		auto bufferSizeInFrames = static_cast<AudioFrameCountType>(SampleRate);

		if (ma_pcm_rb_init(ma_format_f32, NumChannels, bufferSizeInFrames, nullptr, nullptr, &RingBuffer) != MA_SUCCESS)
		{
			return false;
		}
		bRingBufferInitialized = true;

		ma_device_config Config = ma_device_config_init(ma_device_type_playback);
		Config.playback.format = ma_format_f32;
		Config.playback.channels = 2;
		Config.playback.pDeviceID = DeviceId;
		Config.sampleRate = SampleRate;

		Config.dataCallback = DataCallback;
		Config.pUserData = this;

		if (ma_device_init(nullptr, &Config, &Device) != MA_SUCCESS)
		{
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
			return false;
		}

		if (ma_device_start(&Device) != MA_SUCCESS)
		{
			ma_device_uninit(&Device);
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
			return false;
		}

		bInitialized = true;
		return true;
	}

	bool InitializeAudioContainer(ContextType* Context)
	{
		if (!Context)
		{
			std::cerr << "[AudioPolicy] Error: Context is null" << std::endl;
			return false;
		}

		std::cout << "[AudioPolicy] Initializing audio for context: " << Context->Path << std::endl;

		ma_result result;
		ma_context maContext;
		result = ma_context_init(nullptr, 0, nullptr, &maContext);
		if (result != MA_SUCCESS)
		{
			std::cerr << "[AudioPolicy] Error: Failed to initialize miniaudio context: " << result << std::endl;
			return false;
		}

		ma_device_info* pPlaybackInfos;
		ma_uint32 playbackCount;
		ma_device_info* pCaptureInfos;
		ma_uint32 captureCount;

		result = ma_context_get_devices(&maContext, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
		if (result != MA_SUCCESS)
		{
			std::cerr << "[AudioPolicy] Error: Failed to get devices: " << result << std::endl;
			ma_context_uninit(&maContext);
			return false;
		}

		std::string TargetContainerId = get_container_id(Context->Path);
		std::cout << "[AudioPolicy] Target Device Container ID: " << TargetContainerId << std::endl;

		const ma_device_id* pFoundDeviceId = nullptr;
		for (ma_uint32 i = 0; i < playbackCount; i++)
		{
			std::string AudioContainerId = get_audio_container_id(pPlaybackInfos[i].id.wasapi);
			std::cout << "[AudioPolicy] Checking device: " << pPlaybackInfos[i].name
			          << " | Container ID: " << AudioContainerId << " TargetContainerId: " << TargetContainerId << std::endl;

			if (!AudioContainerId.empty() && AudioContainerId == TargetContainerId)
			{
				pFoundDeviceId = &pPlaybackInfos[i].id;
				std::cout << "[AudioPolicy] Found match by Container ID: " << pPlaybackInfos[i].name << std::endl;
				break;
			}
		}

		if (!pFoundDeviceId)
		{
			std::cout << "[AudioPolicy] No match by Container ID. Falling back to name search..." << std::endl;
			for (ma_uint32 i = 0; i < playbackCount; i++)
			{
				std::string deviceName(pPlaybackInfos[i].name);
				if (deviceName.find("DualSense") != std::string::npos ||
				    deviceName.find("Wireless Controller") != std::string::npos)
				{
					pFoundDeviceId = &pPlaybackInfos[i].id;
					std::cout << "[AudioPolicy] Found potential match by name: " << pPlaybackInfos[i].name << std::endl;
					break;
				}
			}
		}

		// Copy the device ID BEFORE ma_context_uninit invalidates pPlaybackInfos
		if (pFoundDeviceId)
		{
			OwnedDeviceId = *pFoundDeviceId;
			bFoundDevice = true;
		}

		ma_context_uninit(&maContext);
		if (bFoundDevice)
		{
			// OwnedDeviceId was copied before context uninit — safe to use
			DeviceId = &OwnedDeviceId;
			std::cout << "[AudioPolicy] Found audio device — initializing ring buffer and playback device..." << std::endl;
			if (!InitializeWithDeviceId(DeviceId))
			{
				std::cerr << "[AudioPolicy] Failed to initialize ring buffer/playback for " << Context->Path << std::endl;
				return false;
			}
			std::cout << "[AudioPolicy] Successfully initialized audio device for " << Context->Path << std::endl;
			return true;
		}

		std::cerr << "[AudioPolicy] Failed to find audio device for " << Context->Path << std::endl;
		return false;
	}

	std::string get_container_id(const std::string& DevicePath)
	{
		std::wstring WPath(DevicePath.begin(), DevicePath.end());
		GUID HidGuid;
		HidD_GetHidGuid(&HidGuid);

		HDEVINFO DeviceInfoSet = SetupDiGetClassDevsW(&HidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (DeviceInfoSet == INVALID_HANDLE_VALUE)
		{
			return "";
		}

		SP_DEVICE_INTERFACE_DATA DeviceInterfaceData = {sizeof(SP_DEVICE_INTERFACE_DATA)};
		if (SetupDiOpenDeviceInterfaceW(DeviceInfoSet, WPath.c_str(), 0, &DeviceInterfaceData))
		{
			SP_DEVINFO_DATA DeviceInfoData = {sizeof(SP_DEVINFO_DATA)};
			// DetailData is needed to get the DevInfoData associated with the interface
			DWORD RequiredSize = 0;
			SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet, &DeviceInterfaceData, nullptr, 0, &RequiredSize, nullptr);
			if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			{
				std::vector<char> Buffer(RequiredSize);
				PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)Buffer.data();
				pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
				if (SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet, &DeviceInterfaceData, pDetail, RequiredSize, nullptr, &DeviceInfoData))
				{
					DEVPROPTYPE PropType;
					GUID ContainerId = {0};
					if (SetupDiGetDevicePropertyW(DeviceInfoSet, &DeviceInfoData, &DEVPKEY_Device_ContainerId, &PropType, (PBYTE)&ContainerId, sizeof(GUID), nullptr, 0))
					{
						wchar_t GuidString[40];
						StringFromGUID2(ContainerId, GuidString, 40);
						SetupDiDestroyDeviceInfoList(DeviceInfoSet);

						char GuidStr[40];
						WideCharToMultiByte(CP_ACP, 0, GuidString, -1, GuidStr, 40, nullptr, nullptr);
						return std::string(GuidStr);
					}
				}
			}
		}
		SetupDiDestroyDeviceInfoList(DeviceInfoSet);
		return "";
	}

	std::string get_audio_container_id(const wchar_t* AudioDeviceId)
	{
		IMMDeviceEnumerator* pEnumerator = nullptr;
		IMMDevice* pDevice = nullptr;
		IPropertyStore* pProps = nullptr;
		std::string Result;

		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
		if (SUCCEEDED(hr))
		{
			hr = pEnumerator->GetDevice(AudioDeviceId, &pDevice);
			if (SUCCEEDED(hr))
			{
				hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
				if (SUCCEEDED(hr))
				{
					PROPVARIANT var;
					PropVariantInit(&var);
					hr = pProps->GetValue(PKEY_Device_ContainerId, &var);
					if (SUCCEEDED(hr) && var.vt == VT_CLSID)
					{
						wchar_t GuidString[40];
						StringFromGUID2(*var.puuid, GuidString, 40);

						char GuidStr[40];
						WideCharToMultiByte(CP_ACP, 0, GuidString, -1, GuidStr, 40, nullptr, nullptr);
						Result = std::string(GuidStr);
					}
					PropVariantClear(&var);
					pProps->Release();
				}
				pDevice->Release();
			}
			pEnumerator->Release();
		}

		return Result;
	}
};


