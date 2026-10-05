#pragma once

#include <functional>

#include <rex/rex_app.h>

namespace larecomp {

// Starts `work` -- the host-side content build: mod archive and custom music --
// on a worker thread and returns at once. A small progress popup comes up in the
// bottom-right corner of the game window if the build is still going after a
// moment, so a boot with nothing to do still looks the way it always did. When
// the work is done, `then` runs on the UI thread.
//
// The build used to run straight on the UI thread, which meant no paint and no
// frame until it finished: a folder of a hundred fresh MP3s left a black,
// not-responding window for a minute. It then ran behind a nested message loop,
// which the SDK's SDL backend does not support (the stock SDK has no way to pump
// it at all). Now nothing waits: the popup keeps the window painting and the
// app's own loop runs, and the caller continues from `then` -- rex::ReXApp's
// LaunchModule, deferred, is what it is meant for.
//
// With no drawer the build runs synchronously, then `then`, as before.
void StartBootBuildWithOverlay(rex::ui::WindowedAppContext& app_context,
                               rex::ui::ImGuiDrawer* drawer, std::function<void()> work,
                               std::function<void()> then);

// Blocks until a build started above has finished. For shutdown: closing the
// window mid-build must not destroy the app under the worker.
void WaitForBootBuild();

}  // namespace larecomp
