#include <stdio.h>
#include <stdint.h>
#include <Windows.h>
#include <math.h>

#define PAGE_SIZE               4096
#define M_PI                    3.14159265358979323846f

#define WAVE_FORMAT_PCM         1

#define WAVE_CHANNEL_MONO       1
#define WAVE_CHANNEL_STEREO     2

#define WAVE_MAGIC_RIFF         0x46464952  // "RIFF" LE
#define WAVE_MAGIC_WAVE         0x45564157  // "WAVE" LE
#define WAVE_MAGIC_FMT          0x20746D66  // "fmt " LE
#define WAVE_MAGIC_DATA         0x61746164  // "data" LE

// Combined headers
typedef struct WaveHeader {
    uint32_t    riff;
    uint32_t    fileSize;
    uint32_t    wave;
    uint32_t    fmt;
    uint32_t    fmtSize;
    uint16_t    formatType;
    uint16_t    numberOfChannels;
    uint32_t    sampleRate;
    uint32_t    byteRate;
    uint16_t    blockAlign;
    uint16_t    bitsPerSample;
    uint32_t    dataHeader;
    uint32_t    dataSize;
} WaveHeader;

// Samples are hardcoded for now
#define BITS_PER_SAMPLE     16
#define SAMPLE_RATE         44100
#define CHANNEL_COUNT       1
#define DURATION_IN_SEC     10

int main() {
    uint32_t sampleCount = DURATION_IN_SEC * SAMPLE_RATE * CHANNEL_COUNT;
    uint32_t dataSize = (sampleCount * sizeof(uint16_t));
    uint32_t fileSize = sizeof(WaveHeader) + dataSize;

    printf("File size: %u\n", fileSize);

    void* fileBuffer = VirtualAlloc(NULL, fileSize, MEM_COMMIT, PAGE_READWRITE);
    if (fileBuffer == NULL) {
        printf("Error: could not allocate memory (%lu)\n", GetLastError());
        return 1;
    }

    void* outputFile = CreateFileW(
        L"AudioFile.wav",
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (outputFile == INVALID_HANDLE_VALUE) {
        printf("Error: could not create output file (%lu)\n", GetLastError());
        return 1;
    }

    // Create header
    WaveHeader* header = (WaveHeader*)fileBuffer;
    header->riff = WAVE_MAGIC_RIFF;
    header->fileSize = fileSize - 8;
    header->wave = WAVE_MAGIC_WAVE;
    header->fmt = WAVE_MAGIC_FMT;
    header->fmtSize = 16;
    header->formatType = WAVE_FORMAT_PCM;
    header->numberOfChannels = CHANNEL_COUNT;
    header->sampleRate = SAMPLE_RATE;
    header->byteRate = (SAMPLE_RATE * BITS_PER_SAMPLE * CHANNEL_COUNT) / 8;
    header->blockAlign = (CHANNEL_COUNT * BITS_PER_SAMPLE) / 8;
    header->bitsPerSample = BITS_PER_SAMPLE;
    header->dataHeader = WAVE_MAGIC_DATA;
    header->dataSize = dataSize;

    {
        uint16_t* cursor = (uint16_t*)(fileBuffer + sizeof(WaveHeader));

        float freq = 440.0;
        float scalingFactor = powf(2.0, (float)(BITS_PER_SAMPLE - 1)) - 1.0;
        for (uint32_t i = 0; i < SAMPLE_RATE * DURATION_IN_SEC; i++) {
            float sample = scalingFactor * sinf(((float)i * 2.0 * M_PI * freq) / (float)SAMPLE_RATE);
            *cursor = (uint16_t)sample;
            cursor++;
        }
    }

    uint32_t bytesWritten = 0;
    BOOL writeOk = WriteFile(outputFile, fileBuffer, fileSize, (LPDWORD)&bytesWritten, NULL);
    if (writeOk == FALSE || bytesWritten != fileSize) {
        printf("Error: could not write to output file (%lu)\n", GetLastError());
        CloseHandle(outputFile);
        return 1;
    }

    CloseHandle(outputFile);
}
