#include "game/level_data.h"

#include <algorithm>

#include "core/platform.h"
#include "core/text.h"
#include "core/xml.h"

namespace nfh {

bool ObjectDef::hasFlag(const std::string& f) const {
  return std::find(flags.begin(), flags.end(), f) != flags.end();
}

Point ObjectDef::hotspotOr(const std::string& key, Point fallback) const {
  auto it = hotspots.find(key);
  return it != hotspots.end() ? it->second : fallback;
}

const ActionDef* ObjectDef::findAction(const std::string& actName, const std::string& actor) const {
  for (const auto& a : actions) {
    if (toLowerAscii(a.name) == toLowerAscii(actName) && (actor.empty() || toLowerAscii(a.actor) == toLowerAscii(actor))) {
      return &a;
    }
  }
  return nullptr;
}

const RoomDef* LevelData::findRoom(const std::string& name) const {
  const std::string key = toLowerAscii(name);
  for (const auto& r : rooms)
    if (toLowerAscii(r.name) == key) return &r;
  return nullptr;
}

const ObjectDef* LevelData::findObjectDef(const std::string& name) const {
  auto it = objectDefs.find(toLowerAscii(name));
  return it == objectDefs.end() ? nullptr : &it->second;
}

const GfxObjectDef* LevelData::findGfxObject(const std::string& gfxName) const {
  auto it = gfxObjects.find(toLowerAscii(gfxName));
  return it == gfxObjects.end() ? nullptr : &it->second;
}

std::string LevelData::getString(const std::string& name, const std::string& category) const {
  auto it = strings.find(toLowerAscii(name) + "|" + toLowerAscii(category));
  return it == strings.end() ? std::string() : it->second;
}

namespace {

bool loadXmlFile(const GameData& gd, const std::string& vpath, XmlDocument& doc) {
  std::vector<uint8_t> raw;
  if (!gd.read(vpath, raw)) return false;
  doc = parseXml(decodeToUtf8(raw));
  return true;
}

void parseGfxDataXml(const XmlDocument& doc, LevelData& out) {
  for (const XmlNode* obj : doc.root.childrenNamed("object")) {
    const std::string name = obj->attrOr("name");
    if (name.empty()) continue;
    GfxObjectDef& def = out.gfxObjects[toLowerAscii(name)];
    if (def.name.empty()) def.name = name;
    for (const XmlNode* gdNode : obj->childrenNamed("gfxdata")) {
      for (const XmlNode* f : gdNode->childrenNamed("file")) {
        const std::string img = f->attrOr("image");
        int ox = 0, oy = 0;
        f->attrPair("offset", ox, oy);
        if (!img.empty()) {
          def.fileOffsets[toLowerAscii(img)] = Point{ox, oy};
          if (toLowerAscii(name) == "house") {
            out.houseBackgroundVpath = "gfx/house/" + img;
          }
        }
      }
    }
  }
}

void parseAnimsXml(const GameData& gd, const XmlDocument& doc, LevelData& out) {
  auto processObject = [&](const XmlNode* obj) {
    const std::string name = obj->attrOr("name");
    if (name.empty()) return;
    GfxObjectDef& def = out.gfxObjects[toLowerAscii(name)];
    if (def.name.empty()) def.name = name;

    for (const XmlNode* reg : obj->childrenNamed("region")) {
      if (toLowerAscii(reg->attrOr("type")) == "text") continue;
      int rx = 0, ry = 0, rw = 0, rh = 0;
      if (!reg->attrPair("position", rx, ry)) reg->attrPair("pos", rx, ry);
      reg->attrPair("size", rw, rh);
      if (rw > 0 && rh > 0) def.regions.push_back(Rect{rx, ry, rw, rh});
    }

    for (const XmlNode* animNode : obj->childrenNamed("animation")) {
      AnimationDef anim;
      anim.name = animNode->attrOr("name");
      anim.loop = toLowerAscii(animNode->attrOr("type")) == "loop";
      for (const XmlNode* frNode : animNode->childrenNamed("frame")) {
        AnimFrame fr;
        fr.gfxFile = frNode->attrOr("gfx");
        fr.sfx = frNode->attrOr("sfx");
        if (!fr.gfxFile.empty()) {
          auto it = def.fileOffsets.find(toLowerAscii(fr.gfxFile));
          if (it != def.fileOffsets.end()) fr.offset = it->second;
          std::string candidate = "gfx/" + def.name + "/" + fr.gfxFile;
          if (const VfsEntry* ve = gd.find(candidate)) {
            fr.vpath = ve->virtualPath;
          } else {
            fr.vpath = candidate;
          }
        }
        anim.frames.push_back(std::move(fr));
      }
      def.anims[toLowerAscii(anim.name)] = std::move(anim);
    }
  };

  for (const XmlNode* all : doc.root.childrenNamed("all_objects")) {
    for (const XmlNode* obj : all->childrenNamed("object")) processObject(obj);
  }
  for (const XmlNode* obj : doc.root.childrenNamed("object")) processObject(obj);
}

ActionDef parseActionNode(const XmlNode* actNode) {
  ActionDef a;
  a.name = actNode->attrOr("name");
  a.actor = actNode->attrOr("actor");
  a.actorAnim = actNode->attrOr("actoranim");
  a.actorNextAnim = actNode->attrOr("actornextanim");
  a.objAnim = actNode->attrOr("objanim");
  a.objNextAnim = actNode->attrOr("objnextanim");
  std::string t = actNode->attrOr("time");
  a.timeTicks = (t.empty() || t == "auto") ? 0 : actNode->attrInt("time", 0);
  a.noise = actNode->attrInt("noise", 0);
  return a;
}

void parseObjectsXml(const XmlDocument& doc, LevelData& out) {
  for (const auto& childPtr : doc.root.children) {
    const XmlNode* n = childPtr.get();
    if (n->name == "icon") {
      std::string name = toLowerAscii(n->attrOr("name"));
      std::string gfx = n->attrOr("gfx");
      if (!name.empty() && !gfx.empty()) out.icons[name] = "gfx/" + gfx;
    } else if (n->name == "inventar") {
      InventarDef inv;
      inv.name = n->attrOr("name");
      for (const XmlNode* img : n->childrenNamed("image")) {
        std::string kind = toLowerAscii(img->attrOr("name"));
        std::string gfx = "gfx/" + img->attrOr("gfx");
        if (kind == "std") inv.gfxStd = gfx;
        else if (kind == "hover") inv.gfxHover = gfx;
        else if (kind == "down") inv.gfxDown = gfx;
      }
      if (!inv.name.empty()) out.inventar[toLowerAscii(inv.name)] = std::move(inv);
    } else if (n->name == "actor") {
      std::string name = n->attrOr("name");
      if (name.empty()) continue;
      ActorDef& ad = out.actors[toLowerAscii(name)];
      ad.name = name;
      if (const std::string* g = n->attr("gfx")) ad.gfx = *g;
      int hx = 0, hy = 0;
      if (n->attrPair("hotspot", hx, hy)) ad.hotspot = Point{hx, hy};
      for (const XmlNode* sp : n->childrenNamed("speed")) {
        ad.speeds[toLowerAscii(sp->attrOr("name"))] = sp->attrInt("speed", 6);
      }
      for (const XmlNode* act : n->childrenNamed("action")) {
        ad.actions.push_back(parseActionNode(act));
      }
    } else if (n->name == "object" || n->name == "door") {
      ObjectDef od;
      od.name = n->attrOr("name");
      od.gfx = n->attrOr("gfx", n->attrOr("animations", od.name));
      od.isDoor = (n->name == "door");
      if (const XmlNode* sa = n->child("stdaction")) od.stdAction = sa->attrOr("name");
      for (const XmlNode* hs : n->childrenNamed("hotspot")) {
        int hx = 0, hy = 0;
        hs->attrPair("offset", hx, hy);
        od.hotspots[toLowerAscii(hs->attrOr("name"))] = Point{hx, hy};
      }
      for (const XmlNode* fl : n->childrenNamed("flag")) {
        od.flags.push_back(toLowerAscii(fl->attrOr("name")));
      }
      for (const XmlNode* ct : n->childrenNamed("content")) {
        od.contents.emplace_back(ct->attrOr("name"), ct->attrInt("count", 1));
      }
      for (const XmlNode* act : n->childrenNamed("action")) {
        ActionDef a = parseActionNode(act);
        int ax = 0, ay = 0;
        if (act->attrPair("position", ax, ay)) {
          std::string actorKey = toLowerAscii(a.actor.empty() ? "woody" : a.actor);
          if (toLowerAscii(a.name) == "leave") {
            od.hotspots.emplace(actorKey + "_out", Point{ax, ay});
          } else {
            od.hotspots.emplace(actorKey, Point{ax, ay});
          }
        }
        od.actions.push_back(std::move(a));
      }
      if (!od.name.empty()) out.objectDefs[toLowerAscii(od.name)] = std::move(od);
    }
  }
}

void parseStringsXml(const XmlDocument& doc, LevelData& out) {
  for (const XmlNode* sNode : doc.root.childrenNamed("strings")) {
    for (const XmlNode* s : sNode->childrenNamed("string")) {
      std::string key = toLowerAscii(s->attrOr("name")) + "|" + toLowerAscii(s->attrOr("category"));
      out.strings[key] = s->attrOr("text");
    }
  }
}

void parseTricksXml(const XmlDocument& doc, LevelData& out) {
  for (const XmlNode* tNode : doc.root.childrenNamed("tricks")) {
    for (const XmlNode* t : tNode->childrenNamed("trick")) {
      TrickDef td;
      td.name = t->attrOr("name");
      td.quota = t->attrInt("quota1", 10);
      td.angryTime = t->attrInt("angrytime", out.defaultAngryTime);
      if (!td.name.empty()) out.tricks[toLowerAscii(td.name)] = td;
    }
  }
}

void parseCombineXml(const XmlDocument& doc, LevelData& out) {
  auto handleCombo = [&](const XmlNode* c) {
    CombinationDef cd;
    cd.resultObject = c->attrOr("name");
    cd.isTrick = toLowerAscii(c->attrOr("trick")) == "true";
    cd.isWrong = toLowerAscii(c->attrOr("wrong")) == "true";
    auto ings = c->childrenNamed("ingredient");
    if (ings.size() >= 1) {
      cd.target = ings[0]->attrOr("name");
      cd.removeTarget = toLowerAscii(ings[0]->attrOr("remove", "true")) == "true";
    }
    if (ings.size() >= 2) {
      cd.item = ings[1]->attrOr("name");
      cd.removeItem = toLowerAscii(ings[1]->attrOr("remove", "true")) == "true";
    }
    if (!cd.resultObject.empty() && !cd.target.empty()) out.combinations.push_back(std::move(cd));
  };
  for (const XmlNode* c : doc.root.childrenNamed("combination")) handleCombo(c);
  for (const XmlNode* root : doc.root.childrenNamed("combinations")) {
    for (const XmlNode* c : root->childrenNamed("combination")) handleCombo(c);
  }
}

void parseTriggerXml(const XmlDocument& doc, LevelData& out) {
  for (const XmlNode* trRoot : doc.root.childrenNamed("triggers")) {
    for (const XmlNode* act : trRoot->childrenNamed("actor")) {
      if (toLowerAscii(act->attrOr("name")) != "neighbor") continue;
      for (const XmlNode* beh : act->childrenNamed("behavior")) {
        for (const XmlNode* t : beh->childrenNamed("trigger")) {
          TriggerDef td;
          td.behavior = beh->attrOr("name");
          td.object = t->attrOr("object");
          td.position = t->attrOr("position", "nearobj");
          td.once = toLowerAscii(t->attrOr("type", "once")) == "once";
          out.triggers.push_back(std::move(td));
        }
      }
    }
  }
}

void parseLevelXml(const XmlDocument& doc, LevelData& out) {
  const XmlNode* lvl = doc.root.child("level");
  if (!lvl) return;
  out.internalName = lvl->attrOr("name");
  lvl->attrPair("size", out.size.x, out.size.y);
  out.defaultAngryTime = lvl->attrInt("angrytime", 200);

  for (const XmlNode* rNode : lvl->childrenNamed("room")) {
    RoomDef r;
    r.name = rNode->attrOr("name");
    rNode->attrPair("offset", r.offset.x, r.offset.y);
    rNode->attrPair("path1", r.path1.x, r.path1.y);
    rNode->attrPair("path2", r.path2.x, r.path2.y);

    for (const XmlNode* fl : rNode->childrenNamed("floor")) {
      FloorRect fr;
      fl->attrPair("offset", fr.rect.x, fr.rect.y);
      fl->attrPair("size", fr.rect.w, fr.rect.h);
      fr.wall = toLowerAscii(fl->attrOr("wall")) == "true";
      fr.hasHotspot = fl->attrPair("hotspot", fr.hotspot.x, fr.hotspot.y);
      r.floors.push_back(fr);
    }

    for (const XmlNode* nb : rNode->childrenNamed("neighbor")) {
      RoomEdge edge;
      edge.targetRoom = nb->attrOr("name");
      edge.cost = nb->attrInt("costs", 1000);
      edge.doorIn = nb->attrOr("doorin");
      edge.doorOut = nb->attrOr("doorout");
      r.neighbors.push_back(std::move(edge));
    }

    auto addPlaced = [&](const XmlNode* n, bool isDoor) {
      PlacedObject po;
      po.name = n->attrOr("name");
      po.roomName = r.name;
      po.layer = n->attrInt("layer", 1);
      n->attrPair("position", po.position.x, po.position.y);
      po.visible = toLowerAscii(n->attrOr("visible", "true")) != "false";
      po.isDoor = isDoor;
      if (const ObjectDef* od = out.findObjectDef(po.name)) {
        for (const auto& [itemName, cnt] : od->contents) {
          for (int i = 0; i < cnt; ++i) po.items.push_back(itemName);
        }
      }
      out.objects.push_back(std::move(po));
    };

    for (const XmlNode* o : rNode->childrenNamed("object")) addPlaced(o, false);
    for (const XmlNode* d : rNode->childrenNamed("door")) addPlaced(d, true);

    for (const XmlNode* a : rNode->childrenNamed("actor")) {
      ActorSpawn sp;
      sp.name = a->attrOr("name");
      sp.room = r.name;
      sp.layer = a->attrInt("layer", 4);
      a->attrPair("position", sp.position.x, sp.position.y);
      out.spawns.push_back(std::move(sp));
    }

    out.rooms.push_back(std::move(r));
  }
}

}  // namespace

std::vector<LevelInfo> loadLevelList(const GameData& gd) {
  std::vector<LevelInfo> list;
  XmlDocument doc;
  if (!loadXmlFile(gd, "gamedata/leveldata.xml", doc)) return list;
  for (const XmlNode* ld : doc.root.childrenNamed("leveldata")) {
    for (const XmlNode* set : ld->childrenNamed("set")) {
      const std::string setName = set->attrOr("name");
      for (const XmlNode* lvl : set->childrenNamed("level")) {
        LevelInfo info;
        info.setName = setName;
        info.name = lvl->attrOr("name");
        info.reachable = lvl->attrInt("reachable", 0);
        info.minQuota = lvl->attrInt("minquota", 50);
        info.timeTicks = lvl->attrInt("time", 0);
        if (!info.name.empty()) list.push_back(std::move(info));
      }
    }
  }
  return list;
}

bool loadLevel(const GameData& gd, const LevelInfo& info, LevelData& out, std::string* err) {
  out = LevelData{};
  out.info = info;
  const std::string dir = "gamedata/" + info.name;

  XmlDocument doc;
  // 1. gfxdata.xml (generic + level) must be loaded before anims.xml so frame offsets are known.
  if (loadXmlFile(gd, "gamedata/generic/gfxdata.xml", doc)) parseGfxDataXml(doc, out);
  if (loadXmlFile(gd, dir + "/gfxdata.xml", doc)) parseGfxDataXml(doc, out);

  // 2. anims.xml (generic + level)
  if (loadXmlFile(gd, "gamedata/generic/anims.xml", doc)) parseAnimsXml(gd, doc, out);
  if (loadXmlFile(gd, dir + "/anims.xml", doc)) parseAnimsXml(gd, doc, out);

  // 3. objects.xml (generic + level)
  if (loadXmlFile(gd, "gamedata/generic/objects.xml", doc)) parseObjectsXml(doc, out);
  if (loadXmlFile(gd, dir + "/objects.xml", doc)) parseObjectsXml(doc, out);

  // 4. strings.xml (generic + level)
  if (loadXmlFile(gd, "gamedata/generic/strings.xml", doc)) parseStringsXml(doc, out);
  if (loadXmlFile(gd, dir + "/strings.xml", doc)) parseStringsXml(doc, out);

  // 5. level.xml (rooms, objects, doors, spawns)
  if (!loadXmlFile(gd, dir + "/level.xml", doc)) {
    if (err) *err = "cannot load " + dir + "/level.xml";
    return false;
  }
  parseLevelXml(doc, out);

  // 6. tricks, combinations, triggers
  if (loadXmlFile(gd, dir + "/tricks.xml", doc)) parseTricksXml(doc, out);
  if (loadXmlFile(gd, dir + "/combine.xml", doc)) parseCombineXml(doc, out);
  if (loadXmlFile(gd, dir + "/trigger.xml", doc)) parseTriggerXml(doc, out);

  return true;
}

}  // namespace nfh
