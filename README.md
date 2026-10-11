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
requires a microphone that supports this format. Symvoice can produce simple
speech by joining recorded clips; neural voice training and Windows SAPI
integration are not implemented yet.

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

Each sample is normalized to mono 16 kHz PCM and stored under `audio/`. Word
clips have low-level silence trimmed at their edges while retaining a short
margin. The transcripts and relative audio paths are kept in `manifest.json`. Record only
your own voice or audio you have permission to use. This creates a labeled
recording corpus. To create a simple spoken WAV using previously recorded
individual words:

```powershell
python python/symvoice.py dataset word-record my-voice --text "hello"
python python/symvoice.py dataset word-add my-voice another-word.wav --text "world"
python python/symvoice.py dataset speak my-voice response.wav --text "Hello, world!"
```

An AI response can be passed over stdin to avoid quoting arbitrary generated
text in a shell command:

```powershell
Get-Content .\response.txt -Raw | python python/symvoice.py dataset speak my-voice response.wav --text -
```

This first speech output uses the longest matching recorded utterances, then
falls back to individual word clips joined with short silences. It supports up
to 64 words per output and requires recordings that cover all requested words
or phrases. It is not natural continuous TTS or a trained voice model.
Punctuation and prosody are not synthesized; text is matched without regard
to case.

### Connect an AI app over localhost

Start the local WAV speech endpoint after building the native utility:

```powershell
python python/symvoice.py serve my-voice --port 8021
```

The server binds only to `127.0.0.1`. A local client can POST JSON to
`http://127.0.0.1:8021/v1/audio/speech` with an `input` string and receive
`audio/wav`; `model` and `voice` string fields are accepted for common client
compatibility, but voice selection is controlled by the corpus passed to
`serve`. `response_format` may be omitted or set to `wav`. Requests are limited
to 64 KiB of text. For example:

```powershell
curl.exe http://127.0.0.1:8021/v1/audio/speech `
  -H "Content-Type: application/json" `
  -d '{"input":"Hello, world!","response_format":"wav"}' `
  --output response.wav
```

This is a local app integration endpoint, not a registered Windows system
voice. It only speaks text covered by the corpus recordings.

The Python command locates the native executable in `build`. To use a different
build location, pass `--engine path\to\symvoice-audio.exe`.
