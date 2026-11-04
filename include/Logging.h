#pragma once
// The logging macros FreeInk's FrontlightManager expects from the firmware: one line on the serial log per message.
#include <Arduino.h>

#define LOG_ERR(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
