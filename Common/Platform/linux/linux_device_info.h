// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
#pragma once
#ifdef BUILD_GAMEPAD_CORE_TESTS

#include "GCore/Types/Structs/Context/DeviceContext.h"
#include <memory>
#include <vector>

class linux_device_info
{
public:
	virtual ~linux_device_info() = default;
	static void process_audio_haptic(FDeviceContext* Context);
	static bool configure_features(FDeviceContext* Context);
	static void read(FDeviceContext* Context);
	static void write(FDeviceContext* Context);
	static void detect(std::vector<FDeviceContext>& Devices);
	static bool create_handle(FDeviceContext* Context);
	static void invalidate_handle(FDeviceContext* Context);
};
#endif
