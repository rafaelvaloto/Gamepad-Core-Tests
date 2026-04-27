// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
#pragma once
#ifdef BUILD_GAMEPAD_CORE_TESTS
#include "GCore/Templates/TGenericHardwareInfo.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "GCore/Utils/SoDefines.h"
#include "miniaudio.h"
#include "windows_device_info.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <mutex>
#include <propsys.h>
#include <set>
#include <string>
#include <vector>

#if GAMEPAD_CORE_HAS_AUDIO
#include "miniaudio.h"
#endif

#include "GCore/Interfaces/IAudioDevice.h"
#include "GImplementations/Utils/GamepadAudio.h"
using namespace GamepadCore;
using namespace FGamepadAudio;

namespace windows_platform
{
	struct windows_hardware_policy;
	using windows_hardware = TGenericHardwareInfo<windows_hardware_policy>;
	
	struct windows_hardware_policy
	{
		windows_hardware_policy() = default;

		void Read(FDeviceContext* Context)
		{
			windows_device_info::read(Context);
		}

		void Write(FDeviceContext* Context)
		{
			windows_device_info::write(Context);
		}

		void Detect(std::vector<FDeviceContext>& Devices)
		{
			windows_device_info::detect(Devices);
		}

		bool CreateHandle(FDeviceContext* Context)
		{
			return windows_device_info::create_handle(Context);
		}

		void InvalidateHandle(FDeviceContext* Context)
		{
			windows_device_info::invalidate_handle(Context);
		}

		void ProcessAudioHaptic(FDeviceContext* Context)
		{
			windows_device_info::process_audio_haptic(Context);
		}

		void ProcessAudioHaptic(FDeviceContext* Context, const std::vector<std::int16_t>& AudioData)
		{
			if (!Context) return;
			IAudioDevice::Get().ProcessAudioHaptic(Context, AudioData);
		}

		void InitializeAudioDevice(FDeviceContext* Context)
		{
			IAudioDevice::Get().InitializeAudioContainer(Context);
		}

		class IGamepadBase* GetLibrary(uint32_t EngineDeviceId)
		{
			return nullptr; // Should be handled by Registry
		}

		void SetRegistry(class IDeviceRegistry* InRegistry)
		{
		}

	};

} // namespace windows_platform
#endif
