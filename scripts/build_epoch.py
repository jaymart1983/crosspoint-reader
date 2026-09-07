"""
PlatformIO pre-build script: inject the firmware's build epoch as
CROSSPOINT_BUILD_EPOCH (UTC seconds since 1970, an integer constant).

Why the firmware carries a timestamp at all: the PCF8563/DS3231/RX8130 RTC ships
with a stopped oscillator, so a device that has never been told the time by a
BLE client reads as "unknown" and every reading position it saves goes unstamped
-- losing every sync conflict. A device cannot be older than the firmware
running on it, so the build epoch is a sound *lower bound* for the wall clock and
HalClock::begin() seeds the RTC with it on first boot (see lib/hal/HalClock.cpp).

The value is the HEAD commit time, not `now`:

  * It is UTC. `git log --format=%ct` is committer time in epoch seconds, with no
    time-zone ambiguity -- unlike __DATE__/__TIME__, which are local build time.
  * It is deterministic. Baking the wall clock in would change a global -D on
    every single build, invalidating the whole compile cache each time; the
    commit time only moves when the source does.
  * It is still a lower bound: the commit cannot postdate the build.

Without git (a source tarball, or git missing) it falls back to the actual build
time, which is likewise UTC and likewise a lower bound. In either case the value
is floored at 2020-01-01 so it can never be a value HalClock would reject.
"""

import os
import subprocess
import sys
import time

# Must match HalClock::MIN_VALID_EPOCH / MAX_VALID_EPOCH -- a seed outside that
# window is refused by setEpoch(), which would silently leave the clock stopped.
MIN_VALID_EPOCH = 1577836800  # 2020-01-01T00:00:00Z
MAX_VALID_EPOCH = 4102444800  # 2100-01-01T00:00:00Z


def warn(msg):
    print(f'WARNING [build_epoch.py]: {msg}', file=sys.stderr)


def git_commit_epoch(project_dir):
    """HEAD's committer time in UTC epoch seconds, or None if unavailable."""
    try:
        value = subprocess.check_output(
            ['git', 'log', '-1', '--format=%ct'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        return int(value)
    except FileNotFoundError:
        warn('git not found on PATH; falling back to the current build time')
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): {e.stderr.strip()}; '
            'falling back to the current build time'
        )
    except (ValueError, OSError) as e:
        warn(f'could not read the HEAD commit time ({e}); falling back to the current build time')
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(f'unexpected error reading the HEAD commit time ({e}); falling back to the current build time')
    return None


def resolve_build_epoch(project_dir):
    epoch = git_commit_epoch(project_dir)
    source = 'HEAD commit time'
    if epoch is None:
        # time.time() is already UTC-based, so no time-zone conversion is needed.
        epoch = int(time.time())
        source = 'build time'
    if epoch < MIN_VALID_EPOCH or epoch >= MAX_VALID_EPOCH:
        warn(
            f'{source} {epoch} is outside the range the RTC accepts; '
            f'clamping the seed to {MIN_VALID_EPOCH}'
        )
        epoch = MIN_VALID_EPOCH
        source = f'{source}, clamped'
    return epoch, source


def inject_build_epoch(env):
    epoch, source = resolve_build_epoch(env['PROJECT_DIR'])
    env.Append(CPPDEFINES=[('CROSSPOINT_BUILD_EPOCH', f'{epoch}UL')])
    stamp = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(epoch))
    print(f'CrossPoint build epoch: {epoch} ({stamp}, from {source})')


# PlatformIO/SCons entry point -- Import and env are SCons builtins injected at
# runtime. When run directly with Python (e.g. for validation), a lightweight
# fake env exercises the same logic without a full build.
try:
    Import('env')                # noqa: F821  # type: ignore[name-defined]
    inject_build_epoch(env)      # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_build_epoch(_Env({'PROJECT_DIR': _project_dir}))
