#include "audio_player.h"
#include <Arduino.h>
// #include <BackgroundAudio.h> // Assuming BackgroundAudioRAW.h is the one to use
#include <BackgroundAudioRAW.h> // Use the new RAW audio class
#include <SD.h>
#include <vector>
#include <string.h>
#include <ESP32I2SAudio.h>
#include <algorithm> // For std::min and std::max
#include "storage_struct.h"
#include "esp_log.h" // Added for ESP_LOGx macros

// SD Card SPI pins
const int _MISO_AUDIO = 0; // AKA SPI RX
const int _MOSI_AUDIO = 2; // AKA SPI TX
const int _CS_AUDIO = 3;
const int _SCK_AUDIO = 1;

ESP32I2SAudio audio(7, 9, 10);      // BCLK, LRCLK, DOUT (,MCLK)
BackgroundAudioRAWClass BMP(audio); // Use BackgroundAudioRAWClass

static const char *TAG = "AudioPlayer"; // Added for ESP_LOGx

static const uint32_t TJA_HEADER_SIZE = 512;
static const uint32_t ADPCM_BLOCK_SIZE = 44032;
static const uint32_t ADPCM_BLOCK_HEADER_SIZE = 6; // Dual-state header
static const uint32_t ADPCM_BLOCK_DATA_SIZE = ADPCM_BLOCK_SIZE - ADPCM_BLOCK_HEADER_SIZE;
static const uint32_t SAMPLES_PER_BLOCK = ADPCM_BLOCK_DATA_SIZE * 2;

int currentTrackIndex = -1;                                   // Index in rawlist of the *currently playing* track
float current_gain_level = 0.05f;                             // Store current gain level
static uint32_t currentStreamStartFileOffsetBytes = 0;        // Start offset of the current stream segment in the file
bool isPaused = false;                                        // State for pause/resume
static uint32_t pausedFilePositionBytes = 0;                  // Store file position when paused
static PlaybackMode currentPlaybackMode = PLAYBACK_MODE_LOOP; // Default to loop mode

static TjaHeader current_track_header;
static uint8_t adpcm_block_buffer[ADPCM_BLOCK_SIZE];
// This buffer holds one fully decoded ADPCM block's worth of PCM data
static const size_t DECODED_PCM_BUFFER_SIZE = SAMPLES_PER_BLOCK * BYTES_PER_SAMPLE_FRAME;
static uint8_t decoded_pcm_buffer[DECODED_PCM_BUFFER_SIZE];
static size_t decoded_pcm_buffer_read_pos = DECODED_PCM_BUFFER_SIZE; // Start at max to force a read

static int total_track_count = 0; // Replaces rawlist.size()

// New variables for enhanced shuffle mode
std::vector<int> shuffled_indices_list; // Stores indices from rawlist in shuffled order
int current_shuffled_list_play_idx = 0; // Points to the current track *within* shuffled_indices_list

static const uint32_t FIXED_SAMPLE_RATE = 44100;
static const uint32_t FIXED_CHANNELS = 2;
static const uint32_t FIXED_BIT_DEPTH = 16;
static const uint32_t BYTES_PER_SAMPLE_FRAME = FIXED_CHANNELS * (FIXED_BIT_DEPTH / 8); // 2 * (16/8) = 4

// Disk constants
static const uint32_t DISK_SECTOR_SIZE = 512;

// The file we're currently playing
File f_audio;
// Buffer size matches BackgroundAudioRAWClass's internal framelen in bytes
// framelen = SECTOR_SIZE (512 samples) => 512 * FIXED_CHANNELS * (FIXED_BIT_DEPTH / 8) bytes
// 512 * 2 * 2 = 2048 bytes
const size_t bufferSizeBytes = 2048; // SECTOR_SIZE * FIXED_CHANNELS * (FIXED_BIT_DEPTH / 8)
uint8_t rawAudioBuffer[bufferSizeBytes];
// int16_t *stereoSamples = (int16_t *)rawAudioBuffer; // Not directly used if BMP handles gain internally

