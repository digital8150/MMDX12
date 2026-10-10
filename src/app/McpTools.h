#pragma once
#include <json.hpp>
#include <string>

namespace mmdx {

inline const char* kMcpProtocolVersion = "2025-06-18";

inline nlohmann::json GetMcpToolDefinitions() {
    using json = nlohmann::json;
    json tools = json::array();

    // 1. list_instances (bridge-only)
    tools.push_back({
        {"name", "list_instances"},
        {"description", "List running MMDX12 instances listening on named pipes"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 2. connect (bridge-only)
    tools.push_back({
        {"name", "connect"},
        {"description", "Connect to a specific MMDX12 instance pipe"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"pipe_name", {
                    {"type", "string"},
                    {"description", "Named pipe name to connect to (e.g. mmdx12_mcp or mmdx12_mcp_<pid>)"}
                }}
            }}
        }}
    });

    // 3. launch_app (bridge-only)
    tools.push_back({
        {"name", "launch_app"},
        {"description", "Launch MMDX12.exe from the bridge's folder with --mcp and wait until its pipe answers"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"extra_args", {
                    {"type", "string"},
                    {"description", "Optional extra command-line arguments to pass to MMDX12.exe"}
                }}
            }}
        }}
    });

    // 4. get_state
    tools.push_back({
        {"name", "get_state"},
        {"description", "Get current application state (screen, playback, scene, render path, upscaler, fps/gpu ms, window size, studio summary)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 5. get_recent_logs
    tools.push_back({
        {"name", "get_recent_logs"},
        {"description", "Get recent log lines from mmdx12.log ring buffer"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"lines", {
                    {"type", "integer"},
                    {"description", "Number of recent lines to return (default 50)"}
                }},
                {"level", {
                    {"type", "string"},
                    {"enum", {"info", "warn", "error"}},
                    {"description", "Filter logs by level"}
                }}
            }}
        }}
    });

    // 6. list_library
    tools.push_back({
        {"name", "list_library"},
        {"description", "List available library assets (characters, stages, songs) usable by load tools"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"enum", {"all", "character", "stage", "song"}},
                    {"description", "Asset kind to list (default: all)"}
                }},
                {"query", {
                    {"type", "string"},
                    {"description", "Optional substring filter on id or display name"}
                }}
            }}
        }}
    });

    // 7. screenshot
    tools.push_back({
        {"name", "screenshot"},
        {"description", "Capture a screenshot of the current back buffer and return as inline PNG base64 image"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"max_width", {
                    {"type", "integer"},
                    {"description", "Maximum width in pixels for downscaling (default 1280)"}
                }}
            }}
        }}
    });

    // 8. wait_frames
    tools.push_back({
        {"name", "wait_frames"},
        {"description", "Wait for N frames to render"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"n", {
                    {"type", "integer"},
                    {"description", "Number of frames to wait (default 1)"}
                }}
            }}
        }}
    });

    // 9. set_screen
    tools.push_back({
        {"name", "set_screen"},
        {"description", "Switch active screen: select, studio, shaders, or bench"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"screen", {
                    {"type", "string"},
                    {"enum", {"select", "studio", "shaders", "bench"}},
                    {"description", "Target screen name"}
                }}
            }},
            {"required", {"screen"}}
        }}
    });

    // 10. load_scene
    tools.push_back({
        {"name", "load_scene"},
        {"description", "Load a scene with character, stage, and song by substring/ID match"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"character", {
                    {"type", "string"},
                    {"description", "Character name or ID substring"}
                }},
                {"stage", {
                    {"type", "string"},
                    {"description", "Stage name or ID substring (or 'none')"}
                }},
                {"song", {
                    {"type", "string"},
                    {"description", "Song name or ID substring"}
                }}
            }},
            {"required", {"character"}}
        }}
    });

    // 11. open_studio
    tools.push_back({
        {"name", "open_studio"},
        {"description", "Open Studio editor with character, stage, song, or an existing .mmdxproj project file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"character", {
                    {"type", "string"},
                    {"description", "Character name or ID substring"}
                }},
                {"stage", {
                    {"type", "string"},
                    {"description", "Stage name or ID substring"}
                }},
                {"song", {
                    {"type", "string"},
                    {"description", "Song name or ID substring"}
                }},
                {"project", {
                    {"type", "string"},
                    {"description", "Path to an existing .mmdxproj file"}
                }}
            }}
        }}
    });

    // 12. play
    tools.push_back({
        {"name", "play"},
        {"description", "Start or resume playback (in play mode or studio)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 13. pause
    tools.push_back({
        {"name", "pause"},
        {"description", "Pause playback (in play mode or studio)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 14. seek
    tools.push_back({
        {"name", "seek"},
        {"description", "Seek playback to specified seconds"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"seconds", {
                    {"type", "number"},
                    {"description", "Target time in seconds"}
                }}
            }},
            {"required", {"seconds"}}
        }}
    });

    // 15. get_render_settings
    tools.push_back({
        {"name", "get_render_settings"},
        {"description", "Get current render and graphics settings"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 16. set_render_settings
    tools.push_back({
        {"name", "set_render_settings"},
        {"description", "Change render and graphics settings"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"render_path", {
                    {"type", "string"},
                    {"enum", {"raster", "rt", "pt"}},
                    {"description", "Render path: raster, rt (ray traced), or pt (path traced)"}
                }},
                {"upscaler", {
                    {"type", "string"},
                    {"enum", {"none", "dlss", "fsr", "xess"}},
                    {"description", "Upscaler: none, dlss, fsr, xess"}
                }},
                {"upscale_quality", {
                    {"type", "string"},
                    {"enum", {"native", "quality", "balanced", "performance", "ultra"}},
                    {"description", "Upscaler quality mode"}
                }},
                {"lighting_preset", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 3},
                    {"description", "Lighting preset (0..3)"}
                }},
                {"quality", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 3},
                    {"description", "Quality preset (0: low, 1: medium, 2: high, 3: ultra)"}
                }},
                {"dof", {
                    {"type", "boolean"},
                    {"description", "Depth of field toggle"}
                }},
                {"volumetric", {
                    {"type", "boolean"},
                    {"description", "Volumetric fog / shafts toggle"}
                }},
                {"bloom_conv", {
                    {"type", "boolean"},
                    {"description", "FFT convolution bloom toggle"}
                }},
                {"shader_pack", {
                    {"type", "string"},
                    {"description", "Shader pack ID for the selected character, or 'default'"}
                }},
                {"effects", {
                    {"type", "string"},
                    {"description", "Comma-separated effect pack IDs in stack order, or 'none'. Effects already in the stack keep their parameters; new ones start at the pack defaults"}
                }},
                {"effect_stack", {
                    {"type", "array"},
                    {"description", "The whole effect stack with parameters, in order (replaces it; use instead of effects). Parameter keys are the pack's pack.json params; values are clamped to their ranges, missing keys use the defaults"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"id", {{"type", "string"}, {"description", "Effect pack id"}}},
                            {"enabled", {{"type", "boolean"}, {"description", "Default true"}}},
                            {"params", {{"type", "object"}, {"additionalProperties", {{"type", "number"}}}, {"description", "Parameter key -> value"}}},
                            {"texture_folder", {{"type", "string"}, {"description", "Per-entry texture folder (empty = the pack-level folder)"}}}
                        }},
                        {"required", {"id"}}
                    }}
                }}
            }}
        }}
    });

    // 17. studio_get_state
    tools.push_back({
        {"name", "studio_get_state"},
        {"description", "Get detailed Studio editor state (frame, selection, key counts, undo/redo, models, lights)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 18. studio_command
    tools.push_back({
        {"name", "studio_command"},
        {"description", "Run any existing ui-script studio command (e.g. studionew, studioadd, etc.)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"cmd", {
                    {"type", "string"},
                    {"description", "Command name (e.g. studionew, studiosave, etc.)"}
                }},
                {"args", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Command arguments array"}
                }}
            }},
            {"required", {"cmd"}}
        }}
    });

    // 19. studio_save
    tools.push_back({
        {"name", "studio_save"},
        {"description", "Save current studio project to a .mmdxproj file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file", {
                    {"type", "string"},
                    {"description", "Target file path"}
                }}
            }},
            {"required", {"file"}}
        }}
    });

    // 20. studio_open
    tools.push_back({
        {"name", "studio_open"},
        {"description", "Open a .mmdxproj project file in Studio"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file", {
                    {"type", "string"},
                    {"description", "Source file path"}
                }}
            }},
            {"required", {"file"}}
        }}
    });

    // 21. studio_add_model
    tools.push_back({
        {"name", "studio_add_model"},
        {"description", "Add a character, stage, or prop model to Studio"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"enum", {"character", "stage", "prop"}},
                    {"description", "Model kind"}
                }},
                {"file", {
                    {"type", "string"},
                    {"description", "Explicit file path"}
                }},
                {"library", {
                    {"type", "string"},
                    {"description", "Library substring match"}
                }}
            }},
            {"required", {"kind"}}
        }}
    });

    // 22. studio_select
    tools.push_back({
        {"name", "studio_select"},
        {"description", "Select model in Studio by index (-1 for none / camera)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"index", {
                    {"type", "integer"},
                    {"description", "Model index"}
                }}
            }},
            {"required", {"index"}}
        }}
    });

    // 23. studio_set_camera
    tools.push_back({
        {"name", "studio_set_camera"},
        {"description", "Set the studio view (target tx/ty/tz, yaw/pitch degrees, distance, fov). Without key: the free view (the motion camera is switched off in the viewport). With key: true: writes the motion camera key at the playhead (inserted first if missing) and views through the motion camera"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"tx", {{"type", "number"}}},
                {"ty", {{"type", "number"}}},
                {"tz", {{"type", "number"}}},
                {"yaw_deg", {{"type", "number"}}},
                {"pitch_deg", {{"type", "number"}}},
                {"dist", {{"type", "number"}}},
                {"fov_deg", {{"type", "number"}}},
                {"key", {{"type", "boolean"}, {"description", "Insert keyframe at current frame"}}}
            }}
        }}
    });

    // 24. studio_set_bone
    tools.push_back({
        {"name", "studio_set_bone"},
        {"description", "Set bone transform on the selected Studio model and optionally insert a keyframe"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"bone", {
                    {"type", "string"},
                    {"description", "Bone name"}
                }},
                {"translate", {
                    {"type", "array"},
                    {"items", {{"type", "number"}}},
                    {"minItems", 3},
                    {"maxItems", 3},
                    {"description", "[x, y, z] translation offset"}
                }},
                {"rotate_deg", {
                    {"type", "array"},
                    {"items", {{"type", "number"}}},
                    {"minItems", 3},
                    {"maxItems", 3},
                    {"description", "[x, y, z] rotation in degrees"}
                }},
                {"key", {
                    {"type", "boolean"},
                    {"description", "Whether to insert a keyframe at the current frame"}
                }}
            }},
            {"required", {"bone"}}
        }}
    });

    // 25. studio_key
    tools.push_back({
        {"name", "studio_key"},
        {"description", "Insert keyframe(s) in Studio at the current frame"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"what", {
                    {"type", "string"},
                    {"enum", {"all", "selected", "camera", "lights"}},
                    {"description", "selected = the selected rows / bones of the selected character, all = every bone of it, camera / lights = their tracks (default: selected)"}
                }}
            }}
        }}
    });

    // 26. undo
    tools.push_back({
        {"name", "undo"},
        {"description", "Undo the last Studio edit action"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 27. redo
    tools.push_back({
        {"name", "redo"},
        {"description", "Redo the last undone Studio action"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 28. render_still
    tools.push_back({
        {"name", "render_still"},
        {"description", "Start offline still image rendering with the GI renderer"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"output", {
                    {"type", "string"},
                    {"description", "Output PNG path"}
                }},
                {"spp", {
                    {"type", "integer"},
                    {"description", "Samples per pixel"}
                }},
                {"width", {
                    {"type", "integer"},
                    {"description", "Image width override"}
                }},
                {"height", {
                    {"type", "integer"},
                    {"description", "Image height override"}
                }}
            }},
            {"required", {"output"}}
        }}
    });

    // 29. render_video
    tools.push_back({
        {"name", "render_video"},
        {"description", "Start offline video render to an MP4 file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"output", {
                    {"type", "string"},
                    {"description", "Output MP4 path"}
                }},
                {"start", {
                    {"type", "number"},
                    {"description", "Start time in seconds"}
                }},
                {"end", {
                    {"type", "number"},
                    {"description", "End time in seconds"}
                }},
                {"renderer", {
                    {"type", "string"},
                    {"enum", {"raster", "rt", "pt", "gi"}},
                    {"description", "Renderer type"}
                }},
                {"fps", {{"type", "integer"}}},
                {"bitrate", {{"type", "integer"}}},
                {"quality", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
                {"spp", {{"type", "integer"}}},
                {"width", {{"type", "integer"}}},
                {"height", {{"type", "integer"}}}
            }},
            {"required", {"output"}}
        }}
    });

    // 30. render_status
    tools.push_back({
        {"name", "render_status"},
        {"description", "Query current offline render status and progress"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 31. render_cancel
    tools.push_back({
        {"name", "render_cancel"},
        {"description", "Cancel the currently active offline render"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 32. ui_input
    tools.push_back({
        {"name", "ui_input"},
        {"description", "Inject UI input events (move, click, dblclick, down, up, wheel, key, text, mod)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"events", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"frame", {{"type", "integer"}, {"description", "Frame offset relative to now (0 = current frame)"}}},
                            {"cmd", {{"type", "string"}, {"description", "Input command (move, click, dblclick, down, up, wheel, key, text, mod)"}}},
                            {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}}
                        }},
                        {"required", {"cmd"}}
                    }}
                }}
            }},
            {"required", {"events"}}
        }}
    });

    // 33. quit_app
    tools.push_back({
        {"name", "quit_app"},
        {"description", "Cleanly close and exit MMDX12"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // Tools that leave the current screen: an open studio project with unsaved changes is kept unless the agent says so.
    for (auto& t : tools) {
        const std::string n = t["name"];
        if (n == "set_screen" || n == "load_scene" || n == "open_studio" || n == "studio_open" || n == "studio_command")
            t["inputSchema"]["properties"]["discard_unsaved"] = {
                {"type", "boolean"},
                {"description", "Leave a studio project with unsaved changes anyway (default false: the call fails instead)"}};
        if (n == "render_still" || n == "render_video") {
            t["description"] = t["description"].get<std::string>() +
                               ". Returns once the render has started (the app keeps running); poll render_status. "
                               "Without 'output' the file goes to the app's usual render folder.";
            t["inputSchema"].erase("required");
        }
    }
    return tools;
}

// How long the app may take to answer a tool call (the app's worker waits this long for the main thread; the
// bridge waits a little longer so the app's own timeout error arrives first).
inline unsigned McpToolTimeoutMs(const std::string& tool) {
    if (tool == "load_scene" || tool == "open_studio" || tool == "studio_open" || tool == "studio_add_model" ||
        tool == "studio_command")
        return 180000;
    if (tool == "wait_frames" || tool == "ui_input") return 120000;
    return 30000;
}

} // namespace mmdx
