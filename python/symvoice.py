import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import uuid
import wave
from pathlib import Path


SAMPLE_RATE = 16000
MANIFEST_NAME = "manifest.json"
WORD_PATTERN = re.compile(r"[^\W_]+(?:['’][^\W_]+)*", re.UNICODE)


def find_engine():
    root = Path(__file__).resolve().parents[1]
    executable = "symvoice-audio.exe" if sys.platform == "win32" else "symvoice-audio"
    candidates = (
        root / "build" / executable,
        root / "build" / "Release" / executable,
        root / "build" / "Debug" / executable,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def read_manifest(dataset):
    path = dataset / MANIFEST_NAME
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read corpus manifest: {error}") from error
    if (not isinstance(manifest, dict)
            or manifest.get("format_version") != 1
            or not isinstance(manifest.get("samples"), list)):
        raise ValueError("corpus manifest has an unsupported or invalid format")
    return manifest


def sample_info(path):
    try:
        with wave.open(str(path), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, SAMPLE_RATE):
                raise ValueError(f"{path} is not mono 16-bit 16 kHz PCM WAV")
            frames = audio.getnframes()
            if frames == 0:
                raise ValueError(f"{path} contains no audio")
            return frames / SAMPLE_RATE
    except (OSError, EOFError, wave.Error) as error:
        raise ValueError(f"cannot read WAV file {path}: {error}") from error


def checked_sample_path(dataset, entry):
    if not isinstance(entry, dict) or not isinstance(entry.get("audio"), str):
        raise ValueError("corpus manifest contains an invalid sample entry")
    relative = Path(entry["audio"])
    if relative.is_absolute() or len(relative.parts) != 2 or relative.parts[0] != "audio":
        raise ValueError("corpus manifest contains an unsafe audio path")
    return dataset / relative


def text_words(text):
    return WORD_PATTERN.findall(text.casefold())


def write_manifest(dataset, manifest):
    descriptor, temporary_name = tempfile.mkstemp(prefix=".manifest-", suffix=".tmp", dir=dataset)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            json.dump(manifest, output, ensure_ascii=False, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, dataset / MANIFEST_NAME)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def init_dataset(dataset):
    dataset.mkdir(parents=True, exist_ok=True)
    (dataset / "audio").mkdir(exist_ok=True)
    manifest_path = dataset / MANIFEST_NAME
    if manifest_path.exists():
        raise ValueError(f"corpus already exists: {dataset}")
    write_manifest(dataset, {"format_version": 1, "sample_rate": SAMPLE_RATE, "samples": []})
    print(f"Initialized voice corpus at {dataset}")


def add_sample(dataset, input_path, text, engine, kind="utterance"):
    transcript = text.strip()
    if not transcript:
        raise ValueError("sample text must not be empty")
    if kind not in ("utterance", "word"):
        raise ValueError("unknown voice sample type")
    if kind == "word" and len(text_words(transcript)) != 1:
        raise ValueError("a word recording must contain exactly one word")
    dataset = dataset.resolve()
    manifest = read_manifest(dataset)
    audio_dir = dataset / "audio"
    if not audio_dir.is_dir():
        raise ValueError("corpus audio directory is missing")
    if kind == "word":
        target_word = text_words(transcript)[0]
        for entry in manifest["samples"]:
            if not isinstance(entry, dict):
                raise ValueError("corpus manifest contains an invalid sample entry")
            if entry.get("kind") == "word" and text_words(entry.get("text", "")) == [target_word]:
                raise ValueError(f"word is already recorded in this corpus: {target_word}")

    sample_id = uuid.uuid4().hex
    final_path = audio_dir / f"{sample_id}.wav"
    descriptor, temporary_name = tempfile.mkstemp(prefix=".sample-", suffix=".wav", dir=audio_dir)
    os.close(descriptor)
    temporary = Path(temporary_name)
    trim_temporary = None
    try:
        subprocess.run([str(engine), "prepare", str(input_path), str(temporary)], check=True)
        if kind == "word":
            trim_descriptor, trim_name = tempfile.mkstemp(
                prefix=".trimmed-", suffix=".wav", dir=audio_dir)
            os.close(trim_descriptor)
            trim_temporary = Path(trim_name)
            subprocess.run(
                [str(engine), "trim", str(temporary), str(trim_temporary)], check=True)
            os.replace(trim_temporary, temporary)
        duration = sample_info(temporary)
        os.replace(temporary, final_path)
        manifest["samples"].append({
            "id": sample_id,
            "audio": f"audio/{sample_id}.wav",
            "text": transcript,
            "kind": kind,
            "duration_seconds": round(duration, 6),
        })
        try:
            write_manifest(dataset, manifest)
        except BaseException:
            final_path.unlink(missing_ok=True)
            raise
    except subprocess.CalledProcessError as error:
        temporary.unlink(missing_ok=True)
        raise ValueError(f"audio preparation failed with exit code {error.returncode}") from error
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    finally:
        if trim_temporary is not None:
            trim_temporary.unlink(missing_ok=True)
    print(f"Added {final_path.name} ({duration:.2f}s): {transcript}")


def record_sample(dataset, seconds, text, engine, kind="utterance"):
    dataset = dataset.resolve()
    read_manifest(dataset)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".recording-", suffix=".wav", dir=dataset)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        print(f"Say this phrase after recording starts: {text}", flush=True)
        subprocess.run([str(engine), "record", str(seconds), str(temporary)], check=True)
        add_sample(dataset, temporary, text, engine, kind=kind)
    except subprocess.CalledProcessError as error:
        raise ValueError(f"microphone recording failed with exit code {error.returncode}") from error
    finally:
        temporary.unlink(missing_ok=True)


