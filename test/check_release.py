"""Check the exported PlatformIO package and build an isolated consumer.

Usage: python test/check_release.py path/to/package.tar.gz [--build]
The optional build installs only the archive, never the repository library.
"""
import argparse
import configparser
import json
import posixpath
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from urllib.parse import unquote, urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    archive = args.archive.resolve()
    with tarfile.open(archive, "r:gz") as package:
        files = {m.name.removeprefix("./"): package.extractfile(m).read()
                 for m in package.getmembers() if m.isfile()}
    required = {"library.json", "LICENSE", "src/i2c.h", "src/i2c.c",
                "README.md", "README.ru.md", "test/hardware/RESULTS.md",
                "test/hardware/results/manifest.json"}
    assert required <= files.keys(), required - files.keys()
    for name in files:
        parts = Path(name).parts
        assert not any(x in parts for x in (".git", ".serena", ".kilo", ".pio", "audit")), name
        assert not name.startswith("test/build/"), name
        assert Path(name).name not in ("compile_commands.json", ".clangd"), name
        assert Path(name).suffix not in (".bin", ".exe", ".o", ".su"), name
    version = json.loads(files["library.json"])["version"]
    for name in ("README.md", "README.ru.md", "src/i2c.c", "src/i2c.h"):
        assert version in files[name].decode("utf-8"), (name, version)
    links = 0
    for name, data in files.items():
        if not name.endswith(".md"):
            continue
        for target in re.findall(r"\[[^\]]*\]\(([^\s)]+)\)", data.decode("utf-8")):
            url = urlsplit(target.strip("<>"))
            if url.scheme or url.netloc or not url.path:
                continue
            relative = posixpath.normpath(posixpath.join(posixpath.dirname(name), unquote(url.path)))
            assert relative in files, (name, target, "missing package link")
            links += 1
    print(f"Package {version}: {len(files)} files, {links} local Markdown links checked", flush=True)
    if not args.build:
        return
    config = configparser.ConfigParser()
    config.read_string(files["platformio.ini"].decode("utf-8"))
    with tempfile.TemporaryDirectory(prefix="i2c-package-") as directory:
        project = Path(directory)
        (project / "src").mkdir()
        (project / "src/main.c").write_bytes(files["examples/size_benchmark/src/main_allapi.c"])
        ini = "[env:consumer]\n"
        for key in ("platform", "platform_packages", "board", "framework", "board_build.use_lto"):
            value = config["env"][key].strip().replace("\n", "\n    ")
            ini += f"{key} = {value}\n"
        ini += f"lib_deps = {archive.as_posix()}\n"
        (project / "platformio.ini").write_text(ini, encoding="utf-8")
        pio = shutil.which("pio") or shutil.which("platformio")
        assert pio, "PlatformIO executable not found"
        subprocess.run([pio, "run", "-d", str(project)], check=True)
        print("Isolated all-API consumer build passed", flush=True)


if __name__ == "__main__":
    main()
