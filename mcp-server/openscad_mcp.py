#!/usr/bin/env python3
"""OpenSCAD MCP Server — bridges Claude Code to OpenSCAD's embedded HTTP API."""

import base64
import os
import sys

import requests
from mcp.server.fastmcp import FastMCP, Image

app = FastMCP(
    name="openscad",
    instructions=(
        "OpenSCAD remote control. Use these tools to read/write editor content, "
        "view the 3D viewport, trigger compiles, and manage files in OpenSCAD."
    ),
)

BASE_URL = os.environ.get("OPENSCAD_API_URL", "http://127.0.0.1:8765")
AUTH_TOKEN = os.environ.get("OPENSCAD_API_TOKEN", "")
TIMEOUT = 15


def _headers():
    h = {"Content-Type": "application/json"}
    if AUTH_TOKEN:
        h["Authorization"] = f"Bearer {AUTH_TOKEN}"
    return h


def _get(path: str) -> dict:
    r = requests.get(f"{BASE_URL}{path}", headers=_headers(), timeout=TIMEOUT)
    r.raise_for_status()
    return r.json()


def _post(path: str, data: dict | None = None) -> dict:
    r = requests.post(
        f"{BASE_URL}{path}", headers=_headers(), json=data or {}, timeout=TIMEOUT
    )
    r.raise_for_status()
    return r.json()


def _get_raw(path: str) -> bytes:
    r = requests.get(f"{BASE_URL}{path}", headers=_headers(), timeout=TIMEOUT)
    r.raise_for_status()
    return r.content


# --- Read tools ---


@app.tool()
def get_editor() -> dict:
    """Get the current OpenSCAD editor content and file path."""
    return _get("/api/editor")


@app.tool()
def get_console() -> dict:
    """Get the console output text (compile messages, errors, warnings)."""
    return _get("/api/console")


@app.tool()
def get_viewport_screenshot() -> Image:
    """Capture a screenshot of the 3D viewport as a PNG image."""
    png_data = _get_raw("/api/viewport/screenshot")
    return Image(data=png_data, format="png")


@app.tool()
def get_camera() -> dict:
    """Get the current viewport camera state: position (vpt), rotation (vpr), distance (vpd), FOV (vpf)."""
    return _get("/api/viewport/camera")


@app.tool()
def get_compile_status() -> dict:
    """Get the current compile error and warning counts."""
    return _get("/api/compile/status")


@app.tool()
def get_status() -> dict:
    """Get overall OpenSCAD app state: editor info, compile status, camera, and whether geometry is rendered."""
    return _get("/api/status")


# --- Write tools ---


@app.tool()
def set_editor(code: str) -> dict:
    """Replace the entire editor content with new OpenSCAD code."""
    return _post("/api/editor", {"code": code})


@app.tool()
def insert_text(text: str) -> dict:
    """Insert text at the current cursor position in the editor."""
    return _post("/api/editor/insert", {"text": text})


@app.tool()
def preview() -> dict:
    """Trigger F5 preview compile. Returns immediately; compile runs in background."""
    return _post("/api/compile/preview")


@app.tool()
def render() -> dict:
    """Trigger F6 full CGAL render. Required before export. Returns immediately."""
    return _post("/api/compile/render")


@app.tool()
def set_camera(
    vpt: list[float] | None = None,
    vpr: list[float] | None = None,
    vpd: float | None = None,
    vpf: float | None = None,
) -> dict:
    """Set camera parameters. All optional: vpt=[x,y,z] position, vpr=[rx,ry,rz] rotation, vpd=distance, vpf=FOV."""
    data = {}
    if vpt is not None:
        data["vpt"] = vpt
    if vpr is not None:
        data["vpr"] = vpr
    if vpd is not None:
        data["vpd"] = vpd
    if vpf is not None:
        data["vpf"] = vpf
    return _post("/api/viewport/camera", data)


@app.tool()
def view_all() -> dict:
    """Reset viewport to show the entire model (zoom to fit)."""
    return _post("/api/viewport/viewall")


# --- File tools ---


@app.tool()
def save_file() -> dict:
    """Save the current file. If untitled, will trigger save-as dialog in OpenSCAD."""
    return _post("/api/file/save")


@app.tool()
def save_file_as(path: str) -> dict:
    """Save the current editor content to a specific file path."""
    return _post("/api/file/saveas", {"path": path})


@app.tool()
def open_file(path: str) -> dict:
    """Open a .scad file in the editor."""
    return _post("/api/file/open", {"path": path})


@app.tool()
def export_model(format: str, path: str) -> dict:
    """Export the rendered model to a file. Requires F6 render first.
    Format: stl, off, amf, 3mf, obj, dxf, svg."""
    return _post("/api/export", {"format": format, "path": path})


if __name__ == "__main__":
    app.run()
