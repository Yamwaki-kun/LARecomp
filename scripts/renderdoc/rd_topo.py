# Lists draws in [E0, E1] with their topology, vertex count and viewport.
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
    draws = []

    def walk(actions):
        for a in actions:
            if (a.flags & rd.ActionFlags.Drawcall) and E0 <= a.eventId <= E1:
                draws.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    for a in draws:
        controller.SetFrameEvent(a.eventId, False)
        st = controller.GetPipelineState()
        vp = st.GetViewport(0)
        outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
        log.write("%d n=%d inst=%d topo=%s vp=(%.0f,%.0f %.0fx%.0f) gs=%s out=%s\n" % (
            a.eventId, a.numIndices, a.numInstances, st.GetPrimitiveTopology(),
            vp.x, vp.y, vp.width, vp.height,
            st.GetShader(rd.ShaderStage.Geometry) != rd.ResourceId.Null(),
            ",".join(outs)))
    controller.Shutdown()
    cap.Shutdown()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
