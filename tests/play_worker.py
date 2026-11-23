"""Linux integration test. Run after make. Only temporary plant files are changed."""
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def play(solo, pump_fault="stop", repo=False):
    with tempfile.TemporaryDirectory() as tmp:
        if not repo:
            for name in ("plant", "archive"):
                shutil.copytree(ROOT / name, Path(tmp) / name, symlinks=True)
        args = ["./worker"] if repo else [str(ROOT / "worker")] + (["--solo-test"] if solo else [])
        with subprocess.Popen(args, cwd=ROOT if repo else tmp, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as proc:
            buffer = b""

            def send(line):
                proc.stdin.write((line + "\n").encode())
                proc.stdin.flush()

            def expect(fragment, timeout=16):
                nonlocal buffer
                deadline = time.monotonic() + timeout
                target = fragment.encode()
                while target not in buffer:
                    delay = deadline - time.monotonic()
                    if delay <= 0 or not select.select([proc.stdout], [], [], delay)[0]:
                        raise AssertionError((fragment, buffer.decode(errors="replace")))
                    chunk = os.read(proc.stdout.fileno(), 8192)
                    if not chunk:
                        raise AssertionError(("unexpected exit", buffer.decode(errors="replace")))
                    buffer += chunk
                buffer = buffer.split(target, 1)[1]

            def read_value():
                nonlocal buffer
                deadline = time.monotonic() + 3
                while b"\n" not in buffer:
                    if not select.select([proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                        raise AssertionError("missing measurement")
                    data = os.read(proc.stdout.fileno(), 1024)
                    if not data:
                        raise AssertionError("unexpected exit")
                    buffer += data
                line, buffer = buffer.split(b"\n", 1)
                return float(line)

            try:
                send("manual index")
                expect("POWER PLANT ERROR CODE INDEX" if solo and not repo else "열람할 수 없습니다")
                if repo:
                    session = Path(os.readlink(f"/proc/{proc.pid}/cwd"))
                    assert session != ROOT
                    send("test on")
                    send("manual index")
                    expect("POWER PLANT ERROR CODE INDEX")
                    send("test off")
                    send("manual index")
                    expect("열람할 수 없습니다")
                    if solo:
                        send("test on")
                        expect("테스트 도구 켜짐")
                send("go cooling")
                send("cat pump_label.txt")
                expect("PUMP-B")
                if solo:
                    send("manual registry")
                    expect("CP-70")
                    send("manual history")
                    expect("TS-B")
                steps = [
                    ("C01", "cooling", "pump_label.txt", ["repair coolant_flow 70"]),
                    ("C02", "cooling", "pump_status.txt",
                     ["resume pump" if pump_fault == "stop" else "restart pump"]),
                    ("C03", "cooling", "cooling_sensor_b.dat", ["repair cooling_sensor B"]),
                    ("P01", "power", "load_status.txt", ["repair circuit_load 80 50"]),
                    ("P02", "power", "circuit_status.txt", ["repair fault_isolated 1", "resume breaker"]),
                    ("P03", "power", "power_config.cfg", ["repair power_split 40 60"]),
                    ("G01", "generator", "temp_sensor_b.dat", ["repair temperature_sensor temp_sensor_b.dat"]),
                    ("G02", "generator", "turbine_status.txt", ["restart turbine"]),
                    ("G03", "generator", "generator_label.txt", ["repair generator_config", "restart turbine"]),
                ]
                selected = steps if solo else steps[:1]
                if pump_fault == "kill":
                    selected = steps[:2]
                for event, room, clue, repairs in selected:
                    send("go " + room)
                    if solo:
                        variant = {"C02": " " + pump_fault, "G02": " kill"}.get(event, "")
                        send("debug next" + variant)
                    expect(event + " 발생")
                    send("cat " + clue)
                    if solo:
                        send("manual " + event)
                    if event == "C01":
                        send("cat coolant_config.cfg")
                        expect("target_flow=40")
                        if solo:
                            send("manual C01")
                            expect("PUMP-B,70")
                        # Observe actual degradation before testing automatic recovery.
                        until = time.monotonic() + 5
                        while True:
                            send("cat coolant_level.txt")
                            expect("current_flow=")
                            if read_value() < 55:
                                break
                            if time.monotonic() > until:
                                raise AssertionError("actual flow did not fall")
                            time.sleep(.1)
                    send("status")
                    if event == "P02":
                        send("repair fault_isolated CR-B4 1")
                        expect("고장 회로와 다릅니다")
                        send("resume breaker")
                        expect("먼저 고장 회로를 격리")
                    for line in repairs:
                        if repo and event == "G03" and line == "repair generator_config":
                            config = session / "plant/generator/generator_config.cfg"
                            assert config.stat().st_mode & 0o7777 == 0
                        send(line)
                        if repo and event == "G03" and line == "repair generator_config":
                            expect("권한 복구 완료")
                            assert config.stat().st_mode & 0o7777 == 0o640
                    if event == "C01":
                        expect("회복 확인 중")
                    expect(event + " 수리 완료")
                if selected == steps:
                    expect("이벤트를 모두 수리했습니다")
                send("quit")
                assert proc.wait(timeout=5) == 0
                if repo:
                    assert not session.exists()
                    assert "이벤트를 모두 수리했습니다".encode() not in buffer + proc.stdout.read()
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["--repository"]:
        # Refuse in-place testing on a native volume where normal play uses plant/.
        with tempfile.NamedTemporaryFile(dir=ROOT / "plant/generator") as probe:
            os.fchmod(probe.fileno(), 0o640)
            if os.fstat(probe.fileno()).st_mode & 0o7777 == 0o640:
                sys.exit("Use the default temporary-copy test on this filesystem")
        def snapshot():
            return {p: (os.readlink(p) if p.is_symlink() else (p.stat().st_mtime_ns, p.read_bytes()))
                    for p in (ROOT / "plant").rglob("*") if p.is_symlink() or p.is_file()}
        before = snapshot()
        play(True, repo=True)
        play(False, repo=True)
        assert snapshot() == before
        print("PASS: ./worker from repository, test on/off, G03 0000 -> 0640, one clear, restart, unchanged plant")
        sys.exit(0)
    play(False)
    play(True)
    play(True, "kill")
    print("PASS: default permissions and C01; solo C01 through G03; both C02 variants")
