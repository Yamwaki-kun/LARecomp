# Pixel history: which events wrote a pixel of a texture.
# RD_HIST = "eid:resid:x:y"
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]
EID, RESID, X, Y = [int(v) for v in os.environ["RD_HIST"].split(":")]


def main(log):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("replay failed %s\n" % result)
        return
    structured = controller.GetStructuredFile()
    tex = [t for t in controller.GetTextures()
           if int(str(t.resourceId).split("::")[-1]) == RESID][0]
    controller.SetFrameEvent(EID, True)
    sub = rd.Subresource(0, 0, 0)
    history = controller.PixelHistory(tex.resourceId, X, Y, sub, rd.CompType.Typeless)
    names = {}

    def walk(actions):
        for a in actions:
            names[a.eventId] = a.GetName(structured)
            walk(a.children)
    walk(controller.GetRootActions())
    for mod in history:
        c = mod.postMod.col.floatValue
        log.write("eid %d %s prim %d passed=%s shader_out=(%.3f %.3f %.3f %.3f) post=(%.3f %.3f %.3f %.3f)\n" % (
            mod.eventId, names.get(mod.eventId, "?"), mod.primitiveID, mod.Passed(),
            mod.shaderOut.col.floatValue[0], mod.shaderOut.col.floatValue[1],
            mod.shaderOut.col.floatValue[2], mod.shaderOut.col.floatValue[3],
            c[0], c[1], c[2], c[3]))
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
