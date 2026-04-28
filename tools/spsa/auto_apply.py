#!/usr/bin/env python3
"""Watch an in-progress SPSA run and commit applied values every N iters.

Runs as a sidecar next to `spsa.py`. Every poll interval, reads the
driver's atomic checkpoint (`tuning/spsa/theta.json`) and, when
`meta.next_iter` has crossed a new N-iteration boundary since the last
apply, updates the in-scope fields of `src/search_params.cpp` with the
current theta, runs `make test` to validate, commits the change with an
iteration-stamped message, and pushes to the current branch. CI's
PR-screen job triggers on each push, so the 10+0.1 screen runs on every
checkpoint milestone during the tune.

The sidecar only touches scalars present in `theta.json`'s `params` map.
Out-of-scope search scalars (Razor*, `NmpBase`) stay at whatever value
they carry in `src/search_params.cpp` today.

Usage:
    python3 tools/spsa/auto_apply.py [--every 50] [--output-dir tuning/spsa]
                                      [--source src/search_params.cpp]
                                      [--poll-sec 30] [--once]

`--once` exits after a single apply (useful for one-off pushes or tests).
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Optional

GIT_AUTHOR_NAME = "Ryan Lefkowitz"
GIT_AUTHOR_EMAIL = "ryan@outtake.ai"


# Each struct line in `src/search_params.cpp` looks like
#     <whitespace><number>, // <Name>  (optional notes)
# The regex captures the prefix, value, separator whitespace, and the
# name token so we can rewrite the number while leaving everything else
# (including the comment) byte-identical.
FIELD_LINE = re.compile(
    r"^(?P<prefix>\s+)(?P<value>-?\d+)(?P<sep>,\s+//\s+)(?P<name>\w+)(?P<rest>.*)$"
)


def read_checkpoint(path: Path) -> Optional[Dict]:
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def rewrite_source(source_path: Path, params: Dict[str, int]) -> Dict[str, int]:
    """Rewrite the struct literal in-place; return updated param values.

    Only touches fields whose name appears as a key in `params`. Lines
    that don't match the `FIELD_LINE` regex are copied through unchanged.
    Returns the map of field name -> (old_value, new_value) for scalars
    that actually moved.
    """
    lines = source_path.read_text().splitlines(keepends=True)
    diff: Dict[str, int] = {}
    new_lines = []
    for line in lines:
        match = FIELD_LINE.match(line.rstrip("\n"))
        if not match:
            new_lines.append(line)
            continue
        name = match.group("name")
        if name not in params:
            new_lines.append(line)
            continue
        old_value = int(match.group("value"))
        new_value = int(params[name])
        if old_value != new_value:
            diff[name] = (old_value, new_value)
        rewritten = (
            match.group("prefix")
            + str(new_value)
            + match.group("sep")
            + name
            + match.group("rest")
            + "\n"
        )
        new_lines.append(rewritten)

    source_path.write_text("".join(new_lines))
    return diff


def run(cmd, check=True, capture=False):
    result = subprocess.run(
        cmd,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    return result


def git_has_uncommitted(paths) -> bool:
    result = run(["git", "status", "--porcelain", "--", *paths], capture=True)
    return bool(result.stdout.strip())


def git_has_other_staged() -> bool:
    """True if anything beyond search_params.cpp is currently staged."""
    result = run(["git", "diff", "--name-only", "--cached"], capture=True)
    staged = [p for p in result.stdout.splitlines() if p and p != "src/search_params.cpp"]
    return bool(staged)


def apply_and_commit(
    source_path: Path,
    params: Dict[str, int],
    next_iter: int,
    iterations_target: int,
) -> bool:
    if git_has_other_staged():
        sys.stderr.write(
            "Refusing to auto-apply: the git index already has staged changes beyond "
            "src/search_params.cpp. Clean the index and re-run the auto-apply sidecar.\n"
        )
        return False

    diff = rewrite_source(source_path, params)
    if not diff:
        sys.stderr.write(
            f"  iter {next_iter}: theta unchanged since last apply; skipping commit\n"
        )
        return False

    sys.stderr.write(
        f"  iter {next_iter}: applying {len(diff)} shifted scalars: "
        + ", ".join(f"{n}:{old}->{new}" for n, (old, new) in diff.items())
        + "\n"
    )

    # Validate before committing.
    sys.stderr.write("  running make build && make test...\n")
    try:
        run(["make", "build"], capture=True)
        run(["make", "test"], capture=True)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            "  build or tests failed after applying theta; reverting source change\n"
        )
        run(["git", "checkout", "--", str(source_path)])
        if exc.stdout:
            sys.stderr.write(exc.stdout[-2000:])
        if exc.stderr:
            sys.stderr.write(exc.stderr[-2000:])
        return False

    message = (
        f"🎯 Auto-apply SPSA-tuned values at iter {next_iter} of {iterations_target}"
    )
    run(["git", "add", str(source_path)])
    run([
        "git",
        "-c",
        f"user.name={GIT_AUTHOR_NAME}",
        "-c",
        f"user.email={GIT_AUTHOR_EMAIL}",
        "commit",
        "-m",
        message,
        f"--author={GIT_AUTHOR_NAME} <{GIT_AUTHOR_EMAIL}>",
    ])

    # Rebase on top of origin in case CI (or anything else) pushed to this
    # branch while the sidecar was sleeping. No force-push; rebase only.
    sys.stderr.write("  fetching and rebasing...\n")
    run(["git", "fetch", "origin"])
    branch = run(["git", "branch", "--show-current"], capture=True).stdout.strip()
    remote_ref = f"origin/{branch}"
    try:
        run(["git", "merge-base", "--is-ancestor", remote_ref, "HEAD"])
    except subprocess.CalledProcessError:
        # Behind origin; rebase onto it.
        try:
            run(["git", "pull", "--rebase", "origin", branch])
        except subprocess.CalledProcessError:
            sys.stderr.write("  rebase failed; skipping push for this milestone\n")
            return False
    run(["git", "push", "origin", branch])
    sys.stderr.write(
        f"  committed and pushed {message!r}\n"
    )
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--every", type=int, default=50,
                    help="apply + commit + push after every N new iterations")
    ap.add_argument("--output-dir", type=str, default="tuning/spsa",
                    help="SPSA driver's output directory (contains theta.json)")
    ap.add_argument("--source", type=str, default="src/search_params.cpp",
                    help="path to the file whose struct literal gets rewritten")
    ap.add_argument("--poll-sec", type=float, default=30.0,
                    help="how often to re-read the checkpoint")
    ap.add_argument("--once", action="store_true",
                    help="apply once (or not at all) and exit")
    ap.add_argument("--force", action="store_true",
                    help="apply the current theta regardless of milestone (implies --once); "
                    "useful for ad-hoc commits triggered between automatic milestones")
    args = ap.parse_args()
    if args.force:
        args.once = True

    theta_path = Path(args.output_dir) / "theta.json"
    source_path = Path(args.source)

    if not source_path.exists():
        sys.stderr.write(f"Source file not found at {source_path}\n")
        return 1

    last_applied = -1

    while True:
        checkpoint = read_checkpoint(theta_path)
        if checkpoint is None:
            if args.once:
                sys.stderr.write(
                    f"No checkpoint at {theta_path} and --once was set; exiting\n"
                )
                return 1
            time.sleep(args.poll_sec)
            continue

        meta = checkpoint.get("meta", {})
        params = checkpoint.get("params", {})
        next_iter = int(meta.get("next_iter", 0))
        target = int(meta.get("iterations_target", 0))
        if next_iter == 0 or not params:
            if args.once:
                sys.stderr.write("Checkpoint has no iterations yet; exiting\n")
                return 0
            time.sleep(args.poll_sec)
            continue

        # Boundary arithmetic: apply at every N-iter multiple strictly after
        # the last one we applied, plus at target completion. --force skips
        # the boundary check so the caller can commit the current theta
        # regardless of where the next milestone sits.
        milestone = (next_iter // args.every) * args.every
        should_apply = args.force or (
            milestone > last_applied and milestone > 0
        ) or (next_iter >= target and last_applied < target)

        if should_apply:
            sent = apply_and_commit(source_path, params, next_iter, target)
            if sent:
                last_applied = milestone if next_iter < target else next_iter
            if args.once:
                return 0 if sent else 0
        elif args.once:
            sys.stderr.write(
                f"No new milestone crossed yet (next_iter={next_iter}, "
                f"last applied milestone={last_applied}); exiting\n"
            )
            return 0

        if next_iter >= target:
            sys.stderr.write("Target iterations reached; sidecar exiting\n")
            return 0

        time.sleep(args.poll_sec)


if __name__ == "__main__":
    sys.exit(main())