def list_dataset(dataset):
    manifest = read_manifest(dataset)
    for entry in manifest["samples"]:
        path = checked_sample_path(dataset, entry)
        text = entry.get("text")
        sample_id = entry.get("id")
        if not isinstance(text, str) or not isinstance(sample_id, str):
            raise ValueError("corpus manifest contains an invalid sample entry")
        print(f"{sample_id}  {text}  {path}")
    print(f"{len(manifest['samples'])} sample(s)")


def validate_dataset(dataset):
    manifest = read_manifest(dataset)
    if manifest.get("sample_rate") != SAMPLE_RATE:
        raise ValueError(f"corpus sample rate must be {SAMPLE_RATE} Hz")
    identifiers = set()
    for entry in manifest["samples"]:
        path = checked_sample_path(dataset, entry)
        sample_id = entry.get("id")
        text = entry.get("text")
        if (not isinstance(sample_id, str) or not sample_id
                or sample_id in identifiers or not isinstance(text, str) or not text.strip()):
            raise ValueError("corpus manifest contains a missing or duplicate ID or empty transcript")
        kind = entry.get("kind", "utterance")
        if kind not in ("utterance", "word") or (kind == "word" and len(text_words(text)) != 1):
            raise ValueError("corpus manifest contains an invalid sample type or word transcript")
        identifiers.add(sample_id)
        sample_info(path)
    print(f"Corpus is valid: {len(identifiers)} sample(s)")


def synthesize_text(dataset, output_path, text, engine):
    words = text_words(text)
    if not words:
        raise ValueError("text must contain at least one word")
    if len(words) > 64:
        raise ValueError("this initial synthesizer supports up to 64 words per utterance")

    dataset = dataset.resolve()
    manifest = read_manifest(dataset)
    lexicon = {}
    phrases = []
    for entry in manifest["samples"]:
        if not isinstance(entry, dict):
            raise ValueError("corpus manifest contains an invalid sample entry")
        tokens = text_words(entry.get("text", ""))
        if entry.get("kind", "utterance") == "utterance":
            if tokens:
                phrases.append((tokens, checked_sample_path(dataset, entry)))
        elif entry.get("kind") == "word":
            if len(tokens) != 1:
                raise ValueError("corpus contains an invalid word recording")
            if tokens[0] in lexicon:
                raise ValueError(f"corpus has duplicate recordings for: {tokens[0]}")
            lexicon[tokens[0]] = checked_sample_path(dataset, entry)

    phrases.sort(key=lambda phrase: len(phrase[0]), reverse=True)
    selected_paths = []
    missing = set()
    position = 0
    while position < len(words):
        phrase = next(
            ((phrase_words, path) for phrase_words, path in phrases
             if words[position:position + len(phrase_words)] == phrase_words),
            None,
        )
        if phrase is not None:
            phrase_words, path = phrase
            selected_paths.append(path)
            position += len(phrase_words)
            continue
        word = words[position]
        if word in lexicon:
            selected_paths.append(lexicon[word])
        else:
            missing.add(word)
        position += 1
    if missing:
        raise ValueError("record these words or phrases first: " + ", ".join(sorted(missing)))

    if not selected_paths:
        raise ValueError("no matching recorded audio was found")
    destination = output_path.resolve()
    if any(path.resolve() == destination for path in selected_paths):
        raise ValueError("synthesis output must not overwrite a recorded sample")
    command = [str(engine), "synthesize", str(output_path)]
    command.extend(str(path) for path in selected_paths)
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        raise ValueError(f"native synthesis failed with exit code {error.returncode}") from error
    print(f"Created speech WAV: {output_path}")


