#!/usr/bin/env python3
"""Summarize an Instruments Time Profiler trace by function (docs/dev/compile-speed.md).

  xcrun xctrace record --template 'Time Profiler' --output t.trace --launch -- eskiuc f.esk -c -o f.o
  xcrun xctrace export --input t.trace \
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > tp.xml
  tools/xctrace_top.py tp.xml [more.xml ...] [N] [--under NAME]

Prints the N functions with the most samples, inclusive (on the stack) and self (leaf).
--under NAME counts only the samples whose stack holds a frame containing NAME.
"""
import sys
import xml.etree.ElementTree as ET
from collections import Counter


def main():
    args = [a for a in sys.argv[1:]]
    under = None
    if "--under" in args:
        i = args.index("--under")
        under = args[i + 1]
        del args[i:i + 2]
    paths = [a for a in args if a.endswith(".xml")]
    rest = [a for a in args if not a.endswith(".xml")]
    top = int(rest[0]) if rest else 40
    incl, self_ = Counter(), Counter()
    total = 0
    for path in paths:
        total += summarize(path, under, incl, self_)
    print(f"samples: {total}")
    print("--- inclusive ---")
    for n, c in incl.most_common(top):
        print(f"{c:6d} {100.0 * c / max(total, 1):5.1f}%  {n[:150]}")
    print("--- self ---")
    for n, c in self_.most_common(top):
        print(f"{c:6d} {100.0 * c / max(total, 1):5.1f}%  {n[:150]}")


def summarize(path, under, incl, self_):
    frames = {}
    stacks = {}
    total = 0
    for _, row in ET.iterparse(path):
        if row.tag != "row":
            continue
        bt = row.find("backtrace")
        if bt is None:
            bt = row.find("tagged-backtrace")
        if bt is None:
            continue
        if "ref" in bt.attrib:
            names = stacks.get(bt.attrib["ref"], [])
        else:
            names = []
            for fr in bt.findall("frame"):
                if "ref" in fr.attrib:
                    names.append(frames.get(fr.attrib["ref"], "?"))
                else:
                    n = fr.attrib.get("name", "?")
                    frames[fr.attrib["id"]] = n
                    names.append(n)
            stacks[bt.attrib.get("id")] = names
        if under and not any(under in n for n in names):
            row.clear()
            continue
        total += 1
        if names:
            self_[names[0]] += 1
        for n in set(names):
            incl[n] += 1
        row.clear()
    return total


if __name__ == "__main__":
    main()
