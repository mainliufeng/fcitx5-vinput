#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser(
    description="Run via dbus-run-session: isolated real PipeWire capture and D-Bus result checks. Does not change the microphone, default sink, or daily daemon."
)
parser.add_argument("corpus", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--daemon", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
corpus = args.corpus.resolve()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=True)
config_dir = root / "config/vinput"
config_dir.mkdir(parents=True, exist_ok=True)
data_dir = root / "data/vinput"
data_dir.mkdir(parents=True, exist_ok=True)
models = data_dir / "models"
if not models.exists():
    models.symlink_to(
        Path.home() / ".local/share/vinput/models", target_is_directory=True
    )
config = json.loads((repo / "data/default-config.json").read_text())
config["global"]["capture_device"] = "vinput_quality_probe.monitor"
config["global"]["duck_output_while_recording"] = False
config["asr"]["providers"][0].update(
    model="model.sherpa-onnx.x-asr-960ms-streaming-zipformer-transducer-zh-en-punct-int8",
    refine_model="model.sherpa-onnx.x-asr-zipformer-transducer-zh-en-punct-int8",
)
(config_dir / "config.json").write_text(json.dumps(config))
env = dict(
    os.environ, XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data")
)
log = open(root / "daemon.log", "w")
daemon = None
module = None
monitor = None
base = [
    "gdbus",
    "call",
    "--session",
    "--dest",
    "org.fcitx.Vinput",
    "--object-path",
    "/org/fcitx/Vinput",
    "--method",
]


def call(method, *args):
    return subprocess.run(
        base + ["org.fcitx.Vinput.Service." + method, *args],
        capture_output=True,
        text=True,
        timeout=10,
    )


try:
    module = subprocess.check_output(
        [
            "pactl",
            "load-module",
            "module-null-sink",
            "sink_name=vinput_quality_probe",
            "rate=16000",
            "channels=1",
        ],
        text=True,
    ).strip()
    daemon = subprocess.Popen(
        [str(args.daemon.resolve() if args.daemon else repo / "build/src/daemon/vinput-daemon")], env=env, stdout=log, stderr=log
    )
    for _ in range(80):
        owner = subprocess.run(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                "org.freedesktop.DBus",
                "--object-path",
                "/org/freedesktop/DBus",
                "--method",
                "org.freedesktop.DBus.NameHasOwner",
                "org.fcitx.Vinput",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if "true" in owner.stdout:
            break
        if daemon.poll() is not None:
            raise RuntimeError("daemon exited")
        time.sleep(0.1)
    else:
        raise RuntimeError("daemon did not register")
    owner_pid = subprocess.check_output(
        [
            "gdbus",
            "call",
            "--session",
            "--dest",
            "org.freedesktop.DBus",
            "--object-path",
            "/org/freedesktop/DBus",
            "--method",
            "org.freedesktop.DBus.GetConnectionUnixProcessID",
            "org.fcitx.Vinput",
        ],
        text=True,
    )
    if int(re.search(r'uint32 (\d+)', owner_pid).group(1)) != daemon.pid:
        raise RuntimeError("unexpected D-Bus owner")
    monitor_log = open(root / "signals.log", "w")
    monitor = subprocess.Popen(
        [
            "gdbus",
            "monitor",
            "--session",
            "--dest",
            "org.fcitx.Vinput",
            "--object-path",
            "/org/fcitx/Vinput",
        ],
        stdout=monitor_log,
        stderr=monitor_log,
    )
    time.sleep(0.3)
    evidence = []
    for name, path in [
        ("speech", str(corpus / "fleurs-cmn_hans_cn-validation-1.wav")),
        ("white-no-speech", str(corpus / "white-no-speech.wav")),
        ("train-no-speech", str(corpus / "train-16k.wav")),
    ]:
        start = call("StartRecording")
        if start.returncode:
            raise RuntimeError(start.stderr)
        time.sleep(0.3)
        subprocess.run(
            ["pw-play", "--target", "vinput_quality_probe", path],
            check=True,
            timeout=30,
        )
        time.sleep(0.25)
        stop = call("StopRecording", "__raw__")
        if stop.returncode:
            raise RuntimeError(stop.stderr)
        for _ in range(100):
            status = call("GetStatus")
            if "idle" in status.stdout:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("recognition did not finish")
        evidence.append(
            dict(
                case=name,
                start=start.stdout.strip(),
                stop=stop.stdout.strip(),
                final_status=status.stdout.strip(),
            )
        )
    monitor.terminate()
    monitor.wait(timeout=5)
    monitor = None
    monitor_log.close()
    import ast

    messages = []
    for line in (root / "signals.log").read_text().splitlines():
        marker = "org.fcitx.Vinput.Service.RecognitionResult "
        if marker in line:
            messages.append(json.loads(ast.literal_eval(line.split(marker, 1)[1])[0]))
    if (
        len(messages) != 3
        or not messages[0]["commit_text"]
        or any(m["commit_text"] for m in messages[1:])
    ):
        raise RuntimeError("real capture transcript/noise gate failed")
    for item, message in zip(evidence, messages):
        item["recognition_result"] = message
    (root / "result.json").write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + "\n"
    )
    print(json.dumps(evidence, ensure_ascii=False, indent=2))
finally:
    if monitor is not None:
        monitor.terminate()
        monitor.wait(timeout=5)
    if daemon is not None:
        daemon.terminate()
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    if module is not None:
        subprocess.run(["pactl", "unload-module", module], check=True)
    log.close()
