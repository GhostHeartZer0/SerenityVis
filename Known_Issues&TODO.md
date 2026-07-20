# Known Issues

### Audio & Telemetry
- **Android System Audio Loopback Restrictions**: Android API 29+ restricts global system audio loopback, requiring explicit app opt-in; fallback relies on device microphone (`MediaRecorder.AudioSource.UNPROCESSED`).
- **Platform Telemetry Dependencies**: Desktop GPU telemetry (NVML / PDH) is Windows-specific and requires abstraction layer for cross-platform target hardware.
- **Spectrum Frequency Imbalance**: Raw bass amplitude dominance can flatten higher spectral bands without auto-compensating relative dynamic mode enabled.

### Visual Issues
- **Visuals tend to 'bleed'**

---

# TODO

### Android Platform Porting
- [ ] Replace WASAPI audio capture with Google Oboe (AAudio / OpenSL ES) backend.
- [ ] Implement JNI bridge to stream C++ DSP byte arrays directly to frontend.
- [ ] Handle Android system audio capture restrictions and microphone fallbacks.
- [ ] Implement frontend via `androidx.webgpu` Jetpack library or hardware-accelerated WebView container.
- [ ] Swap NVENC/AMF video encoding to Android `MediaCodec` API.
- [ ] Replace NVML telemetry with `HardwarePropertiesManager` and `ActivityManager`.
- [ ] Integrate Android `Sustained Performance API` for Heavy Mode shader execution.

### Graphics & Simulation
- [ ] N-Dimensional SDF raymarching shader implementation.
- [ ] Android OpenGL ES sandbox render fallback for legacy mobile hardware.
- [ ] Expand quality presets from 480p24Hz up to 4K120Hz dynamic scaling.

### Core & Hardware
- [ ] Resolve full ASIC dedicated hardware graph targeting and isolated compute queues.
- [ ] Enhance security robustness and policy checks for sub-process execution.
