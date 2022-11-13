#include <stdio.h>
#include <stdint.h>
#include <Windows.h>
#include <immintrin.h>
#include <math.h>

#define USE_SIMD_256            1
#define USE_THREADS             1

#define PAGE_SIZE               4096
#define CACHE_LINE_SIZE         64
#define M_PI                    3.14159265358979323846f

#if DEBUG
#define releaseInline
#else
#define releaseInline            __attribute__((always_inline))
#endif

#define WAVE_FORMAT_PCM         1

#define WAVE_CHANNEL_MONO       1
#define WAVE_CHANNEL_STEREO     2

#define WAVE_MAGIC_RIFF         0x46464952  // "RIFF" LE
#define WAVE_MAGIC_WAVE         0x45564157  // "WAVE" LE
#define WAVE_MAGIC_FMT          0x20746D66  // "fmt " LE
#define WAVE_MAGIC_DATA         0x61746164  // "data" LE
                                            //
// Width used when performing calculations 8xf32
#define SIMD_LANE_COUNT     8
#define SIMD_WIDTH          (SIMD_LANE_COUNT * sizeof(uint16_t))

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

typedef struct Wave {
    uint8_t* fileBuffer;
    uint32_t fileSize;
    float scalingFactor;
    float frequency;
} Wave;

// Samples are hardcoded for now
#define BITS_PER_SAMPLE     16
#define SAMPLE_RATE         44100
#define CHANNEL_COUNT       1
#define DURATION_IN_SEC     60

releaseInline float sinF32(float num) {
    asm("fld DWORD PTR %1     \n"
        "fsin                 \n"
        "fstp DWORD PTR %0    \n"
        : "=m" (num)
        : "m" (num)
    );
    return num;
}

// NOTE: This attribute prevents the compiler from optimizing this function away.
// this is also the reason why we cannot inline this
__attribute__((optnone))
void sinWide(float* num) {
    asm(
        "mov r8, %1                 \n"
        "fld DWORD PTR [r8]         \n"
        "fld DWORD PTR [r8 + 4]     \n"
        "fld DWORD PTR [r8 + 8]     \n"
        "fld DWORD PTR [r8 + 12]    \n"
        "fld DWORD PTR [r8 + 16]    \n"
        "fld DWORD PTR [r8 + 20]    \n"
        "fld DWORD PTR [r8 + 24]    \n"
        "fld DWORD PTR [r8 + 28]    \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 28]   \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 24]   \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 20]   \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 16]   \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 12]   \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 8]    \n"
        "fsin                       \n"
        "fstp DWORD PTR [r8 + 4]    \n"
        "fsin                       \n"
        "fstp  DWORD PTR [r8]       \n"
        : "=r" (num)
        : "r" (num)
        : "r8"
    );
}

