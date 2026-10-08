# For draws in [E0, E1], list pixel-shader textures that are small (<= 512 wide)
# float textures, i.e. candidate reflection maps, and how many draws read each.
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]
E0, E1 = [int(v) for v in os.environ["RD_RANGE"].split(":")]


def main(log):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {t.resourceId: t for t in controller.GetTextures()}
    draws = []

    def walk(actions):
        for a in actions:
            if (a.flags & rd.ActionFlags.Drawcall) and E0 <= a.eventId <= E1:
                draws.append(a.eventId)
            walk(a.children)
    walk(controller.GetRootActions())

    readers = {}
    for eid in draws:
        controller.SetFrameEvent(eid, False)
        state = controller.GetPipelineState()
        for u in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
            t = textures.get(u.descriptor.resource)
            if not t or t.width > 512:
                continue
            fmt = t.format.Name()
            if "FLOAT" not in fmt and "UNORM" not in fmt:
                continue
            key = "%s %dx%d arr%d %s" % (t.resourceId, t.width, t.height, t.arraysize, fmt)
            readers.setdefault(key, []).append(eid)
    log.write("draws scanned: %d\n" % len(draws))
    for key, eids in sorted(readers.items(), key=lambda kv: -len(kv[1])):
        log.write("%5d draws  %s  first=%d last=%d\n" % (len(eids), key, eids[0], eids[-1]))
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
