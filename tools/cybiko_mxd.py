#!/usr/bin/env python3
"""Turn a video into an MXD clip for the Cybiko Xtreme MP3 cartridge, or an
MXD clip back into a video.

    cybiko_mxd.py VIDEO OUT.app --template CLIP.app [options]
    cybiko_mxd.py CLIP.app OUT.mp4 [--zoom N]

The second form plays the clip's frames at its own frame rate over its
sound track, each pixel N x N (default 4) so the picture stays sharp.

MXD clips are ordinary CyOS applications: a 160x100 picture in four greys,
about five frames a second, played over an MP3 sound track. Put OUT.app on
the cartridge's card (tools/cybiko_card.py) and it shows up on the desktop
next to the MP3 Player.

The clip's player program (main.e) is Cybiko's and is not in this
repository, so the converter copies it from one of the clips on the MP3
cartridge CD (flavor.app, free.app and the others all carry the same one):
pass any of them as --template.

Options:
    --name TEXT       the name under the desktop icon (default: OUT's name)
    --fps N           frames a second, 1-5 (default 5, as on the CD; the
                      Cybiko cannot draw much faster and drops frames)
    --start S         start S seconds into the video
    --duration S      keep at most S seconds
    --fit MODE        "crop" (default) fills the screen, "pad" adds bars
    --dither MODE     "fs" (Floyd-Steinberg, default), "bayer" or "none"
    --audio MODE      "small" (64 kbps 22 kHz mono, default) or "hifi"
                      (128 kbps 44.1 kHz stereo, as most CD clips use)
    --gamma G         brighten (<1) or darken (>1) the picture (default 1.0)

Needs Python 3 and ffmpeg (with libmp3lame) on the PATH; Pillow and numpy
are not needed. The formats are described in file-formats.md of cybiko-docs
(https://github.com/SovGVD/cybiko-docs).
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cybiko_cap  # noqa: E402

W, H = 160, 100
ICON_W, ICON_H = 25, 18

BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def run(cmd):
    try:
        return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    except FileNotFoundError:
        sys.exit("ffmpeg not found: install it and put it on the PATH")
    except subprocess.CalledProcessError as e:
        sys.exit("ffmpeg failed:\n" + e.stderr.decode(errors="replace")[-2000:])


def time_args(a):
    t = []
    if a.start:
        t += ["-ss", str(a.start)]
    if a.duration:
        t += ["-t", str(a.duration)]
    return t


def get_frames(a):
    """Grey 160x100 frames from ffmpeg, as lists of 0-255 values."""
    if a.fit == "pad":
        geo = ("scale=%d:%d:force_original_aspect_ratio=decrease,"
               "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:white" % (W, H, W, H))
    else:
        geo = ("scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d" % (W, H, W, H))
    raw = run(["ffmpeg", "-v", "error"] + time_args(a) + ["-i", a.video, "-an",
               "-vf", "fps=%g,%s,format=gray" % (a.fps, geo),
               "-f", "rawvideo", "-pix_fmt", "gray", "-"])
    n = len(raw) // (W * H)
    if not n:
        sys.exit("no video frames in %s" % a.video)
    return [raw[i * W * H:(i + 1) * W * H] for i in range(n)]


def get_music(a):
    if a.audio == "hifi":
        fmt = ["-ac", "2", "-ar", "44100", "-b:a", "128k"]
    else:
        fmt = ["-ac", "1", "-ar", "22050", "-b:a", "64k"]
    # Plain MPEG frames: no ID3 tag and no Xing header frame.
    return run(["ffmpeg", "-v", "error"] + time_args(a) + ["-i", a.video, "-vn"] + fmt +
               ["-c:a", "libmp3lame", "-map_metadata", "-1", "-id3v2_version", "0",
                "-write_xing", "0", "-f", "mp3", "-"])


def to_levels(grey, w, h, dither, gamma):
    """0-255 greys to Cybiko levels 0 (white) to 3 (black)."""
    lum = [255.0 * (v / 255.0) ** gamma for v in grey] if gamma != 1.0 else [float(v) for v in grey]
    out = [0] * (w * h)
    if dither == "fs":
        for y in range(h):
            row = y * w
            for x in range(w):
                i = row + x
                old = lum[i]
                lvl = min(3, max(0, int((255.0 - old) / 85.0 + 0.5)))
                out[i] = lvl
                err = old - (255 - 85 * lvl)
                if x + 1 < w:
                    lum[i + 1] += err * 7 / 16
                if y + 1 < h:
                    if x > 0:
                        lum[i + w - 1] += err * 3 / 16
                    lum[i + w] += err * 5 / 16
                    if x + 1 < w:
                        lum[i + w + 1] += err * 1 / 16
    else:
        for y in range(h):
            for x in range(w):
                i = y * w + x
                d = (BAYER4[y & 3][x & 3] + 0.5) / 16 - 0.5 if dither == "bayer" else 0.0
                out[i] = min(3, max(0, int((255.0 - lum[i]) / 85.0 + 0.5 + d)))
    return out


def pack_2bpp(levels, w, h):
    """Rows of 2-bit pixels, four to a byte, leftmost in the top bits;
    each row padded to whole bytes."""
    stride = (w + 3) // 4
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            out[y * stride + x // 4] |= levels[y * w + x] << (6 - 2 * (x % 4))
    return bytes(out)


def make_icon(frame, dither, gamma):
    """A 25x18 picture from a frame, cropped to the icon's shape."""
    cw = W * ICON_H * 25 // (H * 18)        # same aspect as the icon (139 wide)
    x0 = (W - cw) // 2
    small = []
    for y in range(ICON_H):
        for x in range(ICON_W):
            sx0, sx1 = x0 + x * cw // ICON_W, x0 + (x + 1) * cw // ICON_W
            sy0, sy1 = y * H // ICON_H, (y + 1) * H // ICON_H
            vals = [frame[sy * W + sx] for sy in range(sy0, sy1) for sx in range(sx0, sx1)]
            small.append(sum(vals) // len(vals))
    pix = pack_2bpp(to_levels(small, ICON_W, ICON_H, dither, gamma), ICON_W, ICON_H)
    # 2 bits a pixel, one image, width and height, then (as in every icon
    # seen) 0, 0 and the size again.
    return bytes([2, 1, ICON_W, ICON_H, 0, 0, ICON_W, ICON_H]) + pix


def root_inf(name):
    lines = ["root", name + " ", "", "", "sound.dl", "", "Video clip", "21.1.1", "1.5.1",
             "sound.dl<21.1.1>"]
    return ("\r\n".join(lines) + "\r\n").encode("latin-1", errors="replace")


def template_player(path):
    for name, method, data in cybiko_cap.cy_entries(open(path, "rb").read()):
        if name == "main.e" and data is not None:
            return data
    sys.exit("%s has no main.e: pass one of the MXD clips from the MP3 cartridge CD" % path)


def to_video(clip, out, zoom):
    parts = {name: data for name, method, data in cybiko_cap.cy_entries(open(clip, "rb").read())}
    pic, music = parts.get("clip.pic"), parts.get("music.mp3")
    if not pic or len(pic) < 4:
        sys.exit("%s is not an MXD clip (no clip.pic)" % clip)
    period, w, h = int.from_bytes(pic[0:2], "big"), pic[2], pic[3]
    size = (w + 3) // 4 * h
    grey = bytes([255, 170, 85, 0])
    raw = bytearray()
    pos = 4
    while pos + 4 + size <= len(pic):
        fw, fh = pic[pos + 2], pic[pos + 3]
        if (fw, fh) != (w, h):
            sys.exit("frame at %d is %dx%d, not %dx%d: not supported" % (pos, fw, fh, w, h))
        rows = pic[pos + 4:pos + 4 + size]
        stride = (w + 3) // 4
        for y in range(h):
            for x in range(w):
                raw.append(grey[rows[y * stride + x // 4] >> (6 - 2 * (x % 4)) & 3])
        pos += 4 + size
    frames = len(raw) // (w * h)
    fps = 10000.0 / period if period else 5.0
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", "%dx%d" % (w, h),
           "-framerate", "%g" % fps, "-i", "-"]
    tmp = None
    if music:
        import tempfile
        tmp = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
        tmp.write(music)
        tmp.close()
        cmd += ["-i", tmp.name, "-c:a", "aac", "-b:a", "128k"]
    cmd += ["-vf", "scale=%d:%d:flags=neighbor" % (w * zoom, h * zoom), "-c:v", "libx264",
            "-pix_fmt", "yuv420p", "-crf", "18", out]
    try:
        subprocess.run(cmd, input=bytes(raw), check=True, stderr=subprocess.PIPE)
    except FileNotFoundError:
        sys.exit("ffmpeg not found: install it and put it on the PATH")
    except subprocess.CalledProcessError as e:
        sys.exit("ffmpeg failed:\n" + e.stderr.decode(errors="replace")[-2000:])
    finally:
        if tmp:
            os.unlink(tmp.name)
    print("%s: %d frames at %g fps (%.1f s)" % (out, frames, fps, frames / fps))


def main():
    if len(sys.argv) >= 3 and sys.argv[1].lower().endswith(".app") and not sys.argv[2].lower().endswith(".app"):
        d = argparse.ArgumentParser(description="MXD clip to video")
        d.add_argument("clip")
        d.add_argument("out")
        d.add_argument("--zoom", type=int, default=4)
        a = d.parse_args()
        to_video(a.clip, a.out, max(1, a.zoom))
        return
    p = argparse.ArgumentParser(description="Video to Cybiko MXD clip", usage=__doc__)
    p.add_argument("video")
    p.add_argument("out")
    p.add_argument("--template", required=True)
    p.add_argument("--name")
    p.add_argument("--fps", type=float, default=5.0)
    p.add_argument("--start", type=float)
    p.add_argument("--duration", type=float)
    p.add_argument("--fit", choices=("crop", "pad"), default="crop")
    p.add_argument("--dither", choices=("fs", "bayer", "none"), default="fs")
    p.add_argument("--audio", choices=("small", "hifi"), default="small")
    p.add_argument("--gamma", type=float, default=1.0)
    a = p.parse_args()
    if not 1 <= a.fps <= 5:
        sys.exit("--fps must be 1 to 5")
    name = a.name or os.path.splitext(os.path.basename(a.out))[0]

    player = template_player(a.template)
    frames = get_frames(a)
    music = get_music(a)
    print("%d frames, %d KB of music" % (len(frames), len(music) // 1024))

    # clip.pic: u16 frame period in units of 0.1 ms, width and height as
    # bytes, then for each frame 0, 0, width, height and the picture.
    period = int(round(10000 / a.fps))
    pic = bytearray(period.to_bytes(2, "big") + bytes([W, H]))
    for k, f in enumerate(frames):
        pic += bytes([0, 0, W, H]) + pack_2bpp(to_levels(f, W, H, a.dither, a.gamma), W, H)
        if k % 25 == 24:
            print("  %d/%d frames" % (k + 1, len(frames)), end="\r", flush=True)
    icon = make_icon(frames[len(frames) // 3], a.dither, a.gamma)

    print("compressing...      ")
    archive = cybiko_cap.cy_archive([
        ("music.mp3", music, False),
        ("clip.pic", bytes(pic), True),
        ("main.e", player, True),
        ("root.inf", root_inf(name), True),
        ("0.ico", icon, False),
    ])
    with open(a.out, "wb") as f:
        f.write(archive)
    print("%s: %d KB, %.1f s" % (a.out, len(archive) // 1024, len(frames) / a.fps))


if __name__ == "__main__":
    main()
