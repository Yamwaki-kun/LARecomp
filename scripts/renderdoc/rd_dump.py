# Dumps the action tree (with debug markers) and the texture list of a capture.
# Run inside qrenderdoc: qrenderdoc.exe --python rd_dump.py
import os
import sys
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]


def main(out):
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(CAPTURE, "", None)
    if result != rd.ResultCode.Succeeded:
        out.write("open failed: %s\n" % result)
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        out.write("replay failed: %s\n" % result)
        return

    structured = controller.GetStructuredFile()
    textures = {t.resourceId: t for t in controller.GetTextures()}
    resources = {r.resourceId: r for r in controller.GetResources()}

    def name_of(rid):
        r = resources.get(rid)
        return r.name if r else str(rid)

    out.write("=== textures (%d) ===\n" % len(textures))
    for rid, t in textures.items():
        out.write("%s | %s | %dx%dx%d arr%d mips%d ms%d | %s\n" % (
            rid, name_of(rid), t.width, t.height, t.depth, t.arraysize,
            t.mips, t.msSamp, t.format.Name()))

    out.write("\n=== actions ===\n")

    def walk(actions, depth):
        for a in actions:
            name = a.GetName(structured)
            outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
            out.write("%s%d %s%s\n" % ("  " * depth, a.eventId, name,
                                        (" -> " + ",".join(outs)) if outs else ""))
            if a.children:
                walk(a.children, depth + 1)

    walk(controller.GetRootActions(), 0)
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
