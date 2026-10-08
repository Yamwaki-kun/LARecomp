# Saves textures at given events as PNG.
# RD_JOBS = "eid:resid:name;eid:resid:name" (resid is the number in ResourceId::N)
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUTDIR = os.environ["RD_OUTDIR"]
JOBS = [j.split(":") for j in os.environ["RD_JOBS"].split(";") if j]


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        log.write("open failed\n")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("replay failed %s\n" % result)
        return
    by_id = {int(str(t.resourceId).split("::")[-1]): t for t in controller.GetTextures()}
    for eid, resid, name in JOBS:
        tex = by_id.get(int(resid))
        if tex is None:
            log.write("%s: no texture %s\n" % (name, resid))
            continue
        controller.SetFrameEvent(int(eid), True)
        ts = rd.TextureSave()
        ts.resourceId = tex.resourceId
        ts.mip = 0
        ts.slice.sliceIndex = 0
        ts.alpha = rd.AlphaMapping.Discard
        ts.destType = rd.FileType.PNG
        # Average the samples of MSAA targets.
        ts.sample.mapToArray = False
        ts.sample.sampleIndex = 0xFFFFFFFF
        path = os.path.join(OUTDIR, name + ".png")
        ok = controller.SaveTexture(ts, path)
        log.write("%s: eid %s res %s %dx%d -> %s (%s)\n" % (
            name, eid, resid, tex.width, tex.height, path, ok))
    controller.Shutdown()
    cap.Shutdown()


with open(os.path.join(OUTDIR, "save_log.txt"), "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
