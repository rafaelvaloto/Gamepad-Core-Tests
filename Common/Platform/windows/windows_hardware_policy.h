// Copyright (c) 2025 Rafael Valoto/Publisher. All rights reserved.
// Created for: GamepadCore - Plugin to support DualSense controller on Windows.
// Planned Release Year: 2025

#pragma once
#ifdef BUILD_GAMEPAD_CORE_TESTS
#include "GCore/Templates/TGenericHardwareInfo.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "windows_device_info.h"
#include <vector>
using namespace GamepadCore;

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
	};

} // namespace windows_platform
#endif
