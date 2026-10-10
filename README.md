# symvoice

Symvoice is a Windows-focused, offline speech project built with native C and
Python glue. The goal is to create a voice from recordings you own, use it to
read AI-generated text, and make it available to Windows speech applications.

## First milestone: prepare voice recordings

The initial native utility converts 16-bit PCM WAV recordings with one or two
channels into mono, 16 kHz PCM WAV, a common input format for speech models.
It performs channel mixing and linear sample-rate conversion without third-party
runtime libraries. On Windows it can also record up to one hour of mono, 16 kHz
audio from the default microphone using the Windows multimedia API. Recording
requires a microphone that supports this format. This is audio preparation only;
voice training, speech generation, and Windows SAPI integration are not implemented
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

### Record your voice

```powershell
python python/symvoice.py record 30 voice.wav
```

The command records for the requested number of seconds (1–3600) and saves a
mono, 16 kHz, 16-bit PCM WAV. It uses the Windows default recording device;
microphone recording is not available on other platforms.

### Play a recording

```powershell
python python/symvoice.py play voice.wav
```

Playback uses the Windows default audio device and accepts valid 16-bit PCM WAV
files with one or two channels. Playback is not available on other platforms.

### Collect labeled voice recordings

Create a local corpus, then record phrases or import your own WAV recordings.
Keep each clip focused on the words in its transcript:

```powershell
python python/symvoice.py dataset init my-voice
python python/symvoice.py dataset record my-voice --text "Hello, this is my voice." --seconds 5
python python/symvoice.py dataset add my-voice existing.wav --text "The exact words spoken."
python python/symvoice.py dataset validate my-voice
python python/symvoice.py dataset list my-voice
```

Each sample is normalized to mono 16 kHz PCM and stored under `audio/`; the
transcripts and relative audio paths are kept in `manifest.json`. Record only
your own voice or audio you have permission to use. This creates a labeled
recording corpus; **it does not train a voice model or generate speech yet**.

The Python command locates the native executable in `build`. To use a different
build location, pass `--engine path\to\symvoice-audio.exe`.
