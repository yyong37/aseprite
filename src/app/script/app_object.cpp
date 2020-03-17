// Aseprite
// Copyright (C) 2018-2020  Igara Studio S.A.
// Copyright (C) 2015-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/context.h"
#include "app/doc.h"
#include "app/doc_access.h"
#include "app/i18n/strings.h"
#include "app/loop_tag.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/script/api_version.h"
#include "app/script/docobj.h"
#include "app/script/engine.h"
#include "app/script/luacpp.h"
#include "app/script/security.h"
#include "app/site.h"
#include "app/tools/active_tool.h"
#include "app/tools/tool_box.h"
#include "app/tools/tool_loop.h"
#include "app/tools/tool_loop_manager.h"
#include "app/tx.h"
#include "app/ui/context_bar.h"
#include "app/ui/doc_view.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/tool_loop_impl.h"
#include "app/ui/timeline/timeline.h"
#include "app/ui_context.h"
#include "base/fs.h"
#include "base/replace_string.h"
#include "base/version.h"
#include "doc/layer.h"
#include "doc/primitives.h"
#include "doc/tag.h"
#include "render/render.h"
#include "ui/alert.h"
#include "ver/info.h"

#include <iostream>

namespace app {
namespace script {

int load_sprite_from_file(lua_State* L, const char* filename,
                          const LoadSpriteFromFileParam param)
{
  std::string absFn = base::get_absolute_path(filename);
  if (!ask_access(L, absFn.c_str(), FileAccessMode::Read, true))
    return luaL_error(L, "script doesn't have access to open file %s",
                      absFn.c_str());

  app::Context* ctx = App::instance()->context();
  Doc* oldDoc = ctx->activeDocument();

  Command* openCommand =
    Commands::instance()->byId(CommandId::OpenFile());
  Params params;
  params.set("filename", absFn.c_str());
  if (param == LoadSpriteFromFileParam::OneFrameAsImage)
    params.set("oneframe", "true");
  ctx->executeCommand(openCommand, params);

  Doc* newDoc = ctx->activeDocument();
  if (newDoc != oldDoc) {
    if (param == LoadSpriteFromFileParam::OneFrameAsImage) {
      doc::Sprite* sprite = newDoc->sprite();

      // Render the first frame of the sprite
      // TODO add "frame" parameter to render different frames
      std::unique_ptr<doc::Image> image(doc::Image::create(sprite->spec()));
      doc::clear_image(image.get(), sprite->transparentColor());
      render::Render().renderSprite(image.get(), sprite, 0);

      // Restore the old document and active and destroy the recently
      // loaded sprite.
      ctx->setActiveDocument(oldDoc);

      try {
        DocDestroyer destroyer(ctx, newDoc, 500);
        destroyer.destroyDocument();
      }
      catch (const LockedDocException& ex) {
        // Almost impossible?
        luaL_error(L, "cannot lock document to close it\n%s", ex.what());
      }

      push_image(L, image.release());
      return 1;
    }
    else {
      push_docobj(L, newDoc->sprite());
    }
  }
  else
    lua_pushnil(L);
  return 1;
}

namespace {

int App_open(lua_State* L)
{
  return load_sprite_from_file(
    L, luaL_checkstring(L, 1), LoadSpriteFromFileParam::FullAniAsSprite);
}

int App_exit(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  if (ctx && ctx->isUIAvailable()) {
    Command* exitCommand =
      Commands::instance()->byId(CommandId::Exit());
    ctx->executeCommand(exitCommand);
  }
  return 0;
}

int App_transaction(lua_State* L)
{
  int top = lua_gettop(L);
  int nresults = 0;
  if (lua_isfunction(L, 1)) {
    Tx tx; // Create a new transaction so it exists in the whole
           // duration of the argument function call.
    lua_pushvalue(L, -1);
    if (lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK)
      tx.commit();
    nresults = lua_gettop(L) - top;
  }
  return nresults;
}

int App_undo(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  if (ctx) {
    Command* undo = Commands::instance()->byId(CommandId::Undo());
    ctx->executeCommand(undo);
  }
  return 0;
}

int App_redo(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  if (ctx) {
    Command* redo = Commands::instance()->byId(CommandId::Redo());
    ctx->executeCommand(redo);
  }
  return 0;
}

int App_alert(lua_State* L)
{
#ifdef ENABLE_UI
  app::Context* ctx = App::instance()->context();
  if (!ctx || !ctx->isUIAvailable())
    return 0;                   // No UI to show the alert
  // app.alert("text...")
  else if (lua_isstring(L, 1)) {
    ui::AlertPtr alert(new ui::Alert);
    alert->addLabel(lua_tostring(L, 1), ui::CENTER);
    alert->addButton(Strings::general_ok());
    lua_pushinteger(L, alert->show());
    return 1;
  }
  // app.alert{ ... }
  else if (lua_istable(L, 1)) {
    ui::AlertPtr alert(new ui::Alert);

    int type = lua_getfield(L, 1, "title");
    if (type != LUA_TNIL)
      alert->setTitle(lua_tostring(L, -1));
    lua_pop(L, 1);

    type = lua_getfield(L, 1, "text");
    if (type == LUA_TTABLE) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        const char* v = luaL_tolstring(L, -1, nullptr);
        if (v)
          alert->addLabel(v, ui::LEFT);
        lua_pop(L, 2);
      }
    }
    else if (type == LUA_TSTRING) {
      alert->addLabel(lua_tostring(L, -1), ui::LEFT);
    }
    lua_pop(L, 1);

