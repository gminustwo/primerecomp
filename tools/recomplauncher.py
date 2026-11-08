#!/usr/bin/env python3
"""Release-owned RecompLauncher format-1 manifest (PR #12 AppImages, PR #22 inputs)."""
import argparse
import json
from pathlib import Path


def manifest(platform, program):
    root = "{install}" if platform == "windows" else "{local}/primerecomp"
    settings = []

    def choices(key, label, values, default, group="Video", help="", advanced=False):
        settings.append(dict(id=key, label=label, group=group, help=help, advanced=advanced,
                             choices=[dict(label=name, set={key: str(value)}) for name, value in values],
                             defaults={key: str(default)}))

    def toggle(key, label, default, group="Video", **kw):
        choices(key, label, [("Off", 0), ("On", 1)], int(default), group, **kw)

    def slider(key, label, lo, hi, step, default, group, fmt="%g", advanced=False):
        settings.append(dict(id=key, label=label, group=group, advanced=advanced,
                             slider=dict(key=key, min=lo, max=hi, step=step, format=fmt),
                             defaults={key: str(default)}))

    toggle("fullscreen", "Full screen", False)
    choices("resolution", "Resolution", [("Match display", "match"), ("Dynamic", "dynamic"),
            ("25%", "25%"), ("50%", "50%"), ("75%", "75%"), ("900p (Switch)", "900p"),
            ("1080p", "1080p"), ("1440p", "1440p"), ("4K", "2160p"), ("8K", "4320p")], "match")
    choices("frame_rate", "Frame rate", [("30 fps", 30), ("60 fps (original)", 60), ("90 fps", 90),
            ("120 fps", 120)], 60)
    toggle("vsync", "V-Sync", True)
    choices("aspect", "Screen shape", [("Fill window", "unlocked"), ("Original (16:9)", "original")], "unlocked")
    choices("upscaler", "Upscaling", [("Bilinear", "bilinear"), ("AMD FSR 1", "fsr1")], "fsr1")
    choices("antialias", "Anti-aliasing", [("Off", "off"), ("SMAA", "smaa")], "off")
    choices("aniso", "Texture filtering", [("Game's own", 0), ("2x", 2), ("4x", 4), ("8x", 8), ("16x", 16)], 16)
    toggle("preload_shaders", "Preload shaders", True,
           help="Build shaders before playing when starting from PrimeRecomp's start screen.")
    toggle("show_fps", "Show FPS", False)
    choices("hud", "HUD placement", [("Window edges", "edges"), ("21:9", "21:9"), ("Middle 16:9", "middle")], "edges")
    choices("pause", "Pause screen", [("Fill window", "fill"), ("Middle 16:9", "middle")], "fill")
    toggle("hdr", "HDR", False)
    slider("hdr_peak_nits", "HDR peak brightness", 400, 4000, 50, 1000, "Video", advanced=True)
    slider("hdr_paper_white_nits", "HDR paper white", 80, 500, 10, 200, "Video", advanced=True)
    slider("fov", "Field of view", 45, 90, 1, 55, "Video")
    slider("weapon_fov", "Weapon field of view", 45, 90, 1, 55, "Video")
    slider("volume", "Volume", 0, 2, .05, 1, "Audio", "%.2f")
    choices("speakers", "Speakers", [("Auto", "auto"), ("Stereo", "stereo"), ("5.1", "5.1"), ("7.1", "7.1")], "auto", "Audio")
    toggle("reverb", "Room reverb", True, "Audio")
    toggle("movie_audio", "Movie audio", True, "Audio")
    toggle("rumble", "Rumble", True, "Controls")
    slider("rumble_strength", "Rumble strength", 0, 2, .05, 1, "Controls", "%.2f")
    toggle("hd_rumble", "HD rumble", True, "Controls")
    slider("deadzone_left", "Left stick dead zone", 0, .30, .01, .05, "Controls", "%.2f")
    slider("deadzone_right", "Right stick dead zone", 0, .30, .01, .05, "Controls", "%.2f")
    toggle("gyro", "Gyro", True, "Controls")
    toggle("mouse_aim", "Mouse aim", True, "Controls")
    slider("mouse_sens", "Mouse sensitivity", .1, 10, .05, 1, "Controls", "%.2f")
    toggle("mouse_invert", "Invert mouse", False, "Controls")
    toggle("flick_stick", "Flick stick", False, "Controls")
    choices("face_buttons", "Face buttons", [("Labels", "labels"), ("Nintendo", "nintendo"), ("Xbox", "xbox")], "labels", "Controls")
    choices("button_icons", "Button icons", [("Auto", "auto"), ("Switch", "switch"), ("Xbox", "xbox"),
            ("PlayStation", "playstation"), ("Keyboard", "keyboard"), ("Wii", "wii")], "auto", "Controls")
    toggle("wiimote", "Wii Remote", True, "Controls", advanced=True)
    slider("wiimote_area", "Wii pointer area", .2, 1, .05, .5, "Controls", "%.2f", advanced=True)
    slider("wiimote_smooth", "Wii pointer smoothing", 0, .9, .05, .3, "Controls", "%.2f", advanced=True)
    toggle("steam_controller_direct", "Direct Steam Controller input", False, "Controls", advanced=True)
    choices("language", "Language", list(zip(["Auto", "Japanese", "English", "Spanish", "French", "German", "Italian",
            "Dutch", "Korean", "Chinese (Simplified)", "Chinese (Traditional)"],
            ["auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ko", "zh-CN", "zh-Hant"])), "auto", "Gameplay")
    toggle("skippable_cutscenes", "Skip any cutscene", False, "Gameplay")
    choices("hint_seconds", "Hint timer", [("Game's own", 0), ("1 second", 1), ("30 seconds", 30)] +
            [(f"{m} minutes", m*60) for m in range(1,31)], 0, "Gameplay")
    toggle("hint_on_l3", "Show hint with left stick", True, "Gameplay")
    for key, label in [("infinite_energy", "Infinite energy"), ("infinite_missiles", "Infinite missiles"),
                       ("infinite_power_bombs", "Infinite power bombs")]:
        toggle(key, label, False, "Cheats", advanced=True)
    toggle("texture_mods", "Texture mods", True, "Mods")
    toggle("dump_textures", "Dump textures", False, "Mods", advanced=True)
    toggle("check_updates", "Check for PrimeRecomp updates", True, "Updates",
           help="Applies to PrimeRecomp's own start screen; RecompLauncher manages its updates independently.")
    settings.append({"id": "launch_preload", "label": "Preload before launch", "group": "Video", "in": "args",
                     "help": "Show shader preparation before playing instead of loading alongside the game.",
                     "choices": [{"label": "Off", "args": []}, {"label": "On", "args": ["--preload-shaders"]}]})
    return {
        "$schema": "https://recomplauncher.com/schema/recomplauncher.json",
        "format": 1, "name": "PrimeRecomp", "repo": "gminustwo/primerecomp",
        "inputs": [
            {"id": "base", "label": "Metroid Prime Remastered v1.0.0 (no updates)", "role": "game",
             "formats": [{"type": "folder"}, {"type": "file", "extensions": ["nsp", "xci"], "requires": ["keys"]}]},
            {"id": "keys", "label": "Nintendo Switch prod.keys", "role": "key", "shared": "switch.prod-keys"}],
        "prepare": {"program": [program], "args": ["--prepare", "--game={input:base}", "--keys={input:keys}",
                    f"--data-root={root}"], "label": "Preparing Metroid Prime Remastered", "timeout": 3600},
        "start": {"program": [program], "args": ["--play", f"--data-root={root}"]},
        "config": [{"id": "main", "format": "ini", "file": [{"path": f"{root}/primerecomp.ini"}]}],
        "settings": settings,
        "saves": [f"{root}/save"],
        "folders": {"saves": [{"path": f"{root}/save"}]},
        "actions": [{"id": "saves", "label": "Save folder", "open": "saves"}],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", required=True, choices=("windows", "linux"))
    parser.add_argument("--program", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    data = manifest(args.platform, args.program)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
