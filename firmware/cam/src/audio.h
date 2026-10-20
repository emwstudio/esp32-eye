#pragma once

#include <stdint.h>

// I2S master-RX (ES7210 is the slave) + WebSocket push task.
// WS endpoint: ws://catcam.local:81/audio  (binary frames)
// Frame layout: [8B uint64 LE timestamp us][640B PCM, 16 kHz/16-bit/mono]
// ES7210 is configured for standard stereo I2S (MIC1->L, MIC2->R); the two
// mics are down-mixed to mono by averaging L/R (see audioTask).
void audioInit();  // install I2S driver (channel disabled until a client connects)
void audioStart(); // start WS server + FreeRTOS tasks
int audioClientCount();
