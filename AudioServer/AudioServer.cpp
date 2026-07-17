#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <iostream>
#include <vector>
#include <complex>
#include <cmath>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "User32.lib")

#include <pdh.h>
#pragma comment(lib, "pdh.lib")

// Dynamic NVML types
typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

typedef nvmlReturn_t (*fn_nvmlInit)();
typedef nvmlReturn_t (*fn_nvmlShutdown)();
typedef nvmlReturn_t (*fn_nvmlDeviceGetHandleByIndex)(unsigned int index, nvmlDevice_t* device);
typedef nvmlReturn_t (*fn_nvmlDeviceGetUtilizationRates)(nvmlDevice_t device, nvmlUtilization_t* rates);
typedef nvmlReturn_t (*fn_nvmlDeviceGetEncoderUtilization)(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs);
typedef nvmlReturn_t (*fn_nvmlDeviceGetDecoderUtilization)(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs);
typedef nvmlReturn_t (*fn_nvmlDeviceGetTemperature)(nvmlDevice_t device, int sensorType, unsigned int* temp);
typedef nvmlReturn_t (*fn_nvmlDeviceGetMemoryInfo)(nvmlDevice_t device, nvmlMemory_t* memory);

struct PDHTracker {
    PDH_HQUERY cpuQuery = nullptr;
    PDH_HCOUNTER cpuCounter = nullptr;
    bool initialized = false;

    void Init() {
        if (PdhOpenQueryA(nullptr, 0, &cpuQuery) == ERROR_SUCCESS) {
            if (PdhAddCounterA(cpuQuery, "\\Processor(_Total)\\% Processor Time", 0, &cpuCounter) == ERROR_SUCCESS) {
                PdhCollectQueryData(cpuQuery);
                initialized = true;
            }
        }
    }

    float GetCPUUsage() {
        if (!initialized) return 0.0f;
        PDH_FMT_COUNTERVALUE counterVal;
        PdhCollectQueryData(cpuQuery);
        if (PdhGetFormattedCounterValue(cpuCounter, PDH_FMT_DOUBLE, nullptr, &counterVal) == ERROR_SUCCESS) {
            return (float)(counterVal.doubleValue / 100.0);
        }
        return 0.0f;
    }

    ~PDHTracker() {
        if (cpuQuery) {
            PdhCloseQuery(cpuQuery);
        }
    }
};

struct TelemetryData {
    std::atomic<float> cpuUsage{0.0f};
    std::atomic<float> gpuUsage{0.0f};
    std::atomic<float> encUsage{0.0f};
    std::atomic<float> decUsage{0.0f};
    std::atomic<float> gpuTemp{0.0f};
    std::atomic<float> gpuMem{0.0f};
};

TelemetryData g_Telemetry;

const double PI = 3.14159265358979323846;
const int FFT_SIZE = 1024;
const int BARK_BINS = 128;

// Shared circular buffer for audio mixing
struct AudioRingBuffer {
    std::vector<float> buffer;
    int head = 0;
    std::mutex mtx;

    AudioRingBuffer(size_t size = 16384) : buffer(size, 0.0f) {}

    void Push(const float* samples, size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < count; ++i) {
            buffer[head] = samples[i];
            head = (head + 1) % buffer.size();
        }
    }

    void PushMixed(const float* s1, size_t c1, const float* s2, size_t c2) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t count = min(c1, c2);
        for (size_t i = 0; i < count; ++i) {
            buffer[head] = (s1[i] + s2[i]) * 0.5f;
            head = (head + 1) % buffer.size();
        }
    }

    void GetLatest(int count, std::vector<float>& out) {
        std::lock_guard<std::mutex> lock(mtx);
        out.resize(count);
        int start = (head - count + buffer.size()) % buffer.size();
        for (int i = 0; i < count; ++i) {
            out[i] = buffer[(start + i) % buffer.size()];
        }
    }
};

AudioRingBuffer g_AudioBuffer;
std::atomic<bool> g_Running(true);
std::atomic<int> g_SampleRate(44100);

