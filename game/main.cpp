// opennfh entry point (Stage 2 playable engine).
//   opennfh [--game PATH] [--level NAME] [--screenshot FILE.png]
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/platform.h"
#include "game/engine.h"

int main(int argc, char** argv) {
  SDL_SetMainReady();
  nfh::setupConsoleUtf8();
  std::vector<std::string> args = nfh::utf8Args(argc, argv);

  nfh::GameOptions opts;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool hasNext = i + 1 < args.size();
    if ((a == "--game" || a == "-g") && hasNext) opts.gamePath = args[++i];
    else if ((a == "--level" || a == "-l") && hasNext) {
      opts.levelName = args[++i];
      opts.startInLevel = true;
    } else if (a == "--screen" && hasNext) opts.initialScreen = args[++i];
    else if (a == "--screenshot" && hasNext) opts.screenshot = args[++i];
    else if (a == "--ticks" && hasNext) opts.screenshotTicks = std::atoi(args[++i].c_str());
    else if (a == "--record-dir" && hasNext) opts.recordDir = args[++i];
    else if (a == "--stride" && hasNext) opts.recordStride = std::max(1, std::atoi(args[++i].c_str()));
    else if (a == "-h" || a == "--help") {
      std::printf(
          "OpenNFH — Neighbours from Hell 1 engine\n"
          "usage: opennfh [--game PATH] [--level LEVEL_NAME] [--screen SCREEN] [--screenshot FILE.png]\n"
          "  controls:\n"
          "    Left click    walk / open container / hide / use item / go through door\n"
          "    Right click   sneak / deselect inventory item\n"
          "    Esc           open in-game pause menu / back in menus\n"
          "    [ / ]         previous / next level (all 17 levels)\n"
          "    V or Tab      toggle Full House camera / Follow Woody camera\n"
          "    R             restart current level\n"
          "    H             toggle debug hitboxes\n"
          "    Space         pause\n");
      return 0;
    } else if (opts.gamePath.empty()) {
      opts.gamePath = a;
    }
  }

  nfh::GameEngine engine;
  if (!engine.init(opts)) return 1;
  return engine.run();
}
