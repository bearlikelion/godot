import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffmpeg_detect import find_ffmpeg


def can_build(env, platform):
    if platform not in ("linuxbsd", "windows", "macos"):
        return False
    if find_ffmpeg(platform) is None:
        print(
            "mp4 module: FFmpeg 5.1+ development libraries not found "
            "(pkg-config on linuxbsd, FFMPEG_PATH env var on windows/macos), disabling."
        )
        return False
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "VideoStreamMP4",
    ]


def get_doc_path():
    return "doc_classes"
