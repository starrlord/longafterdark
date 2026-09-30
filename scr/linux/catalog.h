// catalog-win.json (DESIGN.md §6a), the fields the player needs: which
// modules there are, where their files are, and what screen they get.
#pragma once

#include <string>
#include <vector>

#include "geometry.h"

namespace lad {

struct Module {
  std::string id;              // "ad40.toasters"
  std::string display_name;    // "Flying Toasters"
  std::string name;            // moduleName, else displayName
  std::string lane;            // "pe32" | "ne16"
  std::string abi = "afterdark";   // or "intermission" (Star Wars Screen Entertainment)
  SizeI screen;                // catalog "screen" ("640x480": Star Trek, ScreamSavers, Marvel), {0, 0} when none
  std::string path;            // relative to the win dir, forward slashes
  std::string package, package_title;
  std::string same_as;         // the id of the first module with the same bytes, or ""
};

struct Catalog {
  std::vector<Module> modules;
  const Module* find(const std::string& id) const;
};

// A catalog "screen": 1 to 5 decimal digits either side of an 'x' (or
// 'X'), each axis 1..8192 and at most 4096x4096 pixels in all; anything
// else is no screen ({0, 0}), as the Windows saver's catalog parser rules.
SizeI screen_of(const std::string& s);

// False (with *error) when the file can't be read or isn't a catalog.
// Entries without an id or a path are skipped.
bool load_catalog(const std::string& path, Catalog& out, std::string* error);

// The module a name on the command line means:
//  1. an id, exactly (case-insensitive): "ad40.toasters";
//  2. a catalog path, exactly (case-insensitive, either slash);
//  3. a display or module name, exactly (case-insensitive): "Flying Toasters";
//  4. the part of an id after its release: "toasters" for "ad40.toasters".
// The first rule that matches decides; a rule that matches several modules
// (Flying Toasters is in several releases) makes the name ambiguous: null,
// with the candidates in *ambiguous. Nothing matching: null, none.
const Module* resolve_module(const Catalog& c, const std::string& name, std::vector<const Module*>* ambiguous);

}  // namespace lad
