#include "structs.h"
#include <LittleFS.h>

template <typename T>
FileCircularBuffer<T>::FileCircularBuffer(const char *file_path, uint8_t capacity)
    : _file_path(file_path), _capacity(capacity), _head(0), _tail(0), _is_full(false)
{
    LittleFS.begin();
    clear();
}

template <typename T>
esp_err_t FileCircularBuffer<T>::write(T data)
{
    File f = LittleFS.open(_file_path, "r+");
    if (!f)
        return ESP_FAIL;
    if (!f.seek((size_t)_head * sizeof(T), SeekSet))
    {
        f.close();
        return ESP_FAIL;
    }
    if (f.write((const uint8_t *)&data, sizeof(T)) != sizeof(T))
    {
        f.close();
        return ESP_FAIL;
    }
    f.close();
    if (_is_full)
    {
        _tail = (_tail + 1) % _capacity;
    }
    _head = (_head + 1) % _capacity;
    _is_full = (_head == _tail);
    return ESP_OK;
}

template <typename T>
esp_err_t FileCircularBuffer<T>::read(T *data)
{
    if (isEmpty())
        return ESP_ERR_NOT_FOUND;
    File f = LittleFS.open(_file_path, "r");
    if (!f)
        return ESP_FAIL;
    if (!f.seek((size_t)_tail * sizeof(T), SeekSet))
    {
        f.close();
        return ESP_FAIL;
    }
    if (f.read((uint8_t *)data, sizeof(T)) != sizeof(T))
    {
        f.close();
        return ESP_FAIL;
    }
    f.close();
    _is_full = false;
    _tail = (_tail + 1) % _capacity;
    return ESP_OK;
}

template <typename T>
esp_err_t FileCircularBuffer<T>::getAll(T *buffer, uint8_t *count)
{
    *count = getLevel();
    if (*count == 0)
        return ESP_OK;
    File f = LittleFS.open(_file_path, "r");
    if (!f)
        return ESP_FAIL;
    for (uint8_t i = 0; i < *count; i++)
    {
        size_t pos = ((size_t)(_tail + i) % _capacity) * sizeof(T);
        if (!f.seek(pos, SeekSet))
        {
            f.close();
            return ESP_FAIL;
        }
        if (f.read((uint8_t *)&buffer[i], sizeof(T)) != sizeof(T))
        {
            f.close();
            return ESP_FAIL;
        }
    }
    f.close();
    return ESP_OK;
}

template <typename T>
esp_err_t FileCircularBuffer<T>::clear()
{
    LittleFS.remove(_file_path);
    File f = LittleFS.open(_file_path, "w+");
    if (!f)
        return ESP_FAIL;
    // no need to pre-fill; new writes will expand as needed
    f.close();
    _head = 0;
    _tail = 0;
    _is_full = false;
    return ESP_OK;
}

// make sure cpp is included in build so the templates are available
