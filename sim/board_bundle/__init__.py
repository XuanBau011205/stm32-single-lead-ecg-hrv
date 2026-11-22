"""Board bundle package — extracts and loads P05 hardware manifest from Altium Gate G2 artifacts."""

import json
from pathlib import Path
from typing import Any, Dict

DEFAULT_REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parent / "board_manifest.json"


def load_manifest(rebuild: bool = False, repo_root: Path = DEFAULT_REPO_ROOT) -> Dict[str, Any]:
    """Load `board_manifest.json`, extracting it automatically if missing or `rebuild=True`."""
    if rebuild or not DEFAULT_MANIFEST_PATH.exists():
        from .extract_manifest import build_manifest

        return build_manifest(repo_root=repo_root, output_path=DEFAULT_MANIFEST_PATH)
    return json.loads(DEFAULT_MANIFEST_PATH.read_text(encoding="utf-8"))


__all__ = ["load_manifest", "DEFAULT_MANIFEST_PATH", "DEFAULT_REPO_ROOT"]
