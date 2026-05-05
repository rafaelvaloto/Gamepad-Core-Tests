// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
#pragma once
#ifdef BUILD_GAMEPAD_CORE_TESTS
#include "GCore/Templates/TGenericHardwareInfo.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#if GAMEPAD_CORE_HAS_AUDIO
#include "miniaudio.h"
#endif
#include "linux_device_info.h"
#include "../../test_utils.h"
#include <string>
#include <vector>

namespace linux_platform
{
	struct linux_hardware_policy;
	using linux_hardware = GamepadCore::TGenericHardwareInfo<linux_hardware_policy>;

	struct linux_hardware_policy
	{
		linux_hardware_policy() = default;

		static void Read(FDeviceContext* Context)
		{
			linux_device_info::read(Context);
		}

		static void Write(FDeviceContext* Context)
		{
			linux_device_info::write(Context);
		}

		static void Detect(std::vector<FDeviceContext>& Devices)
		{
			linux_device_info::detect(Devices);
		}

		static bool CreateHandle(FDeviceContext* Context)
		{
			return linux_device_info::create_handle(Context);
		}

		static void InvalidateHandle(FDeviceContext* Context)
		{
			linux_device_info::invalidate_handle(Context);
		}

		static void ProcessAudioHaptic(FDeviceContext* Context)
		{
			linux_device_info::process_audio_haptic(Context);
		}
	};
} // namespace linux_platform
#endif