// Fast Cooley-Tukey Radix-2 FFT
void fft(std::vector<std::complex<double>>& a) {
    int n = a.size();
    if (n <= 1) return;
    std::vector<std::complex<double>> a0(n / 2), a1(n / 2);
    for (int i = 0; 2 * i < n; i++) {
        a0[i] = a[2 * i];
        a1[i] = a[2 * i + 1];
    }
    fft(a0);
    fft(a1);
    double angle = 2.0 * PI / n;
    std::complex<double> w(1.0), wn(cos(angle), -sin(angle));
    for (int i = 0; 2 * i < n; i++) {
        a[i] = a0[i] + w * a1[i];
        a[i + n / 2] = a0[i] - w * a1[i];
        w *= wn;
    }
}

// Convert Hz frequency to Bark scale
double hzToBark(double f) {
    return 13.0 * atan(0.00076 * f) + 3.5 * atan(pow(f / 7500.0, 2));
}

// SHA-1 Implementation for WebSocket Handshake
void sha1(const uint8_t* message, size_t message_len, uint8_t* digest) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    size_t new_len = ((message_len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> padded(new_len, 0);
    memcpy(padded.data(), message, message_len);
    padded[message_len] = 0x80;
    
    uint64_t bits_len = (uint64_t)message_len * 8;
    for (int i = 0; i < 8; ++i) {
        padded[new_len - 1 - i] = (uint8_t)(bits_len >> (i * 8));
    }

    auto left_rotate = [](uint32_t value, uint32_t shift) -> uint32_t {
        return (value << shift) | (value >> (32 - shift));
    };

    for (size_t chunk = 0; chunk < new_len; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)padded[chunk + i * 4] << 24) |
                   ((uint32_t)padded[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)padded[chunk + i * 4 + 2] << 8) |
                   ((uint32_t)padded[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = left_rotate(b, 30);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    digest[0] = h0 >> 24; digest[1] = h0 >> 16; digest[2] = h0 >> 8; digest[3] = h0;
    digest[4] = h1 >> 24; digest[5] = h1 >> 16; digest[6] = h1 >> 8; digest[7] = h1;
    digest[8] = h2 >> 24; digest[9] = h2 >> 16; digest[10] = h2 >> 8; digest[11] = h2;
    digest[12] = h3 >> 24; digest[13] = h3 >> 16; digest[14] = h3 >> 8; digest[15] = h3;
    digest[16] = h4 >> 24; digest[17] = h4 >> 16; digest[18] = h4 >> 8; digest[19] = h4;
}

std::string base64_encode(const uint8_t* input, size_t len) {
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t val = (input[i] << 16) + 
                       (i + 1 < len ? (input[i + 1] << 8) : 0) + 
                       (i + 2 < len ? input[i + 2] : 0);
        out.push_back(alphabet[(val >> 18) & 63]);
        out.push_back(alphabet[(val >> 12) & 63]);
        out.push_back(i + 1 < len ? alphabet[(val >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? alphabet[val & 63] : '=');
    }
    return out;
}

// WASAPI Audio Capture Thread
void WASAPICaptureThread(bool useLoopback) {
    CoInitialize(nullptr);
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        std::cerr << "Failed to init COM device enumerator." << std::endl;
        return;
    }

    if (useLoopback) {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    } else {
        hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
    }

    if (FAILED(hr)) {
        std::cerr << "Audio device not found." << std::endl;
        pEnumerator->Release();
        return;
    }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr)) {
        std::cerr << "Failed to activate audio client." << std::endl;
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    WAVEFORMATEX* pwfx = nullptr;
    pAudioClient->GetMixFormat(&pwfx);
    g_SampleRate = pwfx->nSamplesPerSec;

    DWORD flags = useLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, pwfx, nullptr);
    if (FAILED(hr)) {
        std::cerr << "Failed to init audio client." << std::endl;
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    if (FAILED(hr)) {
        std::cerr << "Failed to get capture client." << std::endl;
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start audio client." << std::endl;
    }

    std::cout << (useLoopback ? "[System Audio]" : "[Microphone]") << " Capture thread running. Sample Rate: " << g_SampleRate << " Hz" << std::endl;

    std::vector<float> monoTemp;
    while (g_Running) {
        UINT32 numFramesAvailable;
        hr = pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr)) break;

        while (numFramesAvailable > 0) {
            BYTE* pData;
            UINT32 numFramesToRead;
            DWORD dwFlags;
            hr = pCaptureClient->GetBuffer(&pData, &numFramesToRead, &dwFlags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFramesToRead > 0) {
                monoTemp.resize(numFramesToRead);
                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)pwfx;
                    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                        float* fData = (float*)pData;
                        for (UINT32 f = 0; f < numFramesToRead; ++f) {
                            float sum = 0.0f;
                            for (WORD c = 0; c < pwfx->nChannels; ++c) {
                                sum += fData[f * pwfx->nChannels + c];
                            }
                            monoTemp[f] = sum / pwfx->nChannels;
                        }
                    } else if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                        int16_t* iData = (int16_t*)pData;
                        for (UINT32 f = 0; f < numFramesToRead; ++f) {
                            float sum = 0.0f;
                            for (WORD c = 0; c < pwfx->nChannels; ++c) {
                                sum += (float)iData[f * pwfx->nChannels + c] / 32768.0f;
                            }
                            monoTemp[f] = sum / pwfx->nChannels;
                        }
                    }
                } else if (pwfx->wFormatTag == WAVE_FORMAT_PCM) {
                    if (pwfx->wBitsPerSample == 16) {
                        int16_t* iData = (int16_t*)pData;
                        for (UINT32 f = 0; f < numFramesToRead; ++f) {
                            float sum = 0.0f;
                            for (WORD c = 0; c < pwfx->nChannels; ++c) {
                                sum += (float)iData[f * pwfx->nChannels + c] / 32768.0f;
                            }
                            monoTemp[f] = sum / pwfx->nChannels;
                        }
                    }
                }
                
                if (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(monoTemp.begin(), monoTemp.end(), 0.0f);
                }

                g_AudioBuffer.Push(monoTemp.data(), monoTemp.size());
            }

            pCaptureClient->ReleaseBuffer(numFramesToRead);
            hr = pCaptureClient->GetNextPacketSize(&numFramesAvailable);
            if (FAILED(hr)) break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    pAudioClient->Stop();
    CoTaskMemFree(pwfx);
    pCaptureClient->Release();
    pAudioClient->Release();
    pDevice->Release();
    pEnumerator->Release();
    CoUninitialize();
}

