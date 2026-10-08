# Reads float values of a texture region at an event and reports the max.
# RD_PICK = "eid:resid:x0:y0:x1:y1"
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]
EID, RESID, X0, Y0, X1, Y1 = [int(v) for v in os.environ["RD_PICK"].split(":")]


def main(log):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    tex = [t for t in controller.GetTextures()
           if int(str(t.resourceId).split("::")[-1]) == RESID][0]
    controller.SetFrameEvent(EID, True)
    sub = rd.Subresource(0, 0, 0)
    best = None
    for y in range(Y0, Y1):
        for x in range(X0, X1):
            v = controller.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless).floatValue
            m = max(v[0], v[1], v[2])
            if best is None or m > best[0]:
                best = (m, x, y, list(v[:4]))
    log.write("max channel %.3f at (%d,%d) rgba=%s\n" % best)
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
