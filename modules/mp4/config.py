def can_build(env, platform):
    # Linux/BSD decodes through the system FFmpeg (dlopen at runtime);
    # Windows through Media Foundation. Other platforms have no backend yet.
    return platform in ("linuxbsd", "windows")


def configure(env):
    pass


def get_doc_classes():
    return [
        "VideoStreamMP4",
    ]


def get_doc_path():
    return "doc_classes"
