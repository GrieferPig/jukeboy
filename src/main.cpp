#include <Arduino.h>
#include <driver/i2s_std.h>

#define SR_PL 1
#define SR_CP 3
#define SR_DATA 10

#define WS GPIO_NUM_0
#define DOUT GPIO_NUM_8
#define BCK GPIO_NUM_9

i2s_chan_handle_t tx_handle;
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

void setup()
{
    Serial.begin(115200);
    Serial.println("Hello, World!");

    pinMode(SR_PL, OUTPUT);  // Set PL pin as output
    pinMode(SR_CP, OUTPUT);  // Set CP pin as output
    pinMode(SR_DATA, INPUT); // Set DATA pin as input

    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BCK,
            .ws = WS,
            .dout = DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_channel_init_std_mode(tx_handle, &std_cfg);

    i2s_channel_enable(tx_handle); // Enable the I2S channel
}

void loop()
{
    // Sample the shift register
    digitalWrite(SR_PL, LOW);  // Set PL low to start sampling
    delayMicroseconds(10);     // Wait for 10 microseconds
    digitalWrite(SR_PL, HIGH); // Set PL high to finish sampling

    // Read the data from the shift register
    Serial.print("Shift Register Data: ");
    for (int i = 0; i < 8; i++)
    {
        int bitValue = digitalRead(SR_DATA); // Read each bit from the data pin
        Serial.print(bitValue ? "1" : "0");  // Print 1 or 0 based on the bit value
        digitalWrite(SR_CP, HIGH);           // Pulse the clock to shift the next bit
        delayMicroseconds(10);               // Wait for 10 microseconds to allow the shift register to process
        digitalWrite(SR_CP, LOW);            // Set clock low to prepare for the next bit
    }

    Serial.println(); // Print a newline after reading all bits

    // Generate and write audio data to I2S
    const int sample_count = 256;
    int16_t samples[sample_count * 2]; // Stereo, so 2 samples per frame (16-bit each)
    for (int i = 0; i < sample_count; i++)
    {
        // Generate a simple square wave
        if (i < sample_count / 2)
        {
            samples[i * 2] = 5000;     // Left channel
            samples[i * 2 + 1] = 5000; // Right channel
        }
        else
        {
            samples[i * 2] = -5000;     // Left channel
            samples[i * 2 + 1] = -5000; // Right channel
        }
    }

    size_t bytes_written = 0;
    i2s_channel_write(tx_handle, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
    if (bytes_written < sizeof(samples))
    {
        Serial.printf("I2S write timeout, only wrote %d of %d bytes\n", bytes_written, sizeof(samples));
    }

    delay(1000); // Wait for 1 second before the next read
}