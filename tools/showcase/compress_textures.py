"""Recompresses the showcase glTF textures (run after build_showcase.py).

Usage: python tools/showcase/compress_textures.py assets/showcase/showcase.gltf

Images with real transparency stay RGBA PNG; packed ORM maps become RGB PNG;
colour and normal maps become high-quality JPEG. The glTF image URIs and MIME
types are rewritten to match. Requires Pillow.
"""

import json
import os
import sys

from PIL import Image


def target_format(name, image):
    if image.mode == "RGBA" and image.getchannel("A").getextrema()[0] < 255:
        return "png", "RGBA", {"optimize": True}
    if name.endswith("_orm"):
        return "png", "RGB", {"optimize": True}
    quality = 95 if name.endswith("_normal") else 92
    return "jpg", "RGB", {"quality": quality, "subsampling": 0 if name.endswith("_normal") else 2}


def main(gltf_path):
    root = os.path.dirname(gltf_path)
    with open(gltf_path, encoding="utf-8") as file:
        document = json.load(file)
    for entry in document.get("images", []):
        source = os.path.join(root, entry["uri"])
        name = os.path.splitext(os.path.basename(source))[0]
        with Image.open(source) as opened:
            opened.load()
            image = opened.convert("RGBA")
        extension, mode, options = target_format(name, image)
        relative = os.path.join(os.path.dirname(entry["uri"]), name + "." + extension).replace("\\", "/")
        destination = os.path.join(root, relative)
        image.convert(mode).save(destination, **options)
        if os.path.normcase(os.path.abspath(destination)) != os.path.normcase(os.path.abspath(source)):
            os.remove(source)
        entry["uri"] = relative
        entry["mimeType"] = "image/png" if extension == "png" else "image/jpeg"
    with open(gltf_path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, indent=1)


if __name__ == "__main__":
    main(sys.argv[1])