void generate_shuffled_playlist(int firstTrackRawIndex = -1)
{
    ESP_LOGI(TAG, "Generating shuffled playlist. Requested first track raw index: %d", firstTrackRawIndex);
    shuffled_indices_list.clear();

    if (rawlist.empty())
    {
        current_shuffled_list_play_idx = 0;
        ESP_LOGI(TAG, "Rawlist is empty, shuffle list cleared.");
        return;
    }

    std::vector<int> temp_indices;
    for (int i = 0; i < rawlist.size(); ++i)
    {
        if (i != firstTrackRawIndex)
        { // Exclude the first track if specified and valid
            temp_indices.push_back(i);
        }
    }

    // Fisher-Yates shuffle for temp_indices
    // randomSeed() should be called elsewhere (e.g. in setup()) for better randomness
    for (size_t i = temp_indices.size() - 1; i > 0; --i)
    {
        size_t j = random(i + 1); // random(N) gives 0 to N-1
        std::swap(temp_indices[i], temp_indices[j]);
    }

    // Add the designated first track if it's valid
    if (firstTrackRawIndex >= 0 && firstTrackRawIndex < rawlist.size())
    {
        shuffled_indices_list.push_back(firstTrackRawIndex);
    }

    // Add the shuffled remaining tracks
    for (int index : temp_indices)
    {
        shuffled_indices_list.push_back(index);
    }

    // If firstTrackRawIndex was invalid or not provided, and rawlist is not empty,
    // shuffled_indices_list will contain all tracks in a random order.
    // If rawlist has only one item, shuffled_indices_list will contain just that item's index.

    current_shuffled_list_play_idx = 0; // Start playing from the beginning of the new shuffled list

    // Log the generated shuffled playlist
    if (!shuffled_indices_list.empty())
    {
        char buffer[256];
        int offset = snprintf(buffer, sizeof(buffer), "Generated shuffled playlist. Order (rawlist indices): ");
        const int max_items_to_log = 20; // Limit logged items
        for (size_t i = 0; i < shuffled_indices_list.size() && i < max_items_to_log; ++i)
        {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d ", shuffled_indices_list[i]);
            if (offset >= sizeof(buffer) - 5)
            { // Check space for more numbers and ellipsis
                snprintf(buffer + offset, sizeof(buffer) - offset, "...");
                break;
            }
        }
        if (shuffled_indices_list.size() > max_items_to_log && offset < sizeof(buffer) - 4)
        {
            snprintf(buffer + offset, sizeof(buffer) - offset, "...");
        }
        ESP_LOGI(TAG, "%s", buffer);
    }
    else
    {
        ESP_LOGI(TAG, "Generated shuffled playlist is empty (though rawlist was not).");
    }

    if (shuffled_indices_list.empty() && !rawlist.empty())
    {
        ESP_LOGW(TAG, "Warning - shuffle list is empty but rawlist is not. This might indicate an issue.");
        // Fallback: populate with a simple sequential order if shuffle failed unexpectedly
        // This case should ideally not be hit with the current logic.
        for (int i = 0; i < rawlist.size(); ++i)
            shuffled_indices_list.push_back(i);
        current_shuffled_list_play_idx = 0;
    }
}

void scanDirectory(const char *dirname)
{
    File root = SD.open(dirname);
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s", dirname);
        return;
    }
    if (!root.isDirectory())
    {
        ESP_LOGE(TAG, "%s is not a directory", dirname);
        root.close();
        return;
    }

    File currentFile;
    while (true)
    {
        currentFile = root.openNextFile();
        if (!currentFile)
        {
            break;
        }
        String n = currentFile.name();
        String path = dirname;
        path += currentFile.name();

        if (currentFile.isDirectory())
        {
            if (currentFile.name()[0] == '.')
            {
                currentFile.close();
                continue;
            }
            String sub = dirname;
            sub += currentFile.name();
            sub += "/";
            scanDirectory(sub.c_str());
        }
        else
        {
            String nameLower = n;
            nameLower.toLowerCase();
            if (strstr(nameLower.c_str(), ".tja")) // Look for .raw files
            {
                rawlist.push_back(path); // Add to rawlist
                ESP_LOGI(TAG, "ADD: %s", path.c_str());
            }
            else
            {
                ESP_LOGD(TAG, "SKP: %s", path.c_str());
            }
        }
        currentFile.close();
    }
    root.close();
}

// Define the audio command queue
QueueHandle_t audioCommandQueue;

