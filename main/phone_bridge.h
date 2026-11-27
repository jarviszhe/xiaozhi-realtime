#pragma once

#include "audio_service.h"

class PhoneBridge {
public:
    explicit PhoneBridge(AudioService& audio) : audio_(audio) {}
    void Start();

private:
    static void Task(void* arg);
    void Run();
#if CONFIG_PHONE_BRIDGE_UART
    void RunUart();
#endif
    void RunUdp();
    AudioService& audio_;
    TaskHandle_t task_handle_ = nullptr;
};
