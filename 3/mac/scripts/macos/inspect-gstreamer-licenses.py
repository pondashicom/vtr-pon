#!/usr/bin/env python3
"""Read installed GStreamer registry licenses and the local FFmpeg build license.

Metadata inspection only: no element, pipeline, device or media file is opened.
Uses an isolated registry; installed plugin metadata is not a release selection.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes as C
import json
import os
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--homebrew-prefix", type=Path, default=Path("/opt/homebrew"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix, output = args.homebrew_prefix.resolve(), args.output.resolve()
    if output == prefix or prefix in output.parents or any(p.suffix in (".app", ".framework") for p in output.parents):
        parser.error("output must be outside installed libraries and app bundles")
    output.parent.mkdir(parents=True, exist_ok=True)
    os.environ["GST_REGISTRY"] = str(output.with_suffix(".registry.bin"))
    os.environ["GST_PLUGIN_SYSTEM_PATH_1_0"] = str(prefix / "lib/gstreamer-1.0")
    os.environ["GST_PLUGIN_PATH_1_0"] = ""
    os.environ["GST_PLUGIN_PATH"] = ""
    lib = C.CDLL(str(prefix / "lib/libgstreamer-1.0.0.dylib"))
    lib.gst_init.argtypes = [C.c_void_p, C.c_void_p]
    lib.gst_init.restype = None
    lib.gst_registry_get.argtypes = []
    lib.gst_registry_get.restype = C.c_void_p
    lib.gst_registry_get_plugin_list.argtypes = [C.c_void_p]
    lib.gst_registry_get_plugin_list.restype = C.c_void_p

    class GList(C.Structure):
        pass

    GList._fields_ = [("data", C.c_void_p), ("next", C.POINTER(GList)), ("prev", C.POINTER(GList))]
    fields = ["name", "filename", "version", "license", "source", "package", "origin", "description"]
    for key in fields:
        function = getattr(lib, "gst_plugin_get_" + key)
        function.argtypes = [C.c_void_p]
        function.restype = C.c_char_p
    lib.gst_plugin_list_free.argtypes = [C.c_void_p]
    lib.gst_plugin_list_free.restype = None
    lib.gst_init(None, None)
    head = lib.gst_registry_get_plugin_list(lib.gst_registry_get())
    node, plugins = C.cast(head, C.POINTER(GList)), []
    try:
        while node:
            values = {}
            for key in fields:
                data = getattr(lib, "gst_plugin_get_" + key)(node.contents.data)
                values[key] = data.decode(errors="replace") if data else None
            plugins.append(values)
            node = node.contents.next
    finally:
        lib.gst_plugin_list_free(head)
    ffmpeg = {"path": str(prefix / "opt/ffmpeg/lib/libavcodec.dylib")}
    try:
        codec = C.CDLL(ffmpeg["path"])
        codec.avcodec_license.argtypes = []
        codec.avcodec_license.restype = C.c_char_p
        codec.avcodec_configuration.argtypes = []
        codec.avcodec_configuration.restype = C.c_char_p
        ffmpeg.update(license=codec.avcodec_license().decode(), configuration=codec.avcodec_configuration().decode())
    except OSError as error:
        ffmpeg["inspection_error"] = str(error)
    record = {"plugin_count": len(plugins), "plugins": sorted(plugins, key=lambda p: p["name"]),
              "ffmpeg": ffmpeg, "note": "Installed metadata only; not playback selection or release payload."}
    output.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"registry_plugins": len(plugins), "plugin_licenses": dict(Counter(p["license"] for p in plugins)),
                      "ffmpeg_runtime_license": ffmpeg.get("license")}, indent=2))


if __name__ == "__main__":
    main()