def main():
    parser = argparse.ArgumentParser(prog="symvoice")
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare", help="convert a voice recording to mono 16 kHz PCM WAV")
    prepare.add_argument("input", type=Path)
    prepare.add_argument("output", type=Path)
    prepare.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    record = commands.add_parser("record", help="record a mono 16 kHz WAV from a Windows microphone")
    record.add_argument("seconds", type=int)
    record.add_argument("output", type=Path)
    record.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    play = commands.add_parser("play", help="play a WAV recording through the Windows default audio device")
    play.add_argument("input", type=Path)
    play.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    dataset = commands.add_parser("dataset", help="manage labeled recordings for voice-model training")
    dataset_commands = dataset.add_subparsers(dest="dataset_command", required=True)
    initialize = dataset_commands.add_parser("init", help="create an empty voice corpus")
    initialize.add_argument("directory", type=Path)
    add = dataset_commands.add_parser("add", help="normalize and label an existing WAV recording")
    add.add_argument("directory", type=Path)
    add.add_argument("input", type=Path)
    add.add_argument("--text", required=True, help="exact words spoken in the recording")
    add.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    capture = dataset_commands.add_parser("record", help="record and label a microphone utterance")
    capture.add_argument("directory", type=Path)
    capture.add_argument("--text", required=True, help="words to speak during this recording")
    capture.add_argument("--seconds", type=int, default=5, help="recording length from 1 to 3600 seconds")
    capture.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    listing = dataset_commands.add_parser("list", help="list voice corpus recordings")
    listing.add_argument("directory", type=Path)
    validate = dataset_commands.add_parser("validate", help="check corpus metadata and WAV files")
    validate.add_argument("directory", type=Path)
    word_add = dataset_commands.add_parser("word-add", help="add one normalized word recording to the voice lexicon")
    word_add.add_argument("directory", type=Path)
    word_add.add_argument("input", type=Path)
    word_add.add_argument("--text", required=True, help="single word spoken in the recording")
    word_add.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    word_record = dataset_commands.add_parser("word-record", help="record one word for the voice lexicon")
    word_record.add_argument("directory", type=Path)
    word_record.add_argument("--text", required=True, help="single word to speak")
    word_record.add_argument("--seconds", type=int, default=2, help="recording length from 1 to 3600 seconds")
    word_record.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    speak = dataset_commands.add_parser("speak", help="synthesize text from recorded word samples")
    speak.add_argument("directory", type=Path)
    speak.add_argument("output", type=Path)
    speak.add_argument("--text", required=True, help="text using only words in this corpus")
    speak.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    args = parser.parse_args()

    try:
        if args.command == "dataset" and args.dataset_command == "init":
            init_dataset(args.directory)
            return 0
        if args.command == "dataset" and args.dataset_command == "list":
            list_dataset(args.directory)
            return 0
        if args.command == "dataset" and args.dataset_command == "validate":
            validate_dataset(args.directory)
            return 0
        if args.command == "dataset" and args.dataset_command == "word-add":
            engine = args.engine or find_engine()
            if engine is None or not engine.is_file():
                parser.error("native engine not found; build it with CMake or pass --engine")
            add_sample(args.directory, args.input, args.text, engine, kind="word")
            return 0
        if args.command == "dataset" and args.dataset_command == "speak":
            engine = args.engine or find_engine()
            if engine is None or not engine.is_file():
                parser.error("native engine not found; build it with CMake or pass --engine")
            synthesize_text(args.directory, args.output, args.text, engine)
            return 0

        engine = args.engine or find_engine()
        if engine is None or not engine.is_file():
            parser.error("native engine not found; build it with CMake or pass --engine")
        if args.command == "prepare":
            command = [str(engine), "prepare", str(args.input), str(args.output)]
        elif args.command == "record":
            command = [str(engine), "record", str(args.seconds), str(args.output)]
        elif args.command == "play":
            command = [str(engine), "play", str(args.input)]
        elif args.command == "dataset" and args.dataset_command == "add":
            add_sample(args.directory, args.input, args.text, engine)
            return 0
        elif args.command == "dataset" and args.dataset_command == "record":
            if args.seconds < 1 or args.seconds > 3600:
                raise ValueError("recording duration must be between 1 and 3600 seconds")
            if not args.text.strip():
                raise ValueError("sample text must not be empty")
            record_sample(args.directory, args.seconds, args.text, engine)
            return 0
        else:
            if args.seconds < 1 or args.seconds > 3600:
                raise ValueError("recording duration must be between 1 and 3600 seconds")
            if len(text_words(args.text)) != 1:
                raise ValueError("a word recording must contain exactly one word")
            record_sample(args.directory, args.seconds, args.text, engine, kind="word")
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
