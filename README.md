# symvoice

Symvoice is a Windows-focused, offline speech project built with native C and
Python glue. The goal is to create a voice from recordings you own, use it to
read AI-generated text, and make it available to Windows speech applications.

## Current capabilities

The initial native utility converts 16-bit PCM WAV recordings with one or two
channels into mono, 16 kHz PCM WAV, a common input format for speech models.
It performs channel mixing and linear sample-rate conversion without third-party
runtime libraries. On Windows it can also record up to one hour of mono, 16 kHz
audio from the default microphone using the Windows multimedia API. Recording
requires a microphone that supports this format. Symvoice can produce simple
speech by joining recorded clips and includes an experimental SAPI 5 bridge.
This is not neural voice training or unrestricted natural TTS.

## Download a Windows PC test bundle

The **Windows build and PC test bundle** workflow builds/tests both x64 and x86
variants on Windows and uploads ZIP artifacts. After a push or workflow run,
open the repository's **Actions** tab, select the latest successful workflow,
and download `symvoice-windows-x64` (recommended for 64-bit apps) or
`symvoice-windows-x86`. Artifacts are retained for 14 days. Alternatively, run
the workflow manually using **Run workflow**.

Extract the ZIP to a writable folder on your Windows PC. Install Python 3 if
needed; the bundle contains the built C executable and SAPI DLL, but not Python.
Open PowerShell in the extracted folder.

1. Create a corpus and record a short phrase. Speak the exact phrase during
   the entire recording; the initial recorder captures the specified duration:

   ```powershell
   python .\python\symvoice.py dataset init my-voice
   python .\python\symvoice.py dataset record my-voice --text "Hello, this is my voice." --seconds 5
   python .\python\symvoice.py dataset validate my-voice
   ```

2. In a first PowerShell window, start the local synthesis server and leave it
   running:

   ```powershell
   python .\python\symvoice.py serve my-voice --port 8021
   ```

3. In a second PowerShell window, register the matching architecture for your
   Windows user:

   ```powershell
   .\scripts\Register-Symvoice.ps1 -DllPath .\build\Release\symvoice-sapi.dll
   ```

4. Test it through Windows SAPI. Use a phrase recorded in the corpus:

   ```powershell
   .\scripts\Test-Symvoice-Sapi.ps1 -Text "Hello, this is my voice."
   ```

5. When finished, remove the per-user registration:

   ```powershell
   .\scripts\Unregister-Symvoice.ps1
   ```

The voice server must stay running while SAPI uses Symvoice. x64 DLLs are
visible to 64-bit apps and x86 DLLs to 32-bit apps. Apps using non-SAPI speech
APIs will not use this voice. The workflow verifies compilation and automated
tests; microphone, audio playback, and live SAPI behavior still need a manual
test on your PC.

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

This is the speech backend used by the optional Windows SAPI engine below. It
only speaks text covered by the corpus recordings.

### Register as a Windows SAPI voice

The Windows build also creates `symvoice-sapi.dll`, a native SAPI 5 COM engine.
Build with the matching architecture for the Windows apps that will use it
(typically x64), and register it for your user without administrator rights:

```powershell
cmake -S . -B build
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File .\scripts\Register-Symvoice.ps1 `
  -DllPath .\build\Release\symvoice-sapi.dll
python .\python\symvoice.py serve my-voice --port 8021
```

Leave the speech server running before selecting **Symvoice** in a SAPI 5
application or Windows' legacy Speech Properties. The engine sends spoken text
to the local server and streams its WAV samples back to SAPI. If the server is
stopped, SAPI synthesis fails rather than falling back to another service.
Remove the per-user registration with:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Unregister-Symvoice.ps1
```

This integration targets SAPI 5 applications; apps using Windows' newer
`Windows.Media.SpeechSynthesis` API or their own TTS engines do not automatically
use SAPI voices. SAPI DLL activation has not been tested on Windows in this
environment.

The Python command locates the native executable in `build`. To use a different
build location, pass `--engine path\to\symvoice-audio.exe`.
