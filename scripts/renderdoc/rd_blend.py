# Prints blend state, render target formats and PS info at the given events.
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RD_CAPTURE"]
OUT = os.environ["RD_OUT"]
EIDS = [int(v) for v in os.environ["RD_EIDS"].split(",")]


def main(log):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAPTURE, "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {t.resourceId: t for t in controller.GetTextures()}
    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        st = controller.GetPipelineState()
        log.write("== eid %d\n" % eid)
        for i, b in enumerate(st.GetColorBlends()):
            if i > 1:
                break
            log.write("  rt%d enabled=%s color: %s * %s %s %s * %s | alpha: %s * %s %s %s * %s | mask=%x\n" % (
                i, b.enabled,
                b.colorBlend.source, "src", b.colorBlend.operation, b.colorBlend.destination, "dst",
                b.alphaBlend.source, "src", b.alphaBlend.operation, b.alphaBlend.destination, "dst",
                b.writeMask))
        for o in st.GetOutputTargets():
            t = textures.get(o.resource)
            if t:
                log.write("  out %s %s\n" % (o.resource, t.format.Name()))
        ps = st.GetShaderReflection(rd.ShaderStage.Pixel)
        if ps:
            log.write("  ps outputs: %s\n" % [s.semanticName + str(s.semanticIndex) for s in ps.outputSignature])
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
