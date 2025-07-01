#pragma once

#include <Arduino.h>

/**
 * @brief Initializes and starts the serial interface task for testing audio commands.
 * This task listens for serial input and sends corresponding commands to the audio player.
 */
void serial_interface_init();
