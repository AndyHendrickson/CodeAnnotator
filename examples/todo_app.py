"""A tiny command-line todo list used as annotation demo material."""

import json
import os
from datetime import date

STORE = os.path.expanduser("~/.todo.json")
PRIORITIES = {"high": 0, "normal": 1, "low": 2}


def load_tasks():
    """Read the task list from disk, tolerating a missing store."""
    if not os.path.isfile(STORE):
        return []
    with open(STORE, encoding="utf-8") as f:
        return json.load(f)


def save_tasks(tasks):
    with open(STORE, "w", encoding="utf-8") as f:
        json.dump(tasks, f, indent=2)


def add_task(tasks, title, priority="normal"):
    if priority not in PRIORITIES:
        raise ValueError(f"unknown priority: {priority!r}")
    tasks.append({
        "title": title,
        "priority": priority,
        "created": date.today().isoformat(),
        "done": False,
    })
    return tasks


def pending(tasks):
    """Open tasks, most urgent first, oldest first within a priority."""
    todo = [t for t in tasks if not t["done"]]
    return sorted(todo, key=lambda t: (PRIORITIES[t["priority"]], t["created"]))


def main():
    tasks = load_tasks()
    add_task(tasks, "water the plants", "low")
    add_task(tasks, "ship the release", "high")
    for i, task in enumerate(pending(tasks), 1):
        print(f"{i}. [{task['priority']}] {task['title']}")
    save_tasks(tasks)


if __name__ == "__main__":
    main()
