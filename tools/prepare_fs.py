"""Prepare the filesystem image: bundle the web page and keep the board's devices.

1. Device configuration. An image built from extras/ alone would replace the
   board's 1W.json (pairing keys) and RemoteMap.json with empty files. Before the
   image is built, the current files are downloaded from the board (address and
   login from include/secrets.h) into config/, which is git-ignored because the
   repository is public and 1W.json holds pairing keys. If the board cannot be
   reached, the copy already in config/ is used. The image then carries these
   files instead of the empty ones from extras/.

2. Web page bundle, below.

A page load used to fetch about 27 files at once (HTML, CSS, 9 scripts, SVG icons).
With the TLS cloud link holding much of the heap, that burst ran the board out of
memory: requests failed, the network stack hung or the board crashed. Here the
stylesheet, the scripts and the SVG icons are inlined into a single gzipped
index.html, so a page load fetches that file, the logo and one language file.

The sources in <data_dir>/web_interface_data stay as they are and are still the
files to edit. The image is built from a copy in the build directory, in which
index.html, css/ and js/ are replaced by index.html.gz. The web server serves
index.html.gz with Content-Encoding: gzip when index.html is absent.

Runs automatically for the buildfs, uploadfs and uploadfsota targets.
"""

import base64
import gzip
import json
import os
import re
import shutil
import urllib.request

Import("env")  # noqa: F821 - provided by PlatformIO

WEB_DIR = "web_interface_data"
FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}
BOARD_URL = "http://miopenio.local"
DEVICE_FILES = {"1W.json": "/api/download/devices", "RemoteMap.json": "/api/download/remotes"}


def read_secret(project_dir, name):
    path = os.path.join(project_dir, "include", "secrets.h")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        match = re.search(r'#define\s+%s\s+"([^"]*)"' % name, f.read())
    return match.group(1) if match else None


def refresh_device_config(project_dir, config_dir):
    """Download the board's device files into config_dir. Returns True on success."""
    user = read_secret(project_dir, "LOCAL_WEB_USER")
    password = read_secret(project_dir, "LOCAL_WEB_PASSWORD")
    if not user or password is None:
        print("prepare_fs: no web login in include/secrets.h, not downloading device files")
        return False
    passwords = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    passwords.add_password(None, BOARD_URL, user, password)
    auth = urllib.request.HTTPDigestAuthHandler(passwords)
    opener = urllib.request.build_opener(auth)
    downloaded = {}
    try:
        for name, path in DEVICE_FILES.items():
            with opener.open(BOARD_URL + path, timeout=8) as response:
                body = response.read()
            json.loads(body)  # Refuse to keep anything that is not valid JSON.
            downloaded[name] = body
    except Exception as error:  # Board offline, wrong login, bad reply: fall back.
        print("prepare_fs: could not download device files from the board (%s)" % error)
        return False
    os.makedirs(config_dir, exist_ok=True)
    for name, body in downloaded.items():
        with open(os.path.join(config_dir, name), "wb") as f:
            f.write(body)
    print("prepare_fs: device files downloaded from the board into config/")
    return True


def apply_device_config(config_dir, out_dir):
    names = [n for n in DEVICE_FILES if os.path.exists(os.path.join(config_dir, n))]
    if len(names) != len(DEVICE_FILES):
        print("prepare_fs: WARNING no saved device files in config/: the image carries "
              "the empty ones from extras/ and will erase the board's devices")
        return
    for name in names:
        shutil.copyfile(os.path.join(config_dir, name), os.path.join(out_dir, name))
    print("prepare_fs: image uses the device files from config/")


def bundle(data_dir, out_dir):
    web = os.path.join(data_dir, WEB_DIR)
    shutil.rmtree(out_dir, ignore_errors=True)
    shutil.copytree(data_dir, out_dir)
    out_web = os.path.join(out_dir, WEB_DIR)

    def read_text(rel):
        with open(os.path.join(web, rel), encoding="utf-8") as f:
            return f.read()

    def svg_uri(rel):
        with open(os.path.normpath(os.path.join(web, rel)), "rb") as f:
            return "data:image/svg+xml;base64," + base64.b64encode(f.read()).decode()

    def inline_css(match):
        href = match.group(1).split("?")[0]
        css_dir = os.path.dirname(href)
        css = re.sub(
            r'url\("?([^")]+\.svg)"?\)',
            lambda u: 'url("%s")' % svg_uri(os.path.join(css_dir, u.group(1))),
            read_text(href),
        )
        return "<style>\n%s\n</style>" % css

    def inline_js(match):
        src = match.group(1).split("?")[0]
        # A literal "</script" inside the code would end the inline block early.
        return "<script>\n%s\n</script>" % read_text(src).replace("</script", "<\\/script")

    html = read_text("index.html")
    html = re.sub(r'<link rel="stylesheet" href="([^"]+)">', inline_css, html)
    html = re.sub(r'<script src="([^"]+)"></script>', inline_js, html)
    html = re.sub(r'src="(img/[^"]+\.svg)"', lambda m: 'src="%s"' % svg_uri(m.group(1)), html)

    leftover = re.findall(r'(?:src|href)="((?:css|js)/[^"]+)"', html)
    if leftover:
        raise RuntimeError("prepare_fs: references left unbundled: %s" % ", ".join(leftover))

    os.remove(os.path.join(out_web, "index.html"))
    shutil.rmtree(os.path.join(out_web, "css"))
    shutil.rmtree(os.path.join(out_web, "js"))
    # mtime=0 keeps the output identical for identical sources.
    with gzip.GzipFile(os.path.join(out_web, "index.html.gz"), "wb", compresslevel=9, mtime=0) as f:
        f.write(html.encode("utf-8"))
    print("prepare_fs: index.html.gz %d bytes (%d before compression)" % (
        os.path.getsize(os.path.join(out_web, "index.html.gz")), len(html.encode("utf-8"))))


if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # noqa: F821 - provided by PlatformIO
    project_dir = env.subst("$PROJECT_DIR")
    config_dir = os.path.join(project_dir, "config")
    source_dir = env.subst("$PROJECT_DATA_DIR")
    staged_dir = os.path.join(env.subst("$PROJECT_BUILD_DIR"), env.subst("$PIOENV"), "fsdata")
    bundle(source_dir, staged_dir)
    refresh_device_config(project_dir, config_dir)
    apply_device_config(config_dir, staged_dir)
    env.Replace(PROJECT_DATA_DIR=staged_dir)
