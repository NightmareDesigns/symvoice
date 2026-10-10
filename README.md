# symvoice

Symvoice is a Windows-focused, offline speech project built with native C and
Python glue. The goal is to create a voice from recordings you own, use it to
read AI-generated text, and make it available to Windows speech applications.

## First milestone: prepare voice recordings

The initial native utility converts 16-bit PCM WAV recordings with one or two
channels into mono, 16 kHz PCM WAV, a common input format for speech models.
It performs channel mixing and linear sample-rate conversion without third-party
runtime libraries. This is audio preparation only; voice training, speech
generation, microphone capture, and Windows SAPI integration are not implemented
yet.

### Build on Windows

Install CMake and a C compiler such as Visual Studio Build Tools, then run these
commands from the repository directory:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Prepare a recording

```powershell
python python/symvoice.py prepare recording.wav prepared.wav
```

The Python command locates the native executable in `build`. To use a different
build location, pass `--engine path\to\symvoice-audio.exe`.