    int nbuttons = 0;
    type = lua_getfield(L, 1, "buttons");
    if (type == LUA_TTABLE) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        const char* v = luaL_tolstring(L, -1, nullptr);
        if (v) {
          alert->addButton(v);
          ++nbuttons;
        }
        lua_pop(L, 2);
      }
    }
    else if (type == LUA_TSTRING) {
      alert->addButton(lua_tostring(L, -1));
      ++nbuttons;
    }
    lua_pop(L, 1);

    if (nbuttons == 0)
      alert->addButton(Strings::general_ok());

    lua_pushinteger(L, alert->show());
    return 1;
  }
#endif
  return 0;
}

int App_refresh(lua_State* L)
{
#ifdef ENABLE_UI
  app::Context* ctx = App::instance()->context();
  if (ctx && ctx->isUIAvailable())
    app_refresh_screen();
#endif
  return 0;
}

int App_useTool(lua_State* L)
{
  // First argument must be a table
  if (!lua_istable(L, 1))
    return luaL_error(L, "app.useTool() must be called with a table as its first argument");

  auto ctx = App::instance()->context();
  Site site = ctx->activeSite();

  // Draw in a specific cel, layer, or frame
  int type = lua_getfield(L, 1, "layer");
  if (type != LUA_TNIL) {
    if (auto layer = get_docobj<Layer>(L, -1)) {
      site.document(static_cast<Doc*>(layer->sprite()->document()));
      site.sprite(layer->sprite());
      site.layer(layer);
    }
  }
  lua_pop(L, 1);

  type = lua_getfield(L, 1, "frame");
  if (type != LUA_TNIL) {
    site.frame(get_frame_number_from_arg(L, -1));
  }
  lua_pop(L, 1);

  type = lua_getfield(L, 1, "cel");
  if (type != LUA_TNIL) {
    if (auto cel = get_docobj<Cel>(L, -1)) {
      site.document(static_cast<Doc*>(cel->sprite()->document()));
      site.sprite(cel->sprite());
      site.layer(cel->layer());
      site.frame(cel->frame());
    }
  }
  lua_pop(L, 1);

  if (!site.document())
    return luaL_error(L, "there is no active document to draw with the tool");

  // Select tool by name
  tools::Tool* tool = App::instance()->activeToolManager()->activeTool();
  tools::Ink* ink = tool->getInk(0);
  type = lua_getfield(L, 1, "tool");
  if (type != LUA_TNIL) {
    if (auto toolArg = get_tool_from_arg(L, -1)) {
      tool = toolArg;
      ink = tool->getInk(0);
    }
    else
      return luaL_error(L, "invalid tool specified in app.useTool() function");
  }
  lua_pop(L, 1);

  // Default color is the active fgColor
  app::Color color = Preferences::instance().colorBar.fgColor();
  type = lua_getfield(L, 1, "color");
  if (type != LUA_TNIL)
    color = convert_args_into_color(L, -1);
  lua_pop(L, 1);

  // Default brush is the active brush in the context bar
  BrushRef brush(nullptr);
#ifdef ENABLE_UI
  if (App::instance()->isGui() &&
      App::instance()->contextBar())
    brush = App::instance()->contextBar()->activeBrush(tool, ink);
#endif
  type = lua_getfield(L, 1, "brush");
  if (type != LUA_TNIL)
    brush = get_brush_from_arg(L, -1);
  lua_pop(L, 1);
  if (!brush)
    brush.reset(new Brush(BrushType::kCircleBrushType, 1, 0));

  // How the tileset must be modified depending on this tool usage
  type = lua_getfield(L, 1, "tilesetMode");
  if (type != LUA_TNIL) {
    site.tilesetMode(TilesetMode(lua_tointeger(L, -1)));
  }
  lua_pop(L, 1);

  // Do the tool loop
  type = lua_getfield(L, 1, "points");
  if (type == LUA_TTABLE) {
    std::unique_ptr<tools::ToolLoop> loop(
      create_tool_loop_for_script(ctx, site, tool, ink, color, brush));
    if (!loop)
      return luaL_error(L, "cannot draw in the active site");

    tools::ToolLoopManager manager(loop.get());
    tools::Pointer lastPointer;
    bool first = true;

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      gfx::Point pt = convert_args_into_point(L, -1);

      tools::Pointer pointer(pt, tools::Pointer::Button::Left);
      if (first) {
        first = false;
        manager.prepareLoop(pointer);
        manager.pressButton(pointer);
      }
      else {
        manager.movement(pointer);
      }
      lastPointer = pointer;
      lua_pop(L, 1);
    }
    if (!first)
      manager.releaseButton(lastPointer);

    loop->commitOrRollback();
  }
  lua_pop(L, 1);
  return 0;
}

