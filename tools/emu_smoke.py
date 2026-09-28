"""Smoke-run a course example against the BENTO Emulator's Python modules, on the host.

The emulator's modules (BENTO_IDE/bento-emulator/py) talk to the page through a JS
bridge; here a fake bridge answers with a fixed farm state and swallows drawing calls,
and MicroPython's time.sleep_ms/ticks_ms are shimmed so a 2-minute loop runs in a
moment. It proves the example imports, builds its screen and runs its loop against
the same API surface the emulator offers. It does NOT prove behaviour on a board.

usage: python3 emu_smoke.py <example.py> [...]
       BENTO_EMU_PY=/path/to/bento-emulator/py python3 emu_smoke.py <example.py> [...]
"""
import json
import os
import runpy
import sys
import time as _time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.environ.get("BENTO_EMU_PY") or os.path.abspath(
    os.path.join(HERE, "../../../../BENTO_IDE/bento-emulator/py"))
if not os.path.isdir(EMU):
    sys.exit("emu_smoke: emulator modules not found at %s -- set BENTO_EMU_PY" % EMU)

STATE = {"temp": 27.5, "hum": 68.0, "pressure": 1009.8, "shaking": False,
         "pots": [900, 2600, 2048, 2048], "accel": {"x": 0.3, "y": 1.2, "z": 9.7},
         "gyro": {"x": 0, "y": 0, "z": 0}, "mag": {"x": 20, "y": 5, "z": -40},
         "buttons": [0, 0, 0, 0], "leds": [0, 0, 0, 0],
         "devButtons": [False, False]}
_id = [0]


def _new_id(*a, **k):
    _id[0] += 1
    return _id[0]


bridge = types.ModuleType("bento_bridge")
bridge.hw_json = lambda: json.dumps(STATE)
bridge.poll_json = lambda *a: "[]"
bridge.list_json = lambda *a: "[]"
bridge.create = _new_id
bridge.get_text = lambda *a: ""
bridge.get_value = lambda *a: 0
bridge.get_type = lambda *a: 0
bridge.gpio_led_state = lambda *a: 0
bridge.gpio_button = lambda *a: 0


def _noop(*a, **k):
    return 0


bridge.__getattr__ = lambda name: _noop
sys.modules["bento_bridge"] = bridge

# MicroPython time on top of CPython time, with a clock that runs 50x fast
_t0 = _time.monotonic()
_skew = [0.0]


def ticks_ms():
    return int(((_time.monotonic() - _t0) + _skew[0]) * 1000)


def sleep_ms(ms):
    _skew[0] += ms / 1000.0


def ticks_diff(a, b):
    return a - b


_time.ticks_ms = ticks_ms
_time.sleep_ms = sleep_ms
_time.ticks_diff = ticks_diff
_time.ticks_add = lambda a, b: a + b
_time.ticks_us = lambda: ticks_ms() * 1000
_time.sleep_us = lambda us: None

sys.path.insert(0, EMU)
rc = 0
for path in sys.argv[1:]:
    _skew[0] = 0.0
    try:
        runpy.run_path(path, run_name="__main__")
        print("OK  ", os.path.basename(path))
    except SystemExit:
        print("OK  ", os.path.basename(path), "(SystemExit)")
    except Exception as e:
        rc = 1
        import traceback
        tb = traceback.extract_tb(e.__traceback__)
        where = next((f"{os.path.basename(f.filename)}:{f.lineno}" for f in reversed(tb)
                      if f.filename.endswith(os.path.basename(path))), "?")
        print("FAIL", os.path.basename(path), "at", where, "->", type(e).__name__, e)
sys.exit(rc)