void TelemetryThreadFunc() {
    PDHTracker pdh;
    pdh.Init();

    HMODULE nvmlLib = LoadLibraryA("nvml.dll");
    if (!nvmlLib) {
        nvmlLib = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }

    fn_nvmlInit p_nvmlInit = nullptr;
    fn_nvmlShutdown p_nvmlShutdown = nullptr;
    fn_nvmlDeviceGetHandleByIndex p_nvmlDeviceGetHandleByIndex = nullptr;
    fn_nvmlDeviceGetUtilizationRates p_nvmlDeviceGetUtilizationRates = nullptr;
    fn_nvmlDeviceGetEncoderUtilization p_nvmlDeviceGetEncoderUtilization = nullptr;
    fn_nvmlDeviceGetDecoderUtilization p_nvmlDeviceGetDecoderUtilization = nullptr;
    fn_nvmlDeviceGetTemperature p_nvmlDeviceGetTemperature = nullptr;
    fn_nvmlDeviceGetMemoryInfo p_nvmlDeviceGetMemoryInfo = nullptr;

    nvmlDevice_t nvmlDevice = nullptr;
    bool nvmlInitialized = false;

    if (nvmlLib) {
        p_nvmlInit = (fn_nvmlInit)GetProcAddress(nvmlLib, "nvmlInit");
        p_nvmlShutdown = (fn_nvmlShutdown)GetProcAddress(nvmlLib, "nvmlShutdown");
        p_nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)GetProcAddress(nvmlLib, "nvmlDeviceGetHandleByIndex");
        p_nvmlDeviceGetUtilizationRates = (fn_nvmlDeviceGetUtilizationRates)GetProcAddress(nvmlLib, "nvmlDeviceGetUtilizationRates");
        p_nvmlDeviceGetEncoderUtilization = (fn_nvmlDeviceGetEncoderUtilization)GetProcAddress(nvmlLib, "nvmlDeviceGetEncoderUtilization");
        p_nvmlDeviceGetDecoderUtilization = (fn_nvmlDeviceGetDecoderUtilization)GetProcAddress(nvmlLib, "nvmlDeviceGetDecoderUtilization");
        p_nvmlDeviceGetTemperature = (fn_nvmlDeviceGetTemperature)GetProcAddress(nvmlLib, "nvmlDeviceGetTemperature");
        p_nvmlDeviceGetMemoryInfo = (fn_nvmlDeviceGetMemoryInfo)GetProcAddress(nvmlLib, "nvmlDeviceGetMemoryInfo");

        if (p_nvmlInit && p_nvmlInit() == 0) {
            nvmlInitialized = true;
            if (p_nvmlDeviceGetHandleByIndex) {
                p_nvmlDeviceGetHandleByIndex(0, &nvmlDevice);
            }
            std::cout << "[Telemetry] NVML initialized successfully." << std::endl;
        } else {
            std::cout << "[Telemetry] NVML initialization failed." << std::endl;
        }
    } else {
        std::cout << "[Telemetry] nvml.dll not found. NVIDIA telemetry disabled." << std::endl;
    }

    pdh.GetCPUUsage(); // Baseline call

    while (g_Running) {
        g_Telemetry.cpuUsage = pdh.GetCPUUsage();

        if (nvmlInitialized && nvmlDevice) {
            nvmlUtilization_t rates{};
            if (p_nvmlDeviceGetUtilizationRates && p_nvmlDeviceGetUtilizationRates(nvmlDevice, &rates) == 0) {
                g_Telemetry.gpuUsage = (float)rates.gpu / 100.0f;
            }
            unsigned int encUtil = 0, decUtil = 0, period = 0;
            if (p_nvmlDeviceGetEncoderUtilization && p_nvmlDeviceGetEncoderUtilization(nvmlDevice, &encUtil, &period) == 0) {
                g_Telemetry.encUsage = (float)encUtil / 100.0f;
            }
            if (p_nvmlDeviceGetDecoderUtilization && p_nvmlDeviceGetDecoderUtilization(nvmlDevice, &decUtil, &period) == 0) {
                g_Telemetry.decUsage = (float)decUtil / 100.0f;
            }
            unsigned int temp = 0;
            if (p_nvmlDeviceGetTemperature && p_nvmlDeviceGetTemperature(nvmlDevice, 0, &temp) == 0) {
                g_Telemetry.gpuTemp = (float)temp;
            }
            nvmlMemory_t mem{};
            if (p_nvmlDeviceGetMemoryInfo && p_nvmlDeviceGetMemoryInfo(nvmlDevice, &mem) == 0) {
                if (mem.total > 0) {
                    g_Telemetry.gpuMem = (float)mem.used / (float)mem.total;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (nvmlInitialized && p_nvmlShutdown) {
        p_nvmlShutdown();
    }
    if (nvmlLib) {
        FreeLibrary(nvmlLib);
    }
}

int main() {
    std::cout << "=== NATIVE DSP AUDIO SERVER ===" << std::endl;
    std::cout << "Select Audio Source:\n1. Microphone\n2. System Audio (Loopback)\n3. Both\nChoice (1-3): ";
    int sourceChoice = 1;
    std::cin >> sourceChoice;

    std::cout << "Select Focus:\n1. Normal\n2. Bass-Heavy Focus\nChoice (1-2): ";
    int focusChoice = 1;
    std::cin >> focusChoice;

    std::thread capThread1;
    std::thread capThread2;
    std::thread telemetryThread;

    if (sourceChoice == 1 || sourceChoice == 3) {
        capThread1 = std::thread(WASAPICaptureThread, false);
    }
    if (sourceChoice == 2 || sourceChoice == 3) {
        capThread2 = std::thread(WASAPICaptureThread, true);
    }
    telemetryThread = std::thread(TelemetryThreadFunc);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Winsock initialization failed." << std::endl;
        g_Running = false;
        if (capThread1.joinable()) capThread1.join();
        if (capThread2.joinable()) capThread2.join();
        if (telemetryThread.joinable()) telemetryThread.join();
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(8080);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Socket bind failed." << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        g_Running = false;
        if (capThread1.joinable()) capThread1.join();
        if (capThread2.joinable()) capThread2.join();
        if (telemetryThread.joinable()) telemetryThread.join();
        return 1;
    }

    listen(listenSocket, 1);
    std::cout << "\nWebSocket Server listening on ws://localhost:8080" << std::endl;

    std::vector<float> pcmBuffer;
    std::vector<std::complex<double>> fftInput(FFT_SIZE);
    std::vector<float> barkOutput(BARK_BINS, 0.0f);
    std::vector<float> smoothBark(BARK_BINS, 0.0f);

    while (g_Running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenSocket, &readfds);
        timeval tv{0, 100000};

        int activity = select(0, &readfds, nullptr, nullptr, &tv);
        if (activity > 0) {
            SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
            if (clientSocket != INVALID_SOCKET) {
                std::cout << "\nClient connected." << std::endl;

                char buffer[2048];
                int r = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                if (r > 0) {
                    buffer[r] = '\0';
                    std::string req(buffer);
                    size_t keyPos = req.find("Sec-WebSocket-Key: ");
                    if (keyPos != std::string::npos) {
                        keyPos += 19;
                        size_t keyEnd = req.find("\r\n", keyPos);
                        std::string key = req.substr(keyPos, keyEnd - keyPos);
                        
                        std::string acceptStr = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                        uint8_t digest[20];
                        sha1((const uint8_t*)acceptStr.data(), acceptStr.size(), digest);
                        std::string acceptKey = base64_encode(digest, 20);

                        std::string response = 
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
                        send(clientSocket, response.data(), (int)response.size(), 0);
                        std::cout << "Handshake complete. Streaming DSP data..." << std::endl;

                        while (g_Running) {
                            auto startTime = std::chrono::steady_clock::now();

                            g_AudioBuffer.GetLatest(FFT_SIZE, pcmBuffer);

                            for (int i = 0; i < FFT_SIZE; ++i) {
                                double win = 0.5 - 0.5 * cos(2.0 * PI * i / (FFT_SIZE - 1));
                                fftInput[i] = std::complex<double>(pcmBuffer[i] * win, 0.0);
                            }

                            fft(fftInput);

                            std::fill(barkOutput.begin(), barkOutput.end(), 0.0f);
                            double maxFreq = (double)g_SampleRate / 2.0;
                            double maxBark = hzToBark(maxFreq);

                            for (int i = 0; i < FFT_SIZE / 2; ++i) {
                                double freq = (double)i * g_SampleRate / FFT_SIZE;
                                double bark = hzToBark(freq);
                                double binPos = (bark / maxBark) * (BARK_BINS - 1);
                                
                                int binIdx = (int)binPos;
                                double t = binPos - binIdx;
                                double mag = std::abs(fftInput[i]) / (FFT_SIZE / 2.0);

                                if (binIdx >= 0 && binIdx < BARK_BINS) {
                                    barkOutput[binIdx] += (float)(mag * (1.0 - t));
                                    if (binIdx + 1 < BARK_BINS) {
                                        barkOutput[binIdx + 1] += (float)(mag * t);
                                    }
                                }
                            }

                            for (int i = 0; i < BARK_BINS; ++i) {
                                float weight = 1.0f;
                                if (focusChoice == 2) {
                                    if (i < BARK_BINS * 0.25) {
                                        weight = 2.5f * (1.0f - (float)i / (BARK_BINS * 0.25f));
                                    } else {
                                        weight = 0.4f;
                                    }
                                }
                                barkOutput[i] *= weight;
                                smoothBark[i] = 0.25f * barkOutput[i] + 0.75f * smoothBark[i];
                            }

                            std::vector<float> wsPayload(136, 0.0f);
                            std::copy(smoothBark.begin(), smoothBark.end(), wsPayload.begin());
                            wsPayload[128] = g_Telemetry.cpuUsage.load();
                            wsPayload[129] = g_Telemetry.gpuUsage.load();
                            wsPayload[130] = g_Telemetry.encUsage.load();
                            wsPayload[131] = g_Telemetry.decUsage.load();
                            wsPayload[132] = g_Telemetry.gpuTemp.load();
                            wsPayload[133] = g_Telemetry.gpuMem.load();

                            uint8_t wsHeader[4];
                            wsHeader[0] = 0x82;
                            wsHeader[1] = 126;
                            wsHeader[2] = (544 >> 8) & 0xFF;
                            wsHeader[3] = 544 & 0xFF;

                            int bytesSent = send(clientSocket, (const char*)wsHeader, 4, 0);
                            if (bytesSent == SOCKET_ERROR) break;

                            bytesSent = send(clientSocket, (const char*)wsPayload.data(), 544, 0);
                            if (bytesSent == SOCKET_ERROR) break;

                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
                            if (elapsed < 16) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed));
                            }
                        }
                    }
                }
                closesocket(clientSocket);
                std::cout << "Client disconnected." << std::endl;
            }
        }
    }

    closesocket(listenSocket);
    WSACleanup();

    g_Running = false;
    if (capThread1.joinable()) capThread1.join();
    if (capThread2.joinable()) capThread2.join();
    if (telemetryThread.joinable()) telemetryThread.join();

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}