releaseInline void generateSamplesSimd256(Wave* wave) {
    uint8_t* dataStart = wave->fileBuffer + sizeof(WaveHeader);

    // Start at the first cacheline after the wave header
    uint8_t* cursor = wave->fileBuffer + CACHE_LINE_SIZE;
    uint32_t cursorDiff = (wave->fileBuffer + CACHE_LINE_SIZE) - dataStart;

    float startIndex = (float)(cursorDiff / sizeof(uint16_t));
    __m256 sampleIndices = {
        startIndex, startIndex + 1.0, startIndex + 2.0, startIndex + 3.0,
        startIndex + 4.0, startIndex + 5.0, startIndex + 6.0, startIndex + 7.0
    };

    float sampleIncrement = SIMD_LANE_COUNT;
    __m256 sampleIndexIncrement = _mm256_broadcast_ss(&sampleIncrement);

    float indexMultiplier = 2.0 * M_PI * wave->frequency;
    __m256 indexMultipliers = _mm256_broadcast_ss(&indexMultiplier);
    
    float sampleRate = SAMPLE_RATE;
    __m256 denominator = _mm256_broadcast_ss(&sampleRate);
    __m256 scalingFactors = _mm256_broadcast_ss(&wave->scalingFactor);

    uint8_t* fileEnd = wave->fileBuffer + wave->fileSize;
    while (cursor + SIMD_WIDTH < fileEnd) {
        __m256 numerator = _mm256_mul_ps(sampleIndices, indexMultipliers);
        __m256 r = _mm256_div_ps(numerator, denominator);
        sinWide((float*)&r);
        __m256 resF32 = _mm256_mul_ps(r, scalingFactors);
        // TODO: disable rounding!
        __m256i resU32 = _mm256_cvtps_epi32(resF32);

        __m128i a = _mm256_extractf128_si256(resU32, 0);
        __m128i b = _mm256_extractf128_si256(resU32, 1);

        asm("movdqa xmm8, %1                \n"
            "vpackssdw xmm7, xmm8, %2       \n"
            "movdqa [%0], xmm7      \n"
            : 
            : "r"(cursor), "m"(a), "m"(b)
            : "xmm8"
        );

        sampleIndices = _mm256_add_ps(sampleIndices, sampleIndexIncrement);
        cursor += SIMD_WIDTH;
    }
}

releaseInline void generateSamples(Wave* wave) {
    uint16_t* cursor = (uint16_t*)(wave->fileBuffer + sizeof(WaveHeader));
    for (uint32_t i = 0; i < SAMPLE_RATE * DURATION_IN_SEC; i++) {
        float a = (float)i * 2.0 * M_PI * wave->frequency;
        float b = sinF32(a / (float)SAMPLE_RATE);
        float sample = wave->scalingFactor * b;
        *cursor = (uint16_t)sample;
        cursor++;
    }
}

struct SamplerJob {
    struct Wave* wave;
    uint32_t workCount;
    uint8_t* outputBuffer;
    uint32_t startIndex;
};

uint32_t samplerThreadMain(void* param) {
    struct SamplerJob* job = param;
    struct Wave* wave = job->wave;
    
    uint32_t threadId = GetCurrentThreadId();
    printf("Hello from thread %u\n", threadId);

    float startIndex = (float)job->startIndex;
    __m256 sampleIndices = {
        startIndex, startIndex + 1.0, startIndex + 2.0, startIndex + 3.0,
        startIndex + 4.0, startIndex + 5.0, startIndex + 6.0, startIndex + 7.0
    };

    float sampleIncrement = SIMD_LANE_COUNT;
    __m256 sampleIndexIncrement = _mm256_broadcast_ss(&sampleIncrement);

    float indexMultiplier = 2.0 * M_PI * wave->frequency;
    __m256 indexMultipliers = _mm256_broadcast_ss(&indexMultiplier);
    
    float sampleRate = SAMPLE_RATE;
    __m256 denominator = _mm256_broadcast_ss(&sampleRate);
    __m256 scalingFactors = _mm256_broadcast_ss(&wave->scalingFactor);

    uint8_t* cursor = job->outputBuffer;
    uint8_t* fileEnd = job->outputBuffer + (job->workCount * 32);
    while (cursor + SIMD_WIDTH < fileEnd) {
        __m256 numerator = _mm256_mul_ps(sampleIndices, indexMultipliers);
        __m256 r = _mm256_div_ps(numerator, denominator);
        sinWide((float*)&r);
        __m256 resF32 = _mm256_mul_ps(r, scalingFactors);
        // TODO: disable rounding!
        __m256i resU32 = _mm256_cvtps_epi32(resF32);

        __m128i a = _mm256_extractf128_si256(resU32, 0);
        __m128i b = _mm256_extractf128_si256(resU32, 1);

        asm("movdqa xmm8, %1                \n"
            "vpackssdw xmm7, xmm8, %2       \n"
            "movdqa [%0], xmm7      \n"
            : 
            : "r"(cursor), "m"(a), "m"(b)
            : "xmm8"
        );

        sampleIndices = _mm256_add_ps(sampleIndices, sampleIndexIncrement);
        cursor += SIMD_WIDTH;
    }

    return 0;
}

