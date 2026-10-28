// Data structures and loader for a Neighbours from Hell 1 level.
// Merges gamedata/generic/*.xml with gamedata/<level>/*.xml.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/game_data.h"

namespace nfh {

struct Point {
  int x = 0;
  int y = 0;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct LevelInfo {
  std::string setName;
  std::string name;      // e.g. "level_peep", "level_pie"
  int reachable = 0;     // total tricks on this level
  int minQuota = 0;      // minimum % to win
  int timeTicks = 0;     // time limit in 20 Hz ticks (0 = no limit)
};

struct AnimFrame {
  std::string gfxFile;   // e.g. "w_hide_wardrobe_0001.tga"
  std::string vpath;     // resolved virtual path in GameData, e.g. "gfx/anc/wardrobe/w_hide_wardrobe_0001.tga"
  Point offset;          // frame offset from gfxdata.xml
  std::string sfx;       // optional sound effect path, e.g. "sfx/wod_step1a.wav"
};

struct AnimationDef {
  std::string name;
  bool loop = false;
  std::vector<AnimFrame> frames;
};

struct GfxObjectDef {
  std::string name;  // e.g. "anc/wardrobe", "woody", "doorleft"
  std::vector<Rect> regions;
  std::unordered_map<std::string, AnimationDef> anims;
  std::unordered_map<std::string, Point> fileOffsets;  // lowercase image filename -> offset
};

struct ActionDef {
  std::string name;           // e.g. "open", "close", "take", "enter", "leave", or item name "egg"
  std::string actor;          // "woody" or "neighbor"
  std::string actorAnim;      // e.g. "open", "use_mid", "inv" (invisible)
  std::string actorNextAnim;  // e.g. "ms0", "smile"
  std::string objAnim;        // animation on the object itself
  std::string objNextAnim;    // next animation on the object
  int timeTicks = 0;          // 0 = auto (duration of animation)
  int noise = 0;
};

struct ObjectDef {
  std::string name;      // e.g. "anc/ark", "kit/fridge", "anc/kit"
  std::string gfx;       // key into GfxObjectDef table, e.g. "anc/ark", "doorback"
  std::string stdAction; // "open", "hide", "goto", "end", "look", "use", "examine"
  bool isDoor = false;
  std::unordered_map<std::string, Point> hotspots;  // "woody", "neighbor", "woody_out", "neighbor_out", "progressbar"
  std::vector<std::string> flags;                   // "container", "hideout", "singleuse", "doorleft", ...
  std::vector<std::pair<std::string, int>> contents;
  std::vector<ActionDef> actions;

  bool hasFlag(const std::string& f) const;
  Point hotspotOr(const std::string& key, Point fallback = {}) const;
  const ActionDef* findAction(const std::string& actName, const std::string& actor) const;
};

struct InventarDef {
  std::string name;     // e.g. "egg", "fartbag"
  std::string gfxStd;   // e.g. "gfx/gui/inv/i_egg_norm.tga"
  std::string gfxHover;
  std::string gfxDown;
};

struct TrickDef {
  std::string name;     // resulting trick object name, e.g. "kit/foamcream", "lir/sofa_fartbag"
  int quota = 10;
  int angryTime = 200;
};

struct CombinationDef {
  std::string resultObject;  // e.g. "kit/foamcream"
  bool isTrick = false;
  bool isWrong = false;
  std::string target;        // first ingredient: object or room name (e.g. "kit/whippedcream", "toi")
  bool removeTarget = true;
  std::string item;          // second ingredient: inventory item name (empty for bare-hand tricks like "lir/tv")
  bool removeItem = true;
};

struct TriggerDef {
  std::string behavior;
  std::string object;        // e.g. "anc/mum_smeared", "toi/groundsoap"
  std::string position;      // "nearobj", "room"
  bool once = true;
};

struct FloorRect {
  Rect rect;        // in room-local coordinates
  bool wall = false;
  Point hotspot;
  bool hasHotspot = false;
};

struct RoomEdge {
  std::string targetRoom;
  int cost = 1000;
  std::string doorIn;   // door in current room, e.g. "anc/kit"
  std::string doorOut;  // door in target room, e.g. "kit/anc"
};

struct PlacedObject {
  std::string name;      // e.g. "anc/wardrobe", "kit/fridge"
  std::string roomName;
  int layer = 1;
  Point position;        // extra offset in room (mostly 0/0 for objects, non-zero for doors)
  bool visible = true;
  bool isDoor = false;
  std::string currentAnim = "ms";
  std::string nextAnim = "ms";
  int animFrame = 0;
  std::vector<std::string> items;  // remaining container items
};

struct RoomDef {
  std::string name;  // "fro", "anc", "kit", "lir", "toi", ...
  Point offset;      // room offset in house coordinates
  Point path1;       // left walk boundary in room-local coordinates
  Point path2;       // right walk boundary in room-local coordinates
  std::vector<FloorRect> floors;
  std::vector<RoomEdge> neighbors;
};

struct ActorSpawn {
  std::string name;  // "woody", "neighbor", "dog", "chili"
  std::string room;
  int layer = 4;
  Point position;    // room-local position
};

struct ActorDef {
  std::string name;
  std::string gfx;
  Point hotspot;     // e.g. 463/419 for woody, 460/425 for neighbor
  std::unordered_map<std::string, int> speeds;  // "mg1" -> pixels per tick
  std::vector<ActionDef> actions;
};

struct LevelData {
  LevelInfo info;
  std::string internalName;
  Point size{1428, 736};
  int defaultAngryTime = 200;
  std::string houseBackgroundVpath;  // e.g. "gfx/house/house10.tga"

  std::unordered_map<std::string, GfxObjectDef> gfxObjects;
  std::unordered_map<std::string, ObjectDef> objectDefs;
  std::unordered_map<std::string, InventarDef> inventar;
  std::unordered_map<std::string, std::string> icons;  // icon name -> vpath
  std::vector<std::string> levelIcons;                 // ordered routine icons from <level>/objects.xml
  std::unordered_map<std::string, TrickDef> tricks;
  std::vector<CombinationDef> combinations;
  std::vector<TriggerDef> triggers;
  std::unordered_map<std::string, ActorDef> actors;

  std::vector<RoomDef> rooms;
  std::vector<PlacedObject> objects;
  std::vector<ActorSpawn> spawns;

  // Strings: key is lowercase "name|category"
  std::unordered_map<std::string, std::string> strings;

  const RoomDef* findRoom(const std::string& name) const;
  const ObjectDef* findObjectDef(const std::string& name) const;
  const GfxObjectDef* findGfxObject(const std::string& gfxName) const;
  std::string getString(const std::string& name, const std::string& category) const;
  std::string getUIString(const std::string& name) const { return getString(name, "string"); }
};

// Reads gamedata/leveldata.xml and returns all levels in order.
std::vector<LevelInfo> loadLevelList(const GameData& gd);

// Loads a full level (merging generic + level folder).
bool loadLevel(const GameData& gd, const LevelInfo& info, LevelData& out, std::string* err = nullptr);

}  // namespace nfh