// --- Implementation of public interface functions ---
void audio_play_next()
{
    AudioCommand cmd;
    cmd.type = CMD_PLAY_NEXT;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_play_previous()
{
    AudioCommand cmd;
    cmd.type = CMD_PLAY_PREVIOUS;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_restart_track()
{
    AudioCommand cmd;
    cmd.type = CMD_RESTART_TRACK;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_jump_to_position_seconds(uint32_t seconds)
{
    AudioCommand cmd;
    cmd.type = CMD_JUMP_TO_POSITION_SECONDS;
    cmd.params.seconds = seconds;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_fast_forward_seconds(uint32_t seconds)
{
    AudioCommand cmd;
    cmd.type = CMD_FAST_FORWARD_SECONDS;
    cmd.params.seconds = seconds;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_rewind_seconds(uint32_t seconds)
{
    AudioCommand cmd;
    cmd.type = CMD_REWIND_SECONDS;
    cmd.params.seconds = seconds;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_set_gain(float gain)
{
    AudioCommand cmd;
    cmd.type = CMD_SET_GAIN;
    cmd.params.gain_value = std::max(0.0f, std::min(gain, 0.2f)); // Clamp gain
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_toggle_pause()
{
    AudioCommand cmd;
    cmd.type = CMD_TOGGLE_PAUSE;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_get_current_track_info()
{
    AudioCommand cmd;
    cmd.type = CMD_GET_CURRENT_TRACK_INFO;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_get_playback_status()
{
    AudioCommand cmd;
    cmd.type = CMD_GET_PLAYBACK_STATUS;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}

void audio_set_playback_mode(PlaybackMode mode)
{
    AudioCommand cmd;
    cmd.type = CMD_SET_PLAYBACK_MODE;
    cmd.params.mode = mode;
    xQueueSend(audioCommandQueue, &cmd, portMAX_DELAY);
}
// --- End of public interface functions ---

void playTrack(int trackIndexInRawlist, uint32_t fileStartOffsetBytes = 0)
{
    // Stop existing playback and I2S output. BMP.end() calls _out->end().
    BMP.end();

    if (f_audio)
    {
        f_audio.close(); // Always close before opening/reopening
    }

    if (rawlist.empty() || trackIndexInRawlist < 0 || trackIndexInRawlist >= rawlist.size())
    {
        ESP_LOGW(TAG, "Invalid track index (%d) or empty playlist for playTrack. Rawlist size: %d", trackIndexInRawlist, rawlist.size());
        currentTrackIndex = -1;
        currentStreamStartFileOffsetBytes = 0;
        isPaused = false; // Reset pause state
        // Clear shuffle state if playlist is truly unusable
        if (rawlist.empty())
        {
            shuffled_indices_list.clear();
            current_shuffled_list_play_idx = 0;
        }
        return;
    }

    ESP_LOGI(TAG, "Opening track (rawlist index %d): %s at offset %lu", trackIndexInRawlist, rawlist[trackIndexInRawlist].c_str(), fileStartOffsetBytes);
    f_audio = SD.open(rawlist[trackIndexInRawlist].c_str());
    if (!f_audio)
    {
        ESP_LOGE(TAG, "Failed to open track: %s", rawlist[trackIndexInRawlist].c_str());
        currentTrackIndex = -1; // Mark no track playing
        currentStreamStartFileOffsetBytes = 0;
        isPaused = false;
        return;
    }

    uint32_t fileSize = f_audio.size();
    if (fileStartOffsetBytes > 0)
    {
        if (fileStartOffsetBytes >= fileSize && fileSize > 0)
        {
            ESP_LOGW(TAG, "Start offset %lu is at or beyond EOF (%lu). Will effectively be end of track.", fileStartOffsetBytes, fileSize);
            fileStartOffsetBytes = fileSize;
        }
        if (!f_audio.seek(fileStartOffsetBytes))
        {
            ESP_LOGW(TAG, "Failed to seek to offset %lu in %s. Playing from beginning.", fileStartOffsetBytes, rawlist[trackIndexInRawlist].c_str());
            fileStartOffsetBytes = 0; // Reset to 0 for clarity, will seek to 0 next
            if (!f_audio.seek(0))
            {
                ESP_LOGE(TAG, "Failed to seek to beginning of %s. Closing file.", rawlist[trackIndexInRawlist].c_str());
                f_audio.close();
                currentTrackIndex = -1;
                currentStreamStartFileOffsetBytes = 0;
                isPaused = false;
                return;
            }
        }
    }
    currentStreamStartFileOffsetBytes = f_audio.position();

    currentTrackIndex = trackIndexInRawlist; // Update global currentTrackIndex (index in rawlist)

    // Synchronize shuffle playlist pointer if in shuffle mode
    if (currentPlaybackMode == PLAYBACK_MODE_SHUFFLE)
    {
        if (rawlist.empty())
        { // Should have been caught earlier
            shuffled_indices_list.clear();
            current_shuffled_list_play_idx = 0;
        }
        else if (shuffled_indices_list.empty())
        {
            ESP_LOGI(TAG, "Shuffle list empty, regenerating with current track first.");
            generate_shuffled_playlist(currentTrackIndex);
            // current_shuffled_list_play_idx is set to 0 by generate_shuffled_playlist
        }
        else
        {
            bool foundInShuffle = false;
            for (int i = 0; i < shuffled_indices_list.size(); ++i)
            {
                if (shuffled_indices_list[i] == currentTrackIndex)
                {
                    current_shuffled_list_play_idx = i;
                    foundInShuffle = true;
                    break;
                }
            }
            if (!foundInShuffle)
            {
                ESP_LOGW(TAG, "Track %d not in current shuffle. Regenerating shuffle with this track first.", currentTrackIndex);
                generate_shuffled_playlist(currentTrackIndex);
                // current_shuffled_list_play_idx will be 0.
            }
        }
    }

    BMP.setGain(current_gain_level);
    BMP.begin();

    isPaused = false;

    uint32_t totalSamplesInEntireFile = (fileSize > 0 && BYTES_PER_SAMPLE_FRAME > 0) ? fileSize / BYTES_PER_SAMPLE_FRAME : 0;
    ESP_LOGI(TAG, "Now playing: %s (Rawlist Index: %d, File Pos: %lu bytes, %lu total samples in file)",
             rawlist[currentTrackIndex].c_str(), currentTrackIndex, currentStreamStartFileOffsetBytes, totalSamplesInEntireFile);
}

// Task for handling audio playback
void audioPlayerTask(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting");

    // Create the command queue
    audioCommandQueue = xQueueCreate(10, sizeof(AudioCommand));
    if (audioCommandQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create command queue! Restarting ESP...");
        delay(1000);
        ESP.restart();
    }

    // Initialize SD card
    bool sdInitialized = false;
    SPI.begin(_SCK_AUDIO, _MISO_AUDIO, _MOSI_AUDIO, _CS_AUDIO);
    sdInitialized = SD.begin(_CS_AUDIO);

    if (!sdInitialized)
    {
        ESP_LOGE(TAG, "SD Card initialization failed! Restarting ESP...");
        delay(1000); // Short delay before restart
        ESP.restart();
    }
    ESP_LOGI(TAG, "SD Card initialized.");

    scanDirectory("/");
    ESP_LOGI(TAG, "Found %d RAW files.", rawlist.size());

    if (!rawlist.empty())
    {
        if (currentPlaybackMode == PLAYBACK_MODE_SHUFFLE)
        {
            generate_shuffled_playlist(-1); // Initial shuffle, -1 means fully random start
            if (!shuffled_indices_list.empty())
            {
                // current_shuffled_list_play_idx is already 0 from generate_shuffled_playlist
                playTrack(shuffled_indices_list[current_shuffled_list_play_idx], 0);
            }
            else
            {
                ESP_LOGW(TAG, "Shuffle list empty after generation, no track to play.");
                currentTrackIndex = -1;
            }
        }
        else
        {                          // PLAYBACK_MODE_LOOP or other default
            currentTrackIndex = 0; // Prepare to play the first track
            playTrack(currentTrackIndex, 0);
        }
    }
    else
    {
        currentTrackIndex = -1; // No tracks available
        ESP_LOGI(TAG, "No tracks found to play initially.");
    }
    ESP_LOGI(TAG, "Background audio player task running.");

    for (;;)
    {
        AudioCommand cmd;
        // Wait for a command. If playing, use a short timeout to also service the audio buffer.
        // If not playing (no f_audio or paused), can wait longer.
        TickType_t queueWaitTime = (f_audio && !isPaused) ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(100);

        if (xQueueReceive(audioCommandQueue, &cmd, queueWaitTime) == pdPASS)
        {
            switch (cmd.type)
            {
            case CMD_PLAY_NEXT:
            {
                ESP_LOGD(TAG, "Received CMD_PLAY_NEXT");
                if (rawlist.empty())
                {
                    ESP_LOGW(TAG, "Playlist is empty.");
                    break;
                }

                int nextRawTrackToPlay = -1;
                if (currentPlaybackMode == PLAYBACK_MODE_LOOP)
                {
                    if (currentTrackIndex == -1 && !rawlist.empty())
                    { // If nothing was playing
                        nextRawTrackToPlay = 0;
                    }
                    else if (currentTrackIndex != -1)
                    {
                        nextRawTrackToPlay = (currentTrackIndex + 1) % rawlist.size();
                    }
                }
                else
                { // PLAYBACK_MODE_SHUFFLE
                    if (shuffled_indices_list.empty() && !rawlist.empty())
                    {
                        // This implies shuffle list wasn't initialized or rawlist changed. Regenerate.
                        // Pass currentTrackIndex, or -1 if none was definitively playing.
                        generate_shuffled_playlist(currentTrackIndex);
                    }

                    if (!shuffled_indices_list.empty())
                    {
                        current_shuffled_list_play_idx++;
                        if (current_shuffled_list_play_idx >= shuffled_indices_list.size())
                        {
                            // End of current shuffle sequence, re-shuffle.
                            // Pass -1 to generate a fully random new list, not making the current track first.
                            generate_shuffled_playlist(-1);
                            // current_shuffled_list_play_idx is reset to 0 by generate_shuffled_playlist
                        }
                        if (!shuffled_indices_list.empty())
                        { // Check again after potential regeneration
                            nextRawTrackToPlay = shuffled_indices_list[current_shuffled_list_play_idx];
                        }
                        else if (!rawlist.empty())
                        { // Shuffle list empty but rawlist not - fallback
                            ESP_LOGW(TAG, "Shuffle list empty after trying to get next, playing first raw track.");
                            nextRawTrackToPlay = 0; // Fallback to rawlist[0]
                        }
                    }
                    else if (!rawlist.empty())
                    { // Shuffle list is empty, but rawlist is not. Try to play first raw track.
                        ESP_LOGW(TAG, "Shuffle list empty, attempting to play first raw track.");
                        nextRawTrackToPlay = 0; // Fallback
                    }
                }

                if (nextRawTrackToPlay != -1)
                {
                    playTrack(nextRawTrackToPlay, 0);
                }
                else if (!rawlist.empty())
                {
                    ESP_LOGW(TAG, "Could not determine next track, but playlist not empty. Restarting first track.");
                    playTrack(0, 0); // Fallback if logic failed but tracks exist
                }
                else
                {
                    ESP_LOGI(TAG, "No next track to play.");
                }
                break;
            }
            case CMD_PLAY_PREVIOUS:
            {
                ESP_LOGD(TAG, "Received CMD_PLAY_PREVIOUS");
                if (rawlist.empty())
                {
                    ESP_LOGW(TAG, "Playlist is empty.");
                    break;
                }

                int prevRawTrackToPlay = -1;
                if (currentPlaybackMode == PLAYBACK_MODE_LOOP)
                {
                    if (currentTrackIndex == -1 && !rawlist.empty())
                    {                                            // If nothing was playing
                        prevRawTrackToPlay = rawlist.size() - 1; // Play last track
                    }
                    else if (currentTrackIndex != -1)
                    {
                        prevRawTrackToPlay = currentTrackIndex - 1;
                        if (prevRawTrackToPlay < 0)
                        {
                            prevRawTrackToPlay = rawlist.size() - 1; // Wrap around
                        }
                    }
                }
                else
                { // PLAYBACK_MODE_SHUFFLE
                    if (shuffled_indices_list.empty() && !rawlist.empty())
                    {
                        generate_shuffled_playlist(currentTrackIndex);
                    }

                    if (!shuffled_indices_list.empty())
                    {
                        current_shuffled_list_play_idx--;
                        if (current_shuffled_list_play_idx < 0)
                        {
                            // Wrap around to the end of the *current* shuffled list
                            current_shuffled_list_play_idx = shuffled_indices_list.size() - 1;
                            if (current_shuffled_list_play_idx < 0)
                                current_shuffled_list_play_idx = 0; // Handle if list became size 0
                        }
                        prevRawTrackToPlay = shuffled_indices_list[current_shuffled_list_play_idx];
                    }
                    else if (!rawlist.empty())
                    {
                        ESP_LOGW(TAG, "Shuffle list empty, attempting to play last raw track for previous.");
                        prevRawTrackToPlay = rawlist.size() - 1; // Fallback
                    }
                }

                if (prevRawTrackToPlay != -1)
                {
                    playTrack(prevRawTrackToPlay, 0);
                }
                else if (!rawlist.empty())
                {
                    ESP_LOGW(TAG, "Could not determine previous track, but playlist not empty. Restarting last track.");
                    playTrack(rawlist.size() - 1, 0); // Fallback
                }
                else
                {
                    ESP_LOGI(TAG, "No previous track to play.");
                }
                break;
            }

            case CMD_RESTART_TRACK:
                ESP_LOGD(TAG, "Received CMD_RESTART_TRACK");
                if (currentTrackIndex != -1)
                {
                    playTrack(currentTrackIndex, 0); // playTrack handles shuffle list sync
                }
                else
                {
                    ESP_LOGI(TAG, "No track to restart.");
                }
                break;

            case CMD_JUMP_TO_POSITION_SECONDS:
                ESP_LOGD(TAG, "Received CMD_JUMP_TO_POSITION_SECONDS (%u s)", cmd.params.seconds);
                if (f_audio && currentTrackIndex != -1)
                {
                    uint32_t bytesPerSecond = FIXED_SAMPLE_RATE * BYTES_PER_SAMPLE_FRAME;
                    uint32_t targetOffset = cmd.params.seconds * bytesPerSecond;
                    uint32_t fileSize = f_audio.size();

                    if (targetOffset >= fileSize && fileSize > 0)
                    {
                        targetOffset = fileSize; // Jump to end (will result in end of track)
                    }
                    else if (fileSize == 0)
                    {
                        targetOffset = 0;
                    }
                    targetOffset = (targetOffset / BYTES_PER_SAMPLE_FRAME) * BYTES_PER_SAMPLE_FRAME; // Align to sample frame
                    playTrack(currentTrackIndex, targetOffset);
                }
                else
                {
                    ESP_LOGI(TAG, "No track playing to jump position.");
                }
                break;

            case CMD_FAST_FORWARD_SECONDS:
                ESP_LOGD(TAG, "Received CMD_FAST_FORWARD_SECONDS (%u s)", cmd.params.seconds);
                if (f_audio && currentTrackIndex != -1)
                {
                    uint32_t bytesPerSecond = FIXED_SAMPLE_RATE * BYTES_PER_SAMPLE_FRAME;
                    uint32_t currentFilePos = f_audio.position(); // Position of next read
                    uint32_t jumpBytes = cmd.params.seconds * bytesPerSecond;
                    uint32_t targetOffset = currentFilePos + jumpBytes;
                    uint32_t fileSize = f_audio.size();

                    if (targetOffset >= fileSize && fileSize > 0)
                    {
                        targetOffset = fileSize; // Jump to end
                    }
                    else if (fileSize == 0)
                    {
                        targetOffset = 0;
                    }
                    targetOffset = (targetOffset / BYTES_PER_SAMPLE_FRAME) * BYTES_PER_SAMPLE_FRAME;
                    playTrack(currentTrackIndex, targetOffset);
                }
                else
                {
                    ESP_LOGI(TAG, "No track playing to fast forward.");
                }
                break;

            case CMD_REWIND_SECONDS:
                ESP_LOGD(TAG, "Received CMD_REWIND_SECONDS (%u s)", cmd.params.seconds);
                if (f_audio && currentTrackIndex != -1)
                {
                    uint32_t bytesPerSecond = FIXED_SAMPLE_RATE * BYTES_PER_SAMPLE_FRAME;
                    uint32_t currentFilePos = f_audio.position();
                    uint32_t rewindBytes = cmd.params.seconds * bytesPerSecond;
                    uint32_t targetOffset;

                    if (rewindBytes >= currentFilePos)
                    {
                        targetOffset = 0; // Rewind to beginning
                    }
                    else
                    {
                        targetOffset = currentFilePos - rewindBytes;
                    }
                    targetOffset = (targetOffset / BYTES_PER_SAMPLE_FRAME) * BYTES_PER_SAMPLE_FRAME;
                    playTrack(currentTrackIndex, targetOffset);
                }
                else
                {
                    ESP_LOGI(TAG, "No track playing to rewind.");
                }
                break;

            case CMD_SET_GAIN:
                current_gain_level = cmd.params.gain_value; // Already clamped by sender
                BMP.setGain(current_gain_level);
                ESP_LOGI(TAG, "Set gain to %f", current_gain_level);
                break;

            case CMD_SET_PLAYBACK_MODE:
            {
                PlaybackMode newMode = cmd.params.mode;
                if (newMode == currentPlaybackMode && ((newMode == PLAYBACK_MODE_SHUFFLE && !shuffled_indices_list.empty()) || newMode == PLAYBACK_MODE_LOOP))
                { // also check if shuffle list is valid if switching to shuffle
                    ESP_LOGI(TAG, "Playback mode already %s", (currentPlaybackMode == PLAYBACK_MODE_LOOP) ? "LOOP" : "SHUFFLE");
                    break;
                }

                currentPlaybackMode = newMode;
                ESP_LOGI(TAG, "Set playback mode to %s", (currentPlaybackMode == PLAYBACK_MODE_LOOP) ? "LOOP" : "SHUFFLE");

                if (currentPlaybackMode == PLAYBACK_MODE_SHUFFLE)
                {
                    // currentTrackIndex should be valid if a track was playing or selected
                    // generate_shuffled_playlist will make currentTrackIndex the first if it's valid.
                    // If currentTrackIndex is -1 (no track playing), it generates a fully random list.
                    generate_shuffled_playlist(currentTrackIndex);
                    // current_shuffled_list_play_idx is set to 0 by generate_shuffled_playlist.
                    // The currently playing track (if any) is now the first in shuffled_indices_list.
                    // No need to call playTrack here, as the current song continues.
                    // The *next* song will follow the new shuffle order.
                    // If no track was playing and list generated, next play command will pick from shuffle.
                }
                else
                { // Switched to PLAYBACK_MODE_LOOP
                  // shuffled_indices_list is no longer actively used for track selection.
                  // currentTrackIndex remains the index of the currently playing song in rawlist.
                  // Next/Prev will use rawlist directly.
                  // No need to clear shuffled_indices_list, it will be overwritten if switching back to shuffle.
                }
            }
            break;

            case CMD_TOGGLE_PAUSE:
                if (currentTrackIndex == -1 || !f_audio && !isPaused) // If not playing and not already paused, nothing to do
                {
                    ESP_LOGI(TAG, "No track playing to toggle pause.");
                    break;
                }

                isPaused = !isPaused;
                if (isPaused) // Just changed to paused state
                {
                    if (f_audio) // Ensure f_audio is valid before using
                    {
                        pausedFilePositionBytes = f_audio.position();
                        f_audio.close(); // Close the file
                    }
                    else
                    {
                        // This case should ideally not be hit if currentTrackIndex != -1
                        // but as a fallback, ensure pausedFilePositionBytes is reasonable if f_audio was already closed.
                        // If playTrack was called before and failed to open, f_audio might be null.
                        // In such a scenario, resuming might try to use an old/invalid pausedFilePositionBytes.
                        // However, playTrack on resume will handle file opening failures.
                    }
                    BMP.end(); // Stop I2S DMA and flush buffers
                    ESP_LOGI(TAG, "Playback paused at position %lu bytes.", pausedFilePositionBytes);
                }
                else // Just changed to playing state (resumed)
                {
                    ESP_LOGI(TAG, "Resuming playback...");
                    // playTrack will handle reopening the file, seeking, and starting BMP
                    // It also sets isPaused = false internally.
                    if (currentTrackIndex != -1)
                    {
                        playTrack(currentTrackIndex, pausedFilePositionBytes);
                    }
                    else
                    {
                        // Should not happen if we were paused, but handle defensively
                        ESP_LOGW(TAG, "Cannot resume, no current track index.");
                        isPaused = true; // Revert to paused state as resume failed
                    }
                }
                break;

            case CMD_GET_CURRENT_TRACK_INFO:
                ESP_LOGD(TAG, "Received CMD_GET_CURRENT_TRACK_INFO");
                if (currentTrackIndex != -1 && f_audio && currentTrackIndex < rawlist.size()) // Added boundary check
                {
                    String trackName = rawlist[currentTrackIndex];
                    uint32_t fileSize = f_audio.size();
                    uint32_t bytesPerSecond = FIXED_SAMPLE_RATE * BYTES_PER_SAMPLE_FRAME;
                    uint32_t totalDurationSec = (bytesPerSecond > 0) ? fileSize / bytesPerSecond : 0;

                    // f_audio.position() is where the *next* read will occur.
                    uint32_t currentPosBytes = f_audio.position();
                    uint32_t currentPosSec = (bytesPerSecond > 0) ? currentPosBytes / bytesPerSecond : 0;

                    ESP_LOGI(TAG, "  Track: %s", trackName.c_str());
                    ESP_LOGI(TAG, "  Position: %lu s / %lu s (%lu bytes / %lu bytes)", currentPosSec, totalDurationSec, currentPosBytes, fileSize);
                    ESP_LOGI(TAG, "  File Offset for this stream segment start: %lu bytes", currentStreamStartFileOffsetBytes);
                    ESP_LOGI(TAG, "  Is Paused: %s", isPaused ? "Yes" : "No");
                }
                else
                {
                    ESP_LOGI(TAG, "  No track currently playing or loaded.");
                }
                break;

            case CMD_GET_PLAYBACK_STATUS:
                ESP_LOGD(TAG, "Received CMD_GET_PLAYBACK_STATUS");
                ESP_LOGI(TAG, "  Status: %s", (currentTrackIndex != -1 && f_audio) ? (isPaused ? "Paused" : "Playing") : "Stopped");
                if (currentTrackIndex != -1 && !rawlist.empty() && currentTrackIndex < rawlist.size()) // Added boundary check
                {
                    ESP_LOGI(TAG, "  Current Track: %s (Rawlist Index: %d)", rawlist[currentTrackIndex].c_str(), currentTrackIndex);
                    if (currentPlaybackMode == PLAYBACK_MODE_SHUFFLE && !shuffled_indices_list.empty())
                    {
                        ESP_LOGI(TAG, "  Shuffle Playlist Pos: %d / %d", current_shuffled_list_play_idx, shuffled_indices_list.empty() ? -1 : (int)shuffled_indices_list.size() - 1);
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "  Current Track: None");
                }
                ESP_LOGI(TAG, "  Gain: %.2f", current_gain_level);
                ESP_LOGI(TAG, "  Playlist Size: %d tracks", rawlist.size());
                ESP_LOGI(TAG, "  Playback Mode: %s", (currentPlaybackMode == PLAYBACK_MODE_LOOP) ? "Loop" : "Shuffle");
                break;

            default:
                ESP_LOGW(TAG, "Received unknown command");
                break;
            }
        }

        // Data feeding logic
        if (f_audio && !isPaused)
        {
            // Original logic: if (BMP.availableForWrite() > 8 && f_audio)
            // This means if more than half of the 16 pending buffers are free, start filling.
            if (BMP.availableForWrite() > 8)
            {
                while (BMP.availableForWrite() > 0) // Try to fill all available frames
                {
                    digitalWrite(8, LOW); // Turn on LED to indicate audio processing
                    size_t bytesRead = f_audio.read(rawAudioBuffer, bufferSizeBytes);
                    digitalWrite(8, HIGH); // Turn off LED after reading
                    if (bytesRead > 0)
                    {
                        BMP.write(rawAudioBuffer, bytesRead); // Write the raw audio data to the player
                    }
                    else
                    {
                        // End of file reached
                        ESP_LOGI(TAG, "End of track %s (rawlist index %d) reached.", rawlist[currentTrackIndex].c_str(), currentTrackIndex);
                        BMP.end();       // Stop I2S
                        f_audio.close(); // Close the current file

                        currentStreamStartFileOffsetBytes = 0; // Reset for the next track

                        if (rawlist.empty())
                        {
                            ESP_LOGI(TAG, "Playlist empty after track end, stopping playback.");
                            currentTrackIndex = -1;
                            shuffled_indices_list.clear();
                            current_shuffled_list_play_idx = 0;
                            break;
                        }

                        int nextRawTrackToPlayAtEnd = -1;
                        // currentTrackIndex holds the index of the track that just finished.

                        if (currentPlaybackMode == PLAYBACK_MODE_LOOP)
                        {
                            if (currentTrackIndex != -1)
                            { // Should always be true here
                                nextRawTrackToPlayAtEnd = (currentTrackIndex + 1) % rawlist.size();
                            }
                            else
                            { // Should not happen, but as a fallback
                                nextRawTrackToPlayAtEnd = 0;
                            }
                        }
                        else
                        { // PLAYBACK_MODE_SHUFFLE
                            // current_shuffled_list_play_idx points to the track that just finished.
                            // So, we increment to get the next one for the new list.
                            if (shuffled_indices_list.empty() && !rawlist.empty())
                            {
                                // Should be populated, but regenerate if not.
                                // currentTrackIndex is the one that just finished.
                                // Pass -1 to avoid making the just-finished track first if the list was unexpectedly empty.
                                generate_shuffled_playlist(-1);
                            }

                            if (!shuffled_indices_list.empty())
                            {
                                current_shuffled_list_play_idx++;
                                if (current_shuffled_list_play_idx >= shuffled_indices_list.size())
                                {
                                    // Reached end of shuffled list, regenerate.
                                    // Pass -1 to generate a fully random new list, not making the just-finished track first.
                                    generate_shuffled_playlist(-1);
                                    // current_shuffled_list_play_idx is reset to 0 by generate_shuffled_playlist
                                }
                                if (!shuffled_indices_list.empty())
                                { // Check again after potential regeneration
                                    nextRawTrackToPlayAtEnd = shuffled_indices_list[current_shuffled_list_play_idx];
                                }
                                else if (!rawlist.empty())
                                {
                                    ESP_LOGW(TAG, "Shuffle list empty after EOT regeneration, playing rawlist[0].");
                                    nextRawTrackToPlayAtEnd = 0; // Fallback
                                }
                            }
                            else if (!rawlist.empty())
                            { // Shuffle list is empty, but rawlist is not.
                                ESP_LOGW(TAG, "Shuffle list empty at EOT, playing rawlist[0].");
                                nextRawTrackToPlayAtEnd = 0; // Fallback
                            }
                        }

                        if (nextRawTrackToPlayAtEnd != -1)
                        {
                            ESP_LOGI(TAG, "Auto-playing next track (rawlist index: %d)", nextRawTrackToPlayAtEnd);
                            playTrack(nextRawTrackToPlayAtEnd, 0);
                        }
                        else
                        {
                            ESP_LOGI(TAG, "No next track to play at EOT, stopping.");
                            currentTrackIndex = -1;
                        }
                        break;
                    }
                }
            }
        }
        // If no command was received and no data was fed (or couldn't be fed),
        // the queueWaitTime provides a delay. If a command was processed or data fed,
        // the loop continues. No explicit vTaskDelay needed here beyond queueWaitTime.
    }
}