int App_get_activeSprite(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Doc* doc = ctx->activeDocument();
  if (doc)
    push_docobj(L, doc->sprite());
  else
    lua_pushnil(L);
  return 1;
}

int App_get_activeLayer(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  if (site.layer())
    push_docobj(L, site.layer());
  else
    lua_pushnil(L);
  return 1;
}

int App_get_activeFrame(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  if (site.sprite())
    push_sprite_frame(L, site.sprite(), site.frame());
  else
    lua_pushnil(L);
  return 1;
}

int App_get_activeCel(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  if (site.cel())
    push_sprite_cel(L, site.cel());
  else
    lua_pushnil(L);
  return 1;
}

int App_get_activeImage(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  if (site.cel())
    push_cel_image(L, site.cel());
  else
    lua_pushnil(L);
  return 1;
}

int App_get_activeTag(lua_State* L)
{
  Tag* tag = nullptr;

  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  if (site.sprite()) {
#ifdef ENABLE_UI
    if (App::instance()->timeline()) {
      tag = App::instance()->timeline()->getTagByFrame(site.frame(), false);
    }
    else
#endif
    {
      tag = get_animation_tag(site.sprite(), site.frame());
    }
  }

  if (tag)
    push_docobj(L, tag);
  else
    lua_pushnil(L);
  return 1;
}

int App_get_sprites(lua_State* L)
{
  push_sprites(L);
  return 1;
}

int App_get_fgColor(lua_State* L)
{
  push_obj<app::Color>(L, Preferences::instance().colorBar.fgColor());
  return 1;
}

int App_set_fgColor(lua_State* L)
{
  Preferences::instance().colorBar.fgColor(convert_args_into_color(L, 2));
  return 0;
}

int App_get_bgColor(lua_State* L)
{
  push_obj<app::Color>(L, Preferences::instance().colorBar.bgColor());
  return 1;
}

int App_set_bgColor(lua_State* L)
{
  Preferences::instance().colorBar.bgColor(convert_args_into_color(L, 2));
  return 0;
}

int App_get_site(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  push_obj(L, site);
  return 1;
}

int App_get_range(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  Site site = ctx->activeSite();
  push_doc_range(L, site);
  return 1;
}

int App_get_isUIAvailable(lua_State* L)
{
  app::Context* ctx = App::instance()->context();
  lua_pushboolean(L, ctx && ctx->isUIAvailable());
  return 1;
}

int App_get_version(lua_State* L)
{
  std::string ver = get_app_version();
  base::replace_string(ver, "-x64", ""); // Remove "-x64" suffix
  push_version(L, base::Version(ver));
  return 1;
}

int App_get_apiVersion(lua_State* L)
{
  lua_pushinteger(L, API_VERSION);
  return 1;
}

int App_get_activeTool(lua_State* L)
{
  tools::Tool* tool = App::instance()->activeToolManager()->activeTool();
  push_tool(L, tool);
  return 1;
}

int App_get_activeBrush(lua_State* L)
{
#if ENABLE_UI
  App* app = App::instance();
  if (app->isGui()) {
    doc::BrushRef brush = app->contextBar()->activeBrush();
    push_brush(L, brush);
    return 1;
  }
#endif
  push_brush(L, doc::BrushRef(new doc::Brush()));
  return 1;
}

