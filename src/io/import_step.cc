/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright The OpenSCAD Developers.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#include "io/import.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "geometry/PolySet.h"
#include "utils/printutils.h"

namespace fs = std::filesystem;

std::unique_ptr<PolySet> import_step(const std::string& filename, const Location& loc)
{
  // Convert STEP to STL using Python + OpenCASCADE (OCP)
  // This avoids linking OCCT directly in C++

  fs::path stepPath(filename);
  if (!fs::exists(stepPath)) {
    LOG(message_group::Error,
        "STEP import: file not found '%1$s', import() at line %2$d",
        filename, loc.firstLine());
    return PolySet::createEmpty();
  }

  // Create temp STL path
  auto tempDir = fs::temp_directory_path();
  auto stlPath = tempDir / (stepPath.stem().string() + "_step_import.stl");

  // Python conversion script
  std::string pythonScript =
    "import sys\n"
    "try:\n"
    "    from OCP.STEPControl import STEPControl_Reader\n"
    "    from OCP.BRepMesh import BRepMesh_IncrementalMesh\n"
    "    from OCP.StlAPI import StlAPI_Writer\n"
    "    reader = STEPControl_Reader()\n"
    "    if reader.ReadFile(sys.argv[1]) != 1:\n"
    "        print('STEP read failed', file=sys.stderr)\n"
    "        sys.exit(1)\n"
    "    reader.TransferRoots()\n"
    "    shape = reader.OneShape()\n"
    "    mesh = BRepMesh_IncrementalMesh(shape, 0.1)\n"
    "    mesh.Perform()\n"
    "    writer = StlAPI_Writer()\n"
    "    writer.ASCIIMode = False\n"
    "    writer.Write(shape, sys.argv[2])\n"
    "except ImportError:\n"
    "    print('STEP import requires cadquery-ocp: pip install cadquery-ocp', file=sys.stderr)\n"
    "    sys.exit(1)\n";

  // Write temp script
  auto scriptPath = tempDir / "openscad_step_convert.py";
  {
    std::ofstream script(scriptPath);
    script << pythonScript;
  }

  // Run Python conversion
  std::string cmd = "python3 \"" + scriptPath.string() + "\" \"" +
                    filename + "\" \"" + stlPath.string() + "\" 2>&1";

  LOG("STEP import: converting '%1$s' via OpenCASCADE...", filename);

  int ret = std::system(cmd.c_str());
  fs::remove(scriptPath);

  if (ret != 0 || !fs::exists(stlPath)) {
    LOG(message_group::Error,
        "STEP import: conversion failed for '%1$s'. "
        "Ensure cadquery-ocp is installed: pip install cadquery-ocp. "
        "import() at line %2$d",
        filename, loc.firstLine());
    return PolySet::createEmpty();
  }

  LOG("STEP import: conversion complete, loading mesh...");

  // Import the converted STL
  auto result = import_stl(stlPath.string(), loc);

  // Clean up temp file
  fs::remove(stlPath);

  return result;
}
