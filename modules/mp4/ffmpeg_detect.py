"""Locates the FFmpeg libraries needed by the mp4 module.

On Linux/BSD this uses pkg-config. Some systems ship an FFmpeg compatibility
package (e.g. ffmpeg4.4 on Arch) that takes over via PKG_CONFIG_PATH; if the
version found that way is too old, the probe retries with PKG_CONFIG_PATH
stripped so the real system FFmpeg is used.

On Windows/macOS, set the FFMPEG_PATH environment variable to a prebuilt
FFmpeg directory containing include/ and lib/.
"""

import os
import shutil
import subprocess

FFMPEG_PKGS = ["libavformat", "libavcodec", "libavutil", "libswscale", "libswresample"]
FFMPEG_LIBS = ["avformat", "avcodec", "avutil", "swscale", "swresample"]

# libavformat 59 corresponds to FFmpeg 5.x; the module uses the modern
# channel layout API which requires FFmpeg 5.1+.
MIN_AVFORMAT_VERSION = "59"


def _pkg_config_env():
    if shutil.which("pkg-config") is None:
        return None
    candidates = [os.environ.copy()]
    if "PKG_CONFIG_PATH" in os.environ:
        stripped = os.environ.copy()
        del stripped["PKG_CONFIG_PATH"]
        candidates.append(stripped)
    for candidate in candidates:
        try:
            subprocess.check_call(
                ["pkg-config", "--exists"] + FFMPEG_PKGS,
                env=candidate,
            )
            subprocess.check_call(
                ["pkg-config", "--atleast-version=" + MIN_AVFORMAT_VERSION, "libavformat"],
                env=candidate,
            )
            return candidate
        except (subprocess.CalledProcessError, OSError):
            continue
    return None


def find_ffmpeg(platform):
    """Returns a dict with CFLAGS/LIBFLAGS strings, or None if FFmpeg is unavailable."""
    if platform == "linuxbsd":
        pc_env = _pkg_config_env()
        if pc_env is None:
            return None
        cflags = subprocess.check_output(
            ["pkg-config", "--cflags"] + FFMPEG_PKGS, env=pc_env
        ).decode("utf-8")
        libflags = subprocess.check_output(
            ["pkg-config", "--libs"] + FFMPEG_PKGS, env=pc_env
        ).decode("utf-8")
        return {"CFLAGS": cflags.strip(), "LIBFLAGS": libflags.strip()}

    if platform in ("windows", "macos"):
        ffmpeg_path = os.environ.get("FFMPEG_PATH", "")
        if not ffmpeg_path or not os.path.isdir(os.path.join(ffmpeg_path, "include")):
            return None
        cflags = "-I" + os.path.join(ffmpeg_path, "include")
        libflags = "-L" + os.path.join(ffmpeg_path, "lib") + " " + " ".join("-l" + lib for lib in FFMPEG_LIBS)
        return {"CFLAGS": cflags, "LIBFLAGS": libflags}

    return None
