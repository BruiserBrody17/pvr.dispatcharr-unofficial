# One simulated Dispatcharr worker process: imports the plugin fresh and runs JSON-line commands from stdin.
import json
import logging
import os
import sys
import importlib.util
import traceback
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("tsplugin", os.environ["PLUGIN_PY"])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
logging.basicConfig(
    level=logging.WARNING, stream=sys.stderr, format="[w%s %%(levelname)s] %%(message)s" % os.environ.get("WID", "?")
)
log = logging.getLogger("w")
settings = json.loads(os.environ["PLUGIN_SETTINGS"])
plugin = mod.Plugin()
for line in sys.stdin:
    req = json.loads(line)
    t0 = time.time()
    try:
        if req.get("stop"):
            plugin.stop({"settings": settings, "logger": log, "reason": req["stop"]})
            res = {"stopped": True}
        else:
            res = plugin.run(req["action"], req.get("params", {}), {"settings": settings, "logger": log})
    except Exception as e:
        res = {"exception": repr(e), "tb": traceback.format_exc()[-800:]}
    sys.stdout.write(
        json.dumps({"id": req.get("id"), "dt": round(time.time() - t0, 3), "res": res}, default=str) + "\n"
    )
    sys.stdout.flush()
