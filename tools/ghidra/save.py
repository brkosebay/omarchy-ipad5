# Runs INSIDE the ghidra-mcp server process (injected via lldb).
# Persists the in-memory analysis of the open program to the Ghidra project.
import gc
import traceback

import os
LOG = os.environ.get("GHIDRA_SAVE_LOG", "/tmp/ghidra-save.log")


def log(msg):
    with open(LOG, "a") as f:
        f.write(msg + "\n")


try:
    from ghidra_mcp.headless import HeadlessSession

    sessions = [o for o in gc.get_objects() if isinstance(o, HeadlessSession)]
    log(f"sessions: {len(sessions)}")
    s = sessions[0]
    prog = s._program
    log(f"program: {prog.getName()} changed={prog.isChanged()}")
    from ghidra.program.util import GhidraProgramUtilities

    log(f"isAnalyzed before: {GhidraProgramUtilities.isAnalyzed(prog)}")
    try:
        GhidraProgramUtilities.markProgramAnalyzed(prog)
    except Exception as e:  # noqa: BLE001
        log(f"markProgramAnalyzed failed: {e!r}")
    log(f"isAnalyzed after mark: {GhidraProgramUtilities.isAnalyzed(prog)}")
    prog.save("linux-a9 manual save after analysis", s._task_monitor())
    log(f"SAVED ok, changed now={prog.isChanged()}")
except Exception:  # noqa: BLE001
    log("FAILED:\n" + traceback.format_exc())