int App_get_defaultPalette(lua_State* L)
{
  const Palette* pal = get_default_palette();
  if (pal)
    push_palette(L, new Palette(*pal));
  else
    lua_pushnil(L);
  return 1;
}

int App_set_activeSprite(lua_State* L)
{
  auto sprite = get_docobj<Sprite>(L, 2);
  app::Context* ctx = App::instance()->context();
  doc::Document* doc = sprite->document();
  ctx->setActiveDocument(static_cast<Doc*>(doc));
  return 0;
}

int App_set_activeLayer(lua_State* L)
{
  auto layer = get_docobj<Layer>(L, 2);
  app::Context* ctx = App::instance()->context();
  ctx->setActiveLayer(layer);
  return 0;
}

int App_set_activeFrame(lua_State* L)
{
  const doc::frame_t frame = get_frame_number_from_arg(L, 2);
  app::Context* ctx = App::instance()->context();
  ctx->setActiveFrame(frame);
  return 0;
}

int App_set_activeCel(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 2);
  app::Context* ctx = App::instance()->context();
  ctx->setActiveLayer(cel->layer());
  ctx->setActiveFrame(cel->frame());
  return 0;
}

int App_set_activeImage(lua_State* L)
{
  const auto cel = get_image_cel_from_arg(L, 2);
  if (!cel)
    return 0;

  app::Context* ctx = App::instance()->context();
  ctx->setActiveLayer(cel->layer());
  ctx->setActiveFrame(cel->frame());
  return 0;
}

int App_set_activeTool(lua_State* L)
{
  if (auto tool = get_tool_from_arg(L, 2))
    App::instance()->activeToolManager()->setSelectedTool(tool);
  return 0;
}

int App_set_activeBrush(lua_State* L)
{
#if ENABLE_UI
  if (auto brush = get_brush_from_arg(L, 2)) {
    App* app = App::instance();
    if (app->isGui())
      app->contextBar()->setActiveBrush(brush);
  }
#endif
  return 0;
}

int App_set_defaultPalette(lua_State* L)
{
  if (const doc::Palette* pal = get_palette_from_arg(L, 2))
    set_default_palette(pal);
  return 0;
}

const luaL_Reg App_methods[] = {
  { "open",        App_open },
  { "exit",        App_exit },
  { "transaction", App_transaction },
  { "undo",        App_undo },
  { "redo",        App_redo },
  { "alert",       App_alert },
  { "refresh",     App_refresh },
  { "useTool",     App_useTool },
  { nullptr,       nullptr }
};

const Property App_properties[] = {
  { "activeSprite", App_get_activeSprite, App_set_activeSprite },
  { "activeLayer", App_get_activeLayer, App_set_activeLayer },
  { "activeFrame", App_get_activeFrame, App_set_activeFrame },
  { "activeCel", App_get_activeCel, App_set_activeCel },
  { "activeImage", App_get_activeImage, App_set_activeImage },
  { "activeTag", App_get_activeTag, nullptr },
  { "activeTool", App_get_activeTool, App_set_activeTool },
  { "activeBrush", App_get_activeBrush, App_set_activeBrush },
  { "sprites", App_get_sprites, nullptr },
  { "fgColor", App_get_fgColor, App_set_fgColor },
  { "bgColor", App_get_bgColor, App_set_bgColor },
  { "version", App_get_version, nullptr },
  { "apiVersion", App_get_apiVersion, nullptr },
  { "site", App_get_site, nullptr },
  { "range", App_get_range, nullptr },
  { "isUIAvailable", App_get_isUIAvailable, nullptr },
  { "defaultPalette", App_get_defaultPalette, App_set_defaultPalette },
  { nullptr, nullptr, nullptr }
};

} // anonymous namespace

DEF_MTNAME(App);

void register_app_object(lua_State* L)
{
  REG_CLASS(L, App);
  REG_CLASS_PROPERTIES(L, App);

  lua_newtable(L);              // Create a table which will be the "app" object
  lua_pushvalue(L, -1);
  luaL_getmetatable(L, get_mtname<App>());
  lua_setmetatable(L, -2);
  lua_setglobal(L, "app");
  lua_pop(L, 1);                // Pop app table
}

void set_app_params(lua_State* L, const Params& params)
{
  lua_getglobal(L, "app");
  lua_newtable(L);
  for (const auto& kv : params) {
    lua_pushstring(L, kv.second.c_str());
    lua_setfield(L, -2, kv.first.c_str());
  }
  lua_setfield(L, -2, "params");
  lua_pop(L, 1);
}

} // namespace script
} // namespace app
