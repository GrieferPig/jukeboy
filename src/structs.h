#pragma once
#include <stdint.h>
#include <esp_err.h>
#include <LittleFS.h>

template <typename T>
class FileCircularBuffer
{
public:
    FileCircularBuffer(const char *file_path, uint8_t capacity);
    esp_err_t write(T data);
    esp_err_t read(T *data);
    esp_err_t getAll(T *buffer, uint8_t *count);
    esp_err_t clear();

    uint8_t getCapacity() const { return _capacity; }
    uint8_t getLevel() const
    {
        if (_is_full)
            return _capacity;
        if (_head >= _tail)
            return _head - _tail;
        return _capacity - (_tail - _head);
    }
    bool isFull() const { return _is_full; }
    bool isEmpty() const { return (!_is_full && (_head == _tail)); }

private:
    const char *_file_path;
    uint8_t _capacity;
    uint8_t _head;
    uint8_t _tail;
    bool _is_full;
};