int main() {
    uint32_t sampleCount = DURATION_IN_SEC * SAMPLE_RATE * CHANNEL_COUNT;
    uint32_t dataSize = sampleCount * sizeof(uint16_t);

    Wave wave = {
        .fileSize = sizeof(WaveHeader) + dataSize
    };

    printf("File size: %u\n", wave.fileSize);

    wave.fileBuffer = VirtualAlloc(NULL, wave.fileSize, MEM_COMMIT, PAGE_READWRITE);
    if (wave.fileBuffer == NULL) {
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

#if USE_THREADS
    // Initialize threads
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint32_t physicalThreadCount = sysInfo.dwNumberOfProcessors;

#define MAX_THREAD_COUNT    16
    uint32_t threadCount = physicalThreadCount; 
    if (physicalThreadCount > MAX_THREAD_COUNT) {
        threadCount = MAX_THREAD_COUNT;
    }

    struct SamplerJob jobs[MAX_THREAD_COUNT];
    HANDLE threads[MAX_THREAD_COUNT];

    uint8_t* dataStart = wave.fileBuffer + sizeof(WaveHeader);
    uint32_t simdDataSize = wave.fileSize - CACHE_LINE_SIZE;
    // Start at the first cacheline after the wave header
    uint8_t* start = wave.fileBuffer + CACHE_LINE_SIZE;
    // Round down to the last cache line
    uint32_t cacheLineCount = simdDataSize / CACHE_LINE_SIZE;
    uint32_t cacheLinesPerThread = cacheLineCount / threadCount;
    uint32_t lastThreadCacheLineCount = cacheLinesPerThread + (cacheLineCount % threadCount);

    uint8_t* workBufferStart = start;
    uint32_t startIndex = (start - dataStart) / sizeof(uint16_t);
    for (uint32_t i = 0; i < threadCount; i++) {
        struct SamplerJob* job = &jobs[i];
        job->wave = &wave;
        // divide by 2 because work unit is 32 bytes
        job->workCount = cacheLinesPerThread * 2;
        job->outputBuffer = workBufferStart;
        job->startIndex = startIndex;

        if (i + 1 == threadCount) {
            job->workCount = lastThreadCacheLineCount * 2;
        }

        workBufferStart += job->workCount * 32;
        startIndex += (8 * job->workCount);
    }

    for (uint32_t i = 0; i < threadCount; i++) {
        threads[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)samplerThreadMain, &jobs[i], 0, NULL);
        if (threads[i] == NULL) {
            printf("Error: could not create thread (%lu)\n", GetLastError());
            return 1;
        }
    }
#endif

    // Create header
    WaveHeader* header = (WaveHeader*)wave.fileBuffer;
    header->riff = WAVE_MAGIC_RIFF;
    header->fileSize = wave.fileSize - 8;
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

    wave.frequency = 440.0;
    wave.scalingFactor = powf(2.0, (float)(BITS_PER_SAMPLE - 1)) - 1.0;

#if USE_THREADS == 0
#if USE_SIMD_256
    generateSamplesSimd256(&wave);
#else
    generateSamples(&wave);
#endif
#endif

#if USE_THREADS
#define WAIT_MAX_MS     30000
    uint32_t waitOk = WaitForMultipleObjects(threadCount, threads, TRUE, WAIT_MAX_MS);
#endif

    uint32_t bytesWritten = 0;
    BOOL writeOk = WriteFile(outputFile, wave.fileBuffer, wave.fileSize, (LPDWORD)&bytesWritten, NULL);
    if (writeOk == FALSE || bytesWritten != wave.fileSize) {
        printf("Error: could not write to output file (%lu)\n", GetLastError());
        CloseHandle(outputFile);
        return 1;
    }

    CloseHandle(outputFile);
}
