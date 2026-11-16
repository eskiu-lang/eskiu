"""docs_config.py: the paths and release constants shared by build-docs.py and its modules."""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Shown in the shared top bar (kept in step with the homepage). Bump per release.
VERSION = "v0.15.1"
GH = "https://github.com/eskiu-lang/eskiu"
