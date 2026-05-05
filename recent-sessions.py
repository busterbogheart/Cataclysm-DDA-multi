#!/usr/bin/env python3
"""
Recent Sessions Summary
Reads Claude Code JSONL conversation files and extracts user messages
from the last N hours. Used by Claude at session startup to get context.

Usage:
    python3 recent-sessions.py          # Auto mode: escalates until substantive content found
    python3 recent-sessions.py --hours 4
    python3 recent-sessions.py --hours 24
    python3 recent-sessions.py --files 3  # Last 3 files regardless of time

Auto mode escalation: --hours 2 → --hours 8 → --files 3 → --files 5
Stops when it finds sessions with substantive content (not just "continue", "ok", etc.)

Added: Session 125, Feb 23 2026
"""

import json
import os
import sys
import argparse
from datetime import datetime, timedelta
from pathlib import Path

JSONL_DIR = Path.home() / ".claude/projects/-Users-ethankemp-Cataclysm-DDA-multiplayer"

TRIVIAL_MESSAGES = {
    "continue", "ok", "yes", "no", "sure", "thanks", "good", "great",
    "done", "next", "go", "proceed", "fine", "got it", "okay", "yep",
    "nope", "continue.", "ok.", "yes.", "no.", "sure.", "good.", "great.",
    "continue with the last session", "continue with dark mode",
}


def get_recent_files(hours=None, max_files=None, skip_active=False):
    """Find recent .jsonl conversation files, sorted by modification time (newest first)."""
    files = list(JSONL_DIR.glob("*.jsonl"))
    files.sort(key=lambda f: f.stat().st_mtime, reverse=True)

    if skip_active and files:
        # Skip the most recently modified file — it's the live session being written right now
        files = files[1:]

    if hours is not None:
        cutoff = datetime.now().timestamp() - (hours * 3600)
        files = [f for f in files if f.stat().st_mtime >= cutoff]

    if max_files is not None:
        files = files[:max_files]

    return files


def extract_user_messages(filepath):
    """Extract non-empty user messages from a JSONL conversation file."""
    messages = []
    with open(filepath) as f:
        for line in f:
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            # User messages have type "user" with message.role "user"
            msg = obj.get("message", {})
            if msg.get("role") != "user":
                continue

            content = msg.get("content", [])
            if isinstance(content, list):
                texts = [
                    c.get("text", "").strip()
                    for c in content
                    if c.get("type") == "text" and c.get("text", "").strip()
                ]
                if texts:
                    # Join multiple text blocks (rare but possible)
                    messages.append(" ".join(texts))
            elif isinstance(content, str) and content.strip():
                messages.append(content.strip())

    return messages


def is_substantive(files_and_messages):
    """
    Returns True if the set of sessions contains meaningful work content —
    i.e., at least one message that isn't just a short filler word/phrase
    and is longer than 20 chars or not in the trivial set.
    """
    for _filepath, messages in files_and_messages:
        for msg in messages:
            clean = msg.strip().lower().rstrip(".")
            if clean not in TRIVIAL_MESSAGES and len(msg.strip()) > 25:
                return True
    return False


def format_file_time(filepath):
    """Get human-readable modification time."""
    mtime = filepath.stat().st_mtime
    dt = datetime.fromtimestamp(mtime)
    return dt.strftime("%Y-%m-%d %I:%M %p")


def print_sessions(files_and_messages, label=None):
    if label:
        print(f"[auto-escalated to {label}]\n")
    print(f"Found {len(files_and_messages)} session(s)\n")
    for filepath, messages in files_and_messages:
        timestamp = format_file_time(filepath)
        session_id = filepath.stem[:8]
        print(f"--- Session {session_id}... ({timestamp}) ---")
        if messages:
            for i, msg in enumerate(messages, 1):
                display = msg[:200] + "..." if len(msg) > 200 else msg
                print(f"  {i}. {display}")
        else:
            print("  (no user messages found)")
        print()


def main():
    parser = argparse.ArgumentParser(description="Summarize recent Claude Code sessions")
    parser.add_argument("--hours", type=float, default=None, help="Look back N hours")
    parser.add_argument("--files", type=int, default=None, help="Max number of files to read")
    args = parser.parse_args()

    # Explicit mode: user specified --hours or --files, no auto-escalation
    if args.hours is not None or args.files is not None:
        hours = args.hours if args.files is None else None
        files = get_recent_files(hours=hours, max_files=args.files)

        if not files:
            print(f"No conversation files found.")
            return

        pairs = [(f, extract_user_messages(f)) for f in files]
        print_sessions(pairs)
        return

    # Auto mode: escalate until substantive content is found
    # skip_active=True excludes the currently-running session file
    escalation_steps = [
        ("--hours 2",  dict(hours=2,    skip_active=True)),
        ("--hours 8",  dict(hours=8,    skip_active=True)),
        ("--files 3",  dict(max_files=3, skip_active=True)),
        ("--files 5",  dict(max_files=5, skip_active=True)),
        ("--files 10", dict(max_files=10, skip_active=True)),
    ]

    prev_label = None
    for label, kwargs in escalation_steps:
        files = get_recent_files(**kwargs)
        if not files:
            continue

        pairs = [(f, extract_user_messages(f)) for f in files]
        if is_substantive(pairs):
            print_sessions(pairs, label=label if prev_label is not None else None)
            return

        prev_label = label

    # Fell through all steps — print whatever we have
    if pairs:
        print_sessions(pairs, label="--files 10 (no substantive content found)")
    else:
        print("No conversation files found.")


if __name__ == "__main__":
    main()
