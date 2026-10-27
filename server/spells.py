"""What the laptop does when a spell is cast. Edit freely: the server reloads this file on every cast.

Write a function named after the spell: lumos, nox, leviosa, expelliarmus, accio, or fizzle.
It gets the cast as a dict: {"spell": "LUMOS", "score": 87, "player": "anon", "src": "wand" or "sim", "t": unix time}.
Casts from the web page's simulator call these too; add `if cast["src"] != "wand": return` to skip them.
Each function runs in the background, so a slow one doesn't hold up the wand.
"""
import json
import subprocess
import sys
import webbrowser
from pathlib import Path

SOUNDS = Path(__file__).parent / "sounds"  # put your own .wav / .mp3 / .aiff files here
VOLUME_STEP = 10  # % of full volume per LUMOS / NOX

# Spells that open a link, with their default URL. The web page's "Spell links" box lists these and can change
# them (saved in links.json next to this file); add a spell here to make its link settable there too.
LINKS = {
    "leviosa": "https://www.youtube.com",
    "expelliarmus": "https://www.youtube.com/watch?v=MQbAJKg5g7M?autoplay=1",
}
LINKS_FILE = Path(__file__).parent / "links.json"


def link(spell):
    """The URL for this spell: the one set on the web page, or the default in LINKS."""
    try:
        saved = json.loads(LINKS_FILE.read_text())
    except (OSError, ValueError):
        saved = {}
    return saved.get(spell) or LINKS[spell]


def play(sound):
    """Play a sound without waiting. `sound` is a file in server/sounds/, a full path, or a built-in sound name:
    on macOS Basso, Blow, Bottle, Frog, Funk, Glass, Hero, Morse, Ping, Pop, Purr, Sosumi, Submarine, Tink;
    on Windows the .wav names in C:\\Windows\\Media (e.g. "tada", "chimes"); Windows plays .wav files only."""
    options = [Path(sound), SOUNDS / sound, Path("/System/Library/Sounds") / f"{sound}.aiff",
               Path("C:/Windows/Media") / f"{sound}.wav"]
    path = next((p for p in options if p.is_file()), None)
    if not path:
        print(f"spells.py: no sound called {sound!r}")
        return
    if sys.platform == "darwin":
        subprocess.Popen(["afplay", str(path)])
    elif sys.platform == "win32":
        subprocess.Popen(["powershell", "-c", f"(New-Object Media.SoundPlayer '{path}').PlaySync()"])
    else:
        subprocess.Popen(["paplay", str(path)])


def open_link(url):
    """Open a web page in the laptop's default browser."""
    webbrowser.open(url)


def applescript(script):
    """Run AppleScript (macOS) and print why if it fails."""
    result = subprocess.run(["osascript", "-"], input=script, text=True, capture_output=True)
    if result.returncode:
        print("spells.py: AppleScript failed:", result.stderr.strip())
    return result


def change_volume(step):
    """Turn the laptop's volume up (step > 0) or down, in % of full volume."""
    if sys.platform == "darwin":
        applescript(f"""set v to (output volume of (get volume settings)) + ({step})
if v > 100 then set v to 100
if v < 0 then set v to 0
set volume output volume v
{"set volume without output muted" if step > 0 else ""}""")
    elif sys.platform == "win32":  # the keyboard's volume keys, 2% per press
        key = 175 if step > 0 else 174
        subprocess.Popen(["powershell", "-c", f"$s = New-Object -ComObject WScript.Shell; 1..{abs(step) // 2} | % {{ $s.SendKeys([char]{key}) }}"])
    else:
        print("spells.py: volume control is set up for macOS and Windows only")


BROWSERS = ["Google Chrome", "Brave Browser", "Microsoft Edge", "Safari"]
# JavaScript run inside YouTube tabs. Videos that Accio pauses get a data-accio mark, so the next Accio
# resumes exactly those and not every YouTube tab you have open.
STATE_JS = ("[...document.querySelectorAll('video')].some(v => !v.paused) ? 'playing'"
            " : document.querySelector('video[data-accio]') ? 'marked' : 'paused'")
PAUSE_JS = ("document.querySelectorAll('video').forEach(v => { delete v.dataset.accio;"
            " if (!v.paused) { v.dataset.accio = 1; v.pause(); } }); 'ok'")
RESUME_JS = "document.querySelectorAll('video[data-accio]').forEach(v => { delete v.dataset.accio; v.play(); }); 'ok'"
PLAY_FIRST_JS = "document.querySelector('video')?.play(); 'ok'"


def youtube_tabs(app, js, first_only=False):
    """Run `js` in every YouTube tab of a browser (front window first) and return each tab's result.
    One-time setup in the browser: Chrome / Brave / Edge: View > Developer > Allow JavaScript from Apple Events.
    Safari: Develop > Allow JavaScript from Apple Events. macOS also asks once to let Terminal control it."""
    run = f'do JavaScript "{js}" in t' if app == "Safari" else f'execute t javascript "{js}"'
    result = applescript(f"""set out to {{}}
tell application "{app}"
  repeat with w in windows
    repeat with t in tabs of w
      if URL of t contains "youtube.com" then
        set end of out to ({run}) as text
        {"return out" if first_only else ""}
      end if
    end repeat
  end repeat
end tell
return out""")
    if result.returncode and "JavaScript" in result.stderr:
        print(f'spells.py: in {app}, turn on "Allow JavaScript from Apple Events" (see youtube_tabs)')
    return [s for s in result.stdout.strip().split(", ") if s]


def toggle_youtube():
    """Pause YouTube if anything is playing; otherwise resume what Accio paused (or the first YouTube tab).
    On Windows this presses the keyboard's play/pause media key instead."""
    if sys.platform == "win32":
        subprocess.Popen(["powershell", "-c", "(New-Object -ComObject WScript.Shell).SendKeys([char]179)"])
        return
    if sys.platform != "darwin":
        print("spells.py: YouTube control is set up for macOS and Windows only")
        return
    running = [app for app in BROWSERS if subprocess.run(["pgrep", "-x", app], capture_output=True).returncode == 0]
    states = {app: youtube_tabs(app, STATE_JS) for app in running}  # browsers that aren't running stay closed
    if any("playing" in s for s in states.values()):
        for app in running:
            youtube_tabs(app, PAUSE_JS)
    elif any("marked" in s for s in states.values()):
        for app in running:
            youtube_tabs(app, RESUME_JS)
    else:
        first = next((app for app in running if states[app]), None)
        if first:
            youtube_tabs(first, PLAY_FIRST_JS, first_only=True)


# ---- One function per spell ----

def lumos(cast):
    change_volume(+VOLUME_STEP)
    play("Pop")  # hear the new volume


def nox(cast):
    change_volume(-VOLUME_STEP)
    play("Pop")


def leviosa(cast):
    play("Purr")
    open_link(link("leviosa"))


def expelliarmus(cast):
    play("Hero")
    open_link(link("expelliarmus"))


def accio(cast):
    toggle_youtube()


def fizzle(cast):
    play("Basso")
