// nfh-viewer entry point.
//   nfh-viewer [--game PATH]
#include <SDL.h>

#include <cstdio>
#include <string>

#include "core/platform.h"
#include "viewer/viewer.h"

int main(int argc, char** argv) {
  SDL_SetMainReady();
  nfh::setupConsoleUtf8();
  std::vector<std::string> args = nfh::utf8Args(argc, argv);
  nfh::ViewerOptions opts;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool hasNext = i + 1 < args.size();
    if ((a == "--game" || a == "-g") && hasNext) opts.game = args[++i];
    else if (a == "--filter" && hasNext) opts.filter = args[++i];
    else if (a == "--screenshot" && hasNext) opts.screenshot = args[++i];
    else if (a == "--play") opts.play = true;
    else if (a == "-h" || a == "--help") {
      std::printf(
          "usage: nfh-viewer [--game PATH] [--filter TEXT] [--play] [--screenshot FILE.png]\n"
          "  --game PATH         game folder or its data/ folder (remembered in opennfh.cfg)\n"
          "  --filter TEXT       initial file filter; the first match is selected\n"
          "  --play              start animation / sound right away\n"
          "  --screenshot FILE   render, save a PNG screenshot and exit (for testing)\n");
      return 0;
    } else if (opts.game.empty()) opts.game = a;  // allow plain path argument
  }
  nfh::Viewer viewer;
  if (!viewer.init(opts)) return 1;
  return viewer.run();
}
