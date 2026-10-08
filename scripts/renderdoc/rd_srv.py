# Lists textures read by the pixel shader at an event, and saves them.
# RD_EID = event id
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]
OUTDIR = os.environ["RD_OUTDIR"]
EID = int(os.environ["RD_EID"])


def main(log):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {t.resourceId: t for t in controller.GetTextures()}
    resources = {r.resourceId: r for r in controller.GetResources()}
    controller.SetFrameEvent(EID, True)
    state = controller.GetPipelineState()
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
        used = state.GetReadOnlyResources(stage, True)
        for u in used:
            rid = u.descriptor.resource
            if rid == rd.ResourceId.Null():
                continue
            t = textures.get(rid)
            name = resources[rid].name if rid in resources else str(rid)
            if t:
                log.write("%s %s | %s | %dx%d ms%d %s\n" % (
                    stage, rid, name, t.width, t.height, t.msSamp, t.format.Name()))
                ts = rd.TextureSave()
                ts.resourceId = rid
                ts.alpha = rd.AlphaMapping.Discard
                ts.destType = rd.FileType.PNG
                ts.sample.sampleIndex = 0xFFFFFFFF
                n = str(rid).split("::")[-1]
                controller.SaveTexture(ts, os.path.join(OUTDIR, "srv_%d_%s.png" % (EID, n)))
            else:
                log.write("%s %s | %s (buffer)\n" % (stage, rid, name))
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